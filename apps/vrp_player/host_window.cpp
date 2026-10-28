#include "host_window.hpp"
#include "desktop_ui.hpp"
#include "hud_paint.hpp"
#include "i18n.hpp"
#include "x11_direct_probe.hpp"

#include "audio/audio_player.hpp"
#include "audio/pulse_monitor.hpp"
#include "common.hpp"
#include "input/gamepad.hpp"
#include "input/pad_filter.hpp"
#include "scene/fsr1_upscaler.hpp"
#include "scene/scene_renderer.hpp"
#include "ui/vr_menu.hpp"
#include "video/bc7_thumbs.hpp"
#include "video/cuda_nv12_texture.hpp"
#include "video/vaapi_nv12_texture.hpp"
#include "video/video_decoder.hpp"
#include "xr/xr_vulkan_app.hpp"
#include "youtube_session.hpp"
#include "capture/kde_virtual_display.hpp"
#include "capture/kwin_window_placer.hpp"
#if defined(VRP_HAS_PIPEWIRE)
#include "capture/pipewire_capture.hpp"
#include "video/pipewire_dma_texture.hpp"
#endif

#include <QCoreApplication>

#include <fstream>
#include <QCoreApplication>
#include <QEventLoop>
#include <QImage>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFormLayout>
#include <QTextDocument>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>
#include <QMenu>
#include <QMenuBar>
#include <QAction>
#include <QActionGroup>
#include <QRadioButton>
#include <QWidgetAction>
#include <QDialog>
#include <QDialogButtonBox>
#include <QIcon>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProcess>
#include <QSplitter>
#include <QStandardPaths>
#include <QThread>
#include <QDateTime>
#include <QElapsedTimer>
#include <QTimer>
#include <QTimeZone>
#include <QRect>
#include <QScreen>
#include <QWindow>
#include <unistd.h>
#if defined(VRP_HAS_QT_GUI_PRIVATE)
#include <QtGui/qpa/qplatformwindow_p.h>
#include <dlfcn.h>
#endif
#include <QCloseEvent>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QSet>
#include <QTextStream>
#include <QVBoxLayout>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMap>
#include <QProcess>
#include <QRegularExpression>

#include <chrono>
#include <thread>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/un.h>

HostWindow::HostWindow(vrp::PlayerOptions opt, QString shader_dir, QWidget* parent)
    : QMainWindow(parent), opt_(std::move(opt)), shader_dir_(std::move(shader_dir)) {
  loadAppConf();
  runtime_json_ = findRuntimeJson();
  if (!runtime_json_.isEmpty()) {
    qputenv("XR_RUNTIME_JSON", runtime_json_.toUtf8());
  }
  buildUi();
  if (!opt_.video_path.empty() && !opt_.clear_only) {
    vrp::VideoDecoder probe;
    if (probe.open(opt_.video_path)) {
      reportVideoOpen(probe.info());
    } else {
      reportVideoOpen(probe.info());
    }
  }
  monado_ = new QProcess(this);
  monado_->setProcessChannelMode(QProcess::MergedChannels);
  connect(monado_, &QProcess::readyReadStandardOutput, this, &HostWindow::onMonadoOutput);
  connect(monado_, &QProcess::finished, this, &HostWindow::onMonadoFinished);
  // Do not start Monado at launch — start on HMD connect, stop on disconnect (lighter idle/exit).
  monado_label_->setText(tr("Idle"));
  setHealth("Wait", usingWivrn() ? QStringLiteral("Waiting for headset")
                                 : QStringLiteral("Waiting for USB HMD"));
  loadMonadoModeTable();
  if (!usingWivrn()) {
    QTimer::singleShot(400, this, [this] { maybeAutoConnectHeadset(); });
  }
}

HostWindow::~HostWindow() {
  shutting_down_.store(true);
  thumb_cancel_.store(true);
  if (yt_session_) yt_session_->shutdown();
  destroyVirtualForLeaveLive();
  if (!shutdownXrSession(10000, true)) {
    VRP_ERR("XR thread did not exit in destructor — detaching (last resort)");
    if (xr_thread_.joinable()) xr_thread_.detach();
  }
  if (thumb_thread_.joinable()) {
    thumb_thread_.join();
  }
  QCoreApplication::removePostedEvents(this);
}

void HostWindow::closeEvent(QCloseEvent* event) {
  saveWindowGeometry();
  shutting_down_.store(true);
  thumb_cancel_.store(true);
  if (yt_session_) yt_session_->shutdown();
  destroyVirtualForLeaveLive();
  if (!shutdownXrSession(10000, true)) {
    appendLog(tr("XR thread hung — detach"));
    VRP_ERR("XR thread join timed out on close — detaching");
    if (xr_thread_.joinable()) xr_thread_.detach();
  }
  if (thumb_thread_.joinable()) {
    thumb_cancel_.store(true);
    thumb_thread_.join();
  }
  event->accept();
  QCoreApplication::quit();
}

void HostWindow::signalXrStop() {
  xr_stop_.store(true);
  std::lock_guard<std::mutex> lock(xr_app_mu_);
  if (xr_app_) xr_app_->request_exit();
}

bool HostWindow::shutdownXrSession(int timeout_ms, bool stop_monado) {
  signalXrStop();
  if (stop_monado && !monado_external_) stopMonadoProcess();

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 0);
  while (xr_running_.load() && std::chrono::steady_clock::now() < deadline) {
    QThread::msleep(40);
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 40);
  }

  if (xr_thread_.joinable()) {
    if (!xr_running_.load()) {
      xr_thread_.join();
      return true;
    }
    return false;
  }
  xr_running_.store(false);
  return true;
}

bool HostWindow::usingWivrn() const { return runtime_kind_.load() == 1; }

bool HostWindow::ensureMonadoReady() {
  if (usingWivrn()) {
    if (!isWivrnServerLive()) startOwnedWivrn(/*allow_external_adopt=*/true);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!isWivrnServerLive() && std::chrono::steady_clock::now() < deadline) {
      QThread::msleep(100);
      QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);
    }
    if (!isWivrnServerLive()) {
      setHealth("Error", "WiVRn not responding");
      appendLog(tr("WiVRn not responding"));
      return false;
    }
    if (!runtime_json_.isEmpty()) qputenv("XR_RUNTIME_JSON", runtime_json_.toUtf8());
    return true;
  }
  if (isMonadoIpcLive()) {
    if (!monado_external_ && (!monado_ || monado_->state() == QProcess::NotRunning)) {
      (void)adoptExternalMonado();
    }
    return true;
  }
  startMonado();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (!isMonadoIpcLive() && std::chrono::steady_clock::now() < deadline) {
    QThread::msleep(100);
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);
  }
  if (!isMonadoIpcLive()) {
    setHealth("Error", "Monado not responding");
    appendLog(tr("Monado IPC timed out"));
    return false;
  }
  return true;
}

int HostWindow::countConnectedHmds() const { return static_cast<int>(listConnectedHmds().size()); }

std::vector<UsbHmdInfo> HostWindow::listConnectedHmds() const {
  struct Known {
    quint16 vid;
    quint16 pid;
    const char* name;
  };
  static const Known kHmds[] = {
      {0x054c, 0x0ee8, "PlayStation VR2"},
      {0x054c, 0x09af, "PlayStation VR"},
      {0x054c, 0x0cde, "PS VR2 (related)"},
      {0x28de, 0x2300, "Valve Index"},
      {0x28de, 0x2101, "Valve Index"},
      {0x0bb4, 0x2c87, "HTC Vive"},
      {0x0bb4, 0x0309, "HTC Vive Pro"},
      {0x0bb4, 0x030e, "HTC Vive Cosmos"},
      {0x2833, 0x0051, "Oculus Rift S"},
      {0x2833, 0x0031, "Oculus Rift CV1"},
      {0x2833, 0x0186, "Meta Quest (Link)"},
      {0x2833, 0x0183, "Quest 2"},
      {0x045e, 0x0659, "HP Reverb / WMR"},
      {0x045e, 0x03f4, "Samsung Odyssey+ / WMR"},
      {0x3318, 0x0424, "Xreal Air"},
      {0x3318, 0x0428, "Xreal Air"},
  };

  std::vector<UsbHmdInfo> out;
  const QDir usb("/sys/bus/usb/devices");
  for (const QString& name : usb.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
    if (name.contains(':')) continue;
    const QString base = usb.filePath(name);
    QFile vend(base + "/idVendor");
    QFile prod(base + "/idProduct");
    if (!vend.open(QIODevice::ReadOnly) || !prod.open(QIODevice::ReadOnly)) continue;
    bool ok_v = false, ok_p = false;
    const quint16 vid = vend.readAll().trimmed().toUShort(&ok_v, 16);
    const quint16 pid = prod.readAll().trimmed().toUShort(&ok_p, 16);
    if (!ok_v || !ok_p) continue;
    for (const auto& id : kHmds) {
      if (id.vid == vid && id.pid == pid) {
        UsbHmdInfo info;
        info.sysfs_name = name;
        info.vid = vid;
        info.pid = pid;
        info.label = QStringLiteral("%1 (%2)").arg(QString::fromUtf8(id.name), name);
        out.push_back(info);
        break;
      }
    }
  }
  return out;
}

bool HostWindow::psvr2UsbPresent() const {
  for (const auto& h : listConnectedHmds()) {
    if (h.vid == 0x054c && h.pid == 0x0ee8) return true;
  }
  return false;
}

bool HostWindow::psvr2PanelReady() const {
  const QDir drm(QStringLiteral("/sys/class/drm"));
  if (!drm.exists()) return false;
  for (const QString& name : drm.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
    if (!name.contains(QLatin1Char('-'))) continue;
    const QString base = drm.filePath(name);
    QFile status(base + QStringLiteral("/status"));
    if (!status.open(QIODevice::ReadOnly)) continue;
    if (status.readAll().trimmed() != "connected") continue;

    QFile modes(base + QStringLiteral("/modes"));
    if (!modes.open(QIODevice::ReadOnly)) continue;
    bool mode_ok = false;
    const QString text = QString::fromUtf8(modes.readAll());
    for (const QString& line : text.split(QLatin1Char('\n'))) {
      if (line.startsWith(QStringLiteral("4000x2040"))) {
        mode_ok = true;
        break;
      }
    }
    if (!mode_ok) continue;

    QFile edid(base + QStringLiteral("/edid"));
    if (!edid.open(QIODevice::ReadOnly)) return true;
    const QByteArray raw = edid.read(128);
    if (raw.size() < 10) return true;
    const unsigned id = (static_cast<unsigned char>(raw[8]) << 8) |
                        static_cast<unsigned char>(raw[9]);
    const char mfg[4] = {static_cast<char>(((id >> 10) & 31) + 'A' - 1),
                         static_cast<char>(((id >> 5) & 31) + 'A' - 1),
                         static_cast<char>((id & 31) + 'A' - 1), '\0'};
    if (QLatin1String(mfg) == QLatin1String("SNY")) return true;
  }
  return false;
}

bool HostWindow::deferUntilPsvr2PanelReady() {
  if (!psvr2UsbPresent()) {
    psvr2_display_wait_logged_ = false;
    psvr2_restart_after_panel_ = false;
    return false;
  }
  if (!psvr2PanelReady()) {
    if (isMonadoIpcLive() || (monado_ && monado_->state() != QProcess::NotRunning)) {
      psvr2_restart_after_panel_ = true;
    }
    if (!psvr2_display_wait_logged_) {
      psvr2_display_wait_logged_ = true;
      appendLog(tr("PSVR2 display not ready — waiting"));
    }
    setHealth("Wait", "Waiting for PSVR2 display");
    if (!user_suppressed_auto_connect_) {
      QTimer::singleShot(500, this, [this] { maybeAutoConnectHeadset(); });
    }
    return true;
  }

  psvr2_display_wait_logged_ = false;
  return false;
}

bool HostWindow::connectedHmdUsesDrmLease() const {
  if (usingWivrn()) return false;
  bool any = false;
  for (const auto& h : listConnectedHmds()) {
    if (h.vid == 0x2833 && (h.pid == 0x0186 || h.pid == 0x0183)) continue;
    if (h.vid == 0x3318) continue;
    any = true;
    break;
  }
  return any;
}

namespace {

bool leaseTextHas(const DrmLeaseConnectorInfo& connector, const char* needle) {
  const QString n = QString::fromUtf8(needle);
  return QString::fromStdString(connector.name).contains(n, Qt::CaseInsensitive) ||
         QString::fromStdString(connector.description).contains(n, Qt::CaseInsensitive);
}

QString leaseListText(const DrmLeaseProbeResult& probe) {
  QStringList parts;
  for (const auto& connector : probe.connectors) {
    parts << QStringLiteral("%1 (%2)")
                 .arg(QString::fromStdString(connector.name),
                      QString::fromStdString(connector.description));
  }
  return parts.join(QStringLiteral(", "));
}

// AppRun prepends $APPDIR/usr/lib so the bundled LGPL FFmpeg loads. Host tools
// (monado-service, wivrn-server, vulkaninfo) must keep the system library
// search path; Ubuntu libs there break Tumbleweed's OpenSSL and SDL.
QProcessEnvironment hostToolEnvironment() {
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  const QByteArray appdir = qgetenv("APPDIR");
  if (appdir.isEmpty()) return env;
  const QString bundled = QString::fromLocal8Bit(appdir) + QStringLiteral("/usr/lib");
  const QString key = QStringLiteral("LD_LIBRARY_PATH");
  if (!env.contains(key)) return env;
  const QStringList parts = env.value(key).split(QLatin1Char(':'), Qt::SkipEmptyParts);
  QStringList kept;
  kept.reserve(parts.size());
  for (const QString& part : parts) {
    const QString trimmed = part.endsWith(QLatin1Char('/')) ? part.chopped(1) : part;
    if (trimmed == bundled) continue;
    kept.push_back(part);
  }
  if (kept.isEmpty()) {
    env.remove(key);
  } else {
    env.insert(key, kept.join(QLatin1Char(':')));
  }
  return env;
}

}  // namespace

bool HostWindow::displayLeaseMatches(const DrmLeaseProbeResult& probe) const {
  if (probe.connectors.empty()) return false;
  bool want_psvr2 = false;
  bool want_psvr1 = false;
  for (const auto& h : listConnectedHmds()) {
    if (h.vid == 0x054c && (h.pid == 0x0ee8 || h.pid == 0x0cde)) want_psvr2 = true;
    if (h.vid == 0x054c && h.pid == 0x09af) want_psvr1 = true;
  }
  if (want_psvr2) {
    for (const auto& connector : probe.connectors) {
      if (leaseTextHas(connector, "PS VR2") || leaseTextHas(connector, "4000x2040")) return true;
    }
    return false;
  }
  if (want_psvr1) {
    for (const auto& connector : probe.connectors) {
      const bool vr =
          leaseTextHas(connector, "PS VR") || leaseTextHas(connector, "PlayStation VR");
      if (vr && !leaseTextHas(connector, "PS VR2")) return true;
    }
    return false;
  }
  return true;
}

void HostWindow::noteDisplayEnvironment(bool x11) {
  if (display_env_logged_) return;
  display_env_logged_ = true;
  appendLog(x11 ? tr("Display environment: X11") : tr("Display environment: Wayland"));
}

bool HostWindow::holdDirectDisplay(bool x11, bool ready, bool list_empty, const QString& listed) {
  display_direct_is_x11_ = x11;
  if (!ready) {
    display_lease_ready_logged_ = false;
    if (isMonadoIpcLive() || (monado_ && monado_->state() != QProcess::NotRunning)) {
      display_lease_restart_ = true;
    }
    if (!display_lease_wait_logged_) {
      display_lease_wait_logged_ = true;
      if (list_empty) {
        appendLog(x11 ? tr("X11 direct display not ready — waiting")
                      : tr("Display lease not ready — waiting"));
      } else {
        appendLog((x11 ? tr("X11 direct display: %1") : tr("Display lease: %1")).arg(listed));
      }
    }
    setHealth("Wait", x11 ? "Waiting for X11 direct display" : "Waiting for display lease");
    if (!user_suppressed_auto_connect_) {
      QTimer::singleShot(500, this, [this] { maybeAutoConnectHeadset(); });
    }
    return true;
  }

  display_lease_wait_logged_ = false;
  if (!display_lease_ready_logged_) {
    display_lease_ready_logged_ = true;
    appendLog((x11 ? tr("X11 direct display: %1") : tr("Display lease: %1")).arg(listed));
  }
  return false;
}

bool HostWindow::deferUntilDisplayLeaseReady() {
  if (!connectedHmdUsesDrmLease()) {
    display_lease_wait_logged_ = false;
    display_lease_ready_logged_ = false;
    display_lease_restart_ = false;
    display_direct_is_x11_ = false;
    display_env_logged_ = false;
    return false;
  }
  const DrmLeaseProbeResult probe = probeDrmLeaseConnectors();
  if (probe.kind == DrmLeaseProbeKind::Ok) {
    noteDisplayEnvironment(false);
    return holdDirectDisplay(false, displayLeaseMatches(probe), probe.connectors.empty(),
                             leaseListText(probe));
  }
  // Wayland is connected but has no lease protocol. Monado then opens a Wayland
  // window, so an X11 list would not be the one it reads.
  if (probe.kind == DrmLeaseProbeKind::NoProtocol) {
    noteDisplayEnvironment(false);
    display_lease_wait_logged_ = false;
    display_lease_ready_logged_ = false;
    display_lease_restart_ = false;
    display_direct_is_x11_ = false;
    return false;
  }

  const X11DirectProbeResult x11 = probeX11DirectOutputs();
  if (x11.kind != X11DirectProbeKind::Ok) {
    display_lease_wait_logged_ = false;
    display_lease_ready_logged_ = false;
    display_lease_restart_ = false;
    display_direct_is_x11_ = false;
    return false;
  }
  DrmLeaseProbeResult as_lease;
  as_lease.kind = DrmLeaseProbeKind::Ok;
  as_lease.connectors = x11.outputs;
  noteDisplayEnvironment(true);
  return holdDirectDisplay(true, displayLeaseMatches(as_lease), x11.outputs.empty(),
                           leaseListText(as_lease));
}

bool HostWindow::restartMonadoAfterDisplayLatch() {
  if (!psvr2_restart_after_panel_ && !display_lease_restart_) return false;
  const bool up = isMonadoIpcLive() || (monado_ && monado_->state() != QProcess::NotRunning);
  const bool lease = display_lease_restart_;
  const bool x11 = display_direct_is_x11_;
  psvr2_restart_after_panel_ = false;
  display_lease_restart_ = false;
  if (!up) return false;
  appendLog(lease ? (x11 ? tr("X11 direct display ready — restart Monado")
                         : tr("Display lease ready — restart Monado"))
                  : tr("PSVR2 display ready — restart Monado"));
  restartOwnedMonadoForHz();
  return true;
}

void HostWindow::notePsvr2DisplayGrabbed(const QString& line) {
  if (psvr2_compositor_ready_ms_ > 0 || !psvr2UsbPresent()) return;
  const bool grabbed = line.contains(QStringLiteral("Started vblank event thread")) ||
                       line.contains(QStringLiteral("Not using vblank event thread")) ||
                       line.contains(QStringLiteral("Compositor probably missed frame"));
  if (!grabbed) return;
  psvr2_compositor_ready_ms_ = QDateTime::currentMSecsSinceEpoch();
  appendLog(tr("PSVR2 display grabbed"));
}

bool HostWindow::deferUntilPsvr2CompositorSettled() {
  // An already-running external compositor grabbed the display before we connected.
  if (!psvr2UsbPresent() || monado_external_) {
    psvr2_grab_wait_logged_ = false;
    return false;
  }
  // Let NVIDIA finish the modeset before the app's vkCreateInstance.
  constexpr qint64 kSettleMs = 2000;
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  if (psvr2_compositor_ready_ms_ <= 0 || now - psvr2_compositor_ready_ms_ < kSettleMs) {
    if (!psvr2_grab_wait_logged_) {
      psvr2_grab_wait_logged_ = true;
      appendLog(tr("Waiting for Monado to grab the PSVR2 display"));
    }
    setHealth("Wait", psvr2_compositor_ready_ms_ > 0 ? "PSVR2 display grabbed"
                                                     : "Waiting for PSVR2 display grab");
    if (!user_suppressed_auto_connect_) {
      QTimer::singleShot(400, this, [this] { maybeAutoConnectHeadset(); });
    }
    return true;
  }
  psvr2_grab_wait_logged_ = false;
  return false;
}

bool HostWindow::isPlaceholderHmdName(const QString& system_name) {
  const QString n = system_name.trimmed().toLower();
  if (n.isEmpty() || n == QStringLiteral("(unknown hmd)") || n == QStringLiteral("未検出")) return true;
  return n.contains(QStringLiteral("simulated")) || n.contains(QStringLiteral("qwerty")) ||
         n.contains(QStringLiteral("null hmd")) || n.contains(QStringLiteral("remote"));
}

void HostWindow::maybeAutoConnectHeadset() {
  if (shutting_down_.load()) return;
  if (usingWivrn()) return;

  // While XR is up, keep watching USB. Monado often does not emit LOSS_PENDING on unplug.
  if (xr_running_.load()) {
    const int n = countConnectedHmds();
    last_usb_hmd_count_ = n;
    if (n == 0) {
      appendLog(tr("USB: HMD unplugged — stop session"));
      disconnectHeadsetSession(/*resume_auto_poll=*/!user_suppressed_auto_connect_);
      return;
    }
    QTimer::singleShot(500, this, [this] { maybeAutoConnectHeadset(); });
    return;
  }

  if (user_suppressed_auto_connect_) return;
  if (monado_auto_connect_in_progress_) return;
  // connectHeadset disables connect UI before xr_running flips — avoid a second connect.
  if (!connect_ui_enabled_) return;

  const int n = countConnectedHmds();
  const int prev = last_usb_hmd_count_;
  last_usb_hmd_count_ = n;

  // HMD unplugged before/after session: release owned compositor (keep manually-started
  // Monado if USB never showed an HMD — debug「Monado 起動」).
  if (n == 0) {
    psvr2_display_wait_logged_ = false;
    psvr2_restart_after_panel_ = false;
    display_lease_wait_logged_ = false;
    display_lease_ready_logged_ = false;
    display_lease_restart_ = false;
    display_direct_is_x11_ = false;
    display_env_logged_ = false;
    psvr2_grab_wait_logged_ = false;
    psvr2_compositor_ready_ms_ = 0;
    if (prev == 1 && !monado_external_ && monado_ && monado_->state() != QProcess::NotRunning) {
      appendLog(tr("USB HMD removed — stop Monado"));
      stopMonadoProcess();
      monado_label_->setText(tr("Idle"));
    }
    QTimer::singleShot(500, this, [this] { maybeAutoConnectHeadset(); });
    return;
  }

  if (n >= 2) {
    if (!monado_multi_hmd_logged_) {
      monado_multi_hmd_logged_ = true;
      appendLog(tr("Auto-connect skipped: %1 HMDs").arg(n));
      setHealth("Wait", "Multiple HMDs — connect manually");
    }
    QTimer::singleShot(500, this, [this] { maybeAutoConnectHeadset(); });
    return;
  }
  monado_multi_hmd_logged_ = false;

  if (QDateTime::currentMSecsSinceEpoch() < monado_auto_connect_cooldown_ms_) {
    QTimer::singleShot(500, this, [this] { maybeAutoConnectHeadset(); });
    return;
  }

  // PSVR2: do not start Monado until the panel lists 4000×2040. The distortion mesh
  // is read once at device create and cannot be rebuilt later.
  if (deferUntilPsvr2PanelReady()) return;
  // Wayland direct mode reads the compositor lease list once. An empty list
  // falls back to a desktop window for the life of that process.
  if (deferUntilDisplayLeaseReady()) return;
  if (restartMonadoAfterDisplayLatch()) return;

  // Start Monado only when a single USB HMD is present (probe sees the real device).
  if (!isMonadoIpcLive()) {
    // Restart just spawned our process; the socket appears after USB init.
    // Calling start again only logs "already running" once per pending timer.
    if (monado_ && monado_->state() != QProcess::NotRunning) {
      QTimer::singleShot(400, this, [this] { maybeAutoConnectHeadset(); });
      return;
    }
    setHealth("Wait", "Starting Monado…");
    resolvePreferredDisplayMode();
    if (monado_hz_require_owned_ || monado_desired_mode_ >= 0) {
      startOwnedMonado(/*allow_external_adopt=*/false);
    } else {
      startMonado();
    }
    return;
  }

  // A leftover compositor was not started with this hz=. Replace it before connecting.
  resolvePreferredDisplayMode();
  if (monado_desired_mode_ >= 0 &&
      (monado_hz_require_owned_ || monado_external_ || !monado_ ||
       monado_->state() == QProcess::NotRunning)) {
    appendLog(tr("Auto-connect: replace external Monado for Hz"));
    restartOwnedMonadoForHz();
    return;
  }

  if (deferUntilPsvr2CompositorSettled()) return;

  monado_auto_connect_in_progress_ = true;
  connectHeadset();
}

void HostWindow::onAutoConnectXrReady(const QString& system_name) {
  if (monado_auto_connect_in_progress_) {
    if (isPlaceholderHmdName(system_name)) {
      monado_auto_connect_in_progress_ = false;
      monado_auto_connect_cooldown_ms_ = QDateTime::currentMSecsSinceEpoch() + 3000;
      if (!monado_simulated_reject_logged_) {
        monado_simulated_reject_logged_ = true;
        appendLog(tr("Auto-connect: %1 is not a real HMD").arg(system_name.isEmpty() ? tr("(none)") : system_name));
      }
      QTimer::singleShot(0, this, [this] {
        disconnectHeadsetSession(/*resume_auto_poll=*/true);
      });
      return;
    }

    monado_auto_connect_in_progress_ = false;
    monado_simulated_reject_logged_ = false;
  }

  // Keep USB watch alive while the session runs (Monado may not signal unplug).
  QTimer::singleShot(500, this, [this] { maybeAutoConnectHeadset(); });
}

void HostWindow::syncDisconnectedUi(const QString& detail) {
  setSessionChrome(false);
  last_xr_system_.clear();
  hmd_label_->setText(tr("HMD: none"));
  sync_lag_hold_until_ms_ = 0;
  if (sync_status_label_) {
    sync_status_label_->setText(QStringLiteral("-"));
    sync_status_label_->setStyleSheet({});
  }
  setHealth("Wait", detail);
  // Drop HMD HDMI route; Pulse may still list PSVR2 briefly after unplug.
  refreshAudioDevices(/*prefer_hmd=*/false);
  rebuildConnectDeviceMenu();
}

void HostWindow::showControlsHelp() {
  // Startup-only guide: stays until B/× (no countdown auto-close).
  controls_help_.store(true);
  requestHudPaint();
}

void HostWindow::hideControlsHelp() {
  if (!controls_help_.exchange(false)) return;
  requestHudPaint();
}

void HostWindow::showHudToast(const QString& text, int duration_ms) {
  {
    std::lock_guard<std::mutex> lock(hud_toast_mu_);
    hud_toast_ = text;
    hud_toast_until_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(duration_ms);
  }
  hud_toast_active_.store(true);
  requestHudPaint();
}

bool HostWindow::hudToastActive() const {
  if (!hud_toast_active_.load()) return false;
  std::lock_guard<std::mutex> lock(hud_toast_mu_);
  return std::chrono::steady_clock::now() < hud_toast_until_ && !hud_toast_.isEmpty();
}

void HostWindow::requestHudPaint() {
  hud_paint_req_.fetch_add(1, std::memory_order_relaxed);
  bool expected = false;
  if (!hud_paint_queued_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
  QMetaObject::invokeMethod(this, [this] { paintHudNow(); }, Qt::QueuedConnection);
}

void HostWindow::paintHudNow() {
  const uint64_t seen = hud_paint_req_.load(std::memory_order_acquire);
  struct Done {
    HostWindow* self;
    uint64_t seen;
    ~Done() {
      self->hud_paint_queued_.store(false, std::memory_order_release);
      if (self->hud_paint_req_.load(std::memory_order_acquire) != seen) self->requestHudPaint();
    }
  } done{this, seen};
  if (shutting_down_.load()) return;
  vrp::VrMenu* menu = nullptr;
  {
    std::lock_guard<std::mutex> lock(menu_mu_);
    menu = vr_menu_;
  }
  if (!menu) return;
  const auto snap = menu->snapshot();
  QString toast;
  bool toast_on = false;
  {
    std::lock_guard<std::mutex> lock(hud_toast_mu_);
    if (hud_toast_active_.load() && std::chrono::steady_clock::now() < hud_toast_until_ &&
        !hud_toast_.isEmpty()) {
      toast = hud_toast_;
      toast_on = true;
    } else if (hud_toast_active_.load()) {
      hud_toast_.clear();
      hud_toast_active_.store(false);
    }
  }
  const bool help = controls_help_.load();
  const int kind = help             ? 1
                   : snap.visible   ? 2
                   : snap.controls_visible ? 3
                   : snap.seek_bar_only    ? 4
                   : toast_on              ? 5
                                           : 0;
  if (kind == 0) {
    std::lock_guard<std::mutex> lock(hud_pixels_mu_);
    hud_verts_.clear();
    hud_pixels_kind_ = 0;
    hud_pixels_gen_ = ++hud_paint_serial_;
    hud_paint_done_.store(snap.gen);
    return;
  }
  const vrp::HudPaintResult hud =
      help     ? vrp::paint_controls_help(hud_w_, hud_h_, face_sony_.load(), menu_option_.load())
      : kind == 5 ? vrp::paint_hud_toast(toast, hud_w_, hud_h_)
                  : vrp::paint_vr_menu(snap, hud_w_, hud_h_, face_sony_.load(), menu_option_.load());
  QImage thumb = (!help && snap.visible) ? vrp::paint_file_thumb(snap) : QImage();
  if (!thumb.isNull() && thumb.format() != QImage::Format_RGBA8888) {
    thumb = thumb.convertToFormat(QImage::Format_RGBA8888);
  }
  {
    std::lock_guard<std::mutex> lock(hud_pixels_mu_);
    if (hud.atlas && hud.atlas_bytes > 0 && hud.atlas_gen != hud_atlas_gen_) {
      hud_pixels_.assign(hud.atlas, hud.atlas + hud.atlas_bytes);
      hud_atlas_w_ = hud.atlas_w;
      hud_atlas_h_ = hud.atlas_h;
      hud_atlas_gen_ = hud.atlas_gen;
    }
    hud_verts_ = hud.verts;
    hud_mesh_atlas_gen_ = hud.atlas_gen;
    hud_pixels_kind_ = kind;
    hud_pixels_gen_ = ++hud_paint_serial_;
    if (kind == 4) hud_painted_seek_sec_ = snap.position_sec;
    if (thumb.isNull() || thumb.width() <= 0 || thumb.height() <= 0) {
      thumb_pixels_.clear();
      thumb_w_ = thumb_h_ = 0;
    } else {
      thumb_w_ = thumb.width();
      thumb_h_ = thumb.height();
      thumb_pixels_.resize(static_cast<size_t>(thumb_w_ * thumb_h_ * 4));
      for (int y = 0; y < thumb_h_; ++y) {
        std::memcpy(thumb_pixels_.data() + static_cast<size_t>(y * thumb_w_ * 4), thumb.constScanLine(y),
                    static_cast<size_t>(thumb_w_ * 4));
      }
    }
    thumb_gen_ = hud_pixels_gen_;
  }
  hud_paint_done_.store(snap.gen);
}

void HostWindow::stopMonadoProcess() {
  if (monado_external_) {
    monado_external_ = false;
    return;
  }
  if (!monado_ || monado_->state() == QProcess::NotRunning) {
    clearStaleMonadoSocket();
    return;
  }
  monado_intentional_stop_ = true;
  monado_->terminate();
  if (!monado_->waitForFinished(1500)) {
    monado_->kill();
    monado_->waitForFinished(1000);
  }
  monado_intentional_stop_ = false;
  clearStaleMonadoSocket();
  psvr2_compositor_ready_ms_ = 0;
  psvr2_grab_wait_logged_ = false;
}

void HostWindow::stopSystemdMonadoForOwnedStart() {
  // Socket activation will respawn monado-service without our DESIRED_MODE unless stopped.
  QProcess ctl;
  ctl.setProcessEnvironment(hostToolEnvironment());
  ctl.start(QStringLiteral("systemctl"),
            {QStringLiteral("--user"), QStringLiteral("stop"), QStringLiteral("monado.socket"),
             QStringLiteral("monado.service")});
  if (ctl.waitForStarted(1000)) {
    ctl.waitForFinished(4000);
    if (ctl.exitCode() == 0) {
    }
  }
  // Best-effort: any leftover monado-service (not our QProcess child).
  QProcess killall;
  killall.setProcessEnvironment(hostToolEnvironment());
  killall.start(QStringLiteral("pkill"), {QStringLiteral("-x"), QStringLiteral("monado-service")});
  if (killall.waitForStarted(500)) killall.waitForFinished(2000);
  for (int i = 0; i < 50 && isMonadoIpcLive(); ++i) {
    QThread::msleep(40);
    clearStaleMonadoSocket();
  }
}

void HostWindow::restartOwnedMonadoForHz() {
  monado_hz_require_owned_ = true;
  monado_external_ = false;
  if (monado_ && monado_->state() != QProcess::NotRunning) {
    monado_intentional_stop_ = true;
    monado_->terminate();
    if (!monado_->waitForFinished(3000)) {
      monado_->kill();
      monado_->waitForFinished(1000);
    }
    monado_intentional_stop_ = false;
  }
  clearStaleMonadoSocket();
  stopSystemdMonadoForOwnedStart();
  startOwnedMonado(/*allow_external_adopt=*/false);
}

namespace {

/** Desktop fixed face and size. Without a desktop fixed font, keep a monospace face at the UI size. */
QFont makeLogTerminalFont() {
  QFont term = desktopFixedFont();
  if (term.family().isEmpty() || !QFontInfo(term).fixedPitch()) {
    term = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    term.setStyleHint(QFont::TypeWriter);
    term.setFixedPitch(true);
    if (!QFontInfo(term).fixedPitch()) {
      const char* fallbacks[] = {"Hack", "DejaVu Sans Mono", "Noto Sans Mono", "Source Code Pro",
                                 "monospace", nullptr};
      for (int i = 0; fallbacks[i]; ++i) {
        QFont trial(QString::fromUtf8(fallbacks[i]));
        trial.setStyleHint(QFont::TypeWriter);
        trial.setFixedPitch(true);
        if (QFontInfo(trial).fixedPitch()) {
          term = trial;
          break;
        }
      }
    }
    const QFont ui = desktopUiFont();
    if (ui.pointSizeF() > 0.0) term.setPointSizeF(ui.pointSizeF());
    else if (ui.pixelSize() > 0) term.setPixelSize(ui.pixelSize());
    else {
      const QFont small = QFontDatabase::systemFont(QFontDatabase::SmallestReadableFont);
      if (small.pointSizeF() > 0.0) term.setPointSizeF(small.pointSizeF());
      else if (small.pixelSize() > 0) term.setPixelSize(small.pixelSize());
    }
  }
  term.setStyleHint(QFont::TypeWriter);
  term.setFixedPitch(true);
  return term;
}

}  // namespace

void HostWindow::buildUi() {
  setWindowTitle(QStringLiteral("monaSphere"));
  buildMenus();

  auto* root = new QWidget(this);
  auto* layout = new QVBoxLayout(root);
  layout->setContentsMargins(6, 6, 6, 6);
  layout->setSpacing(6);

  auto* status = new QWidget(root);
  auto* form = new QFormLayout(status);
  form->setContentsMargins(0, 0, 0, 0);
  form->setHorizontalSpacing(12);
  form->setVerticalSpacing(6);
  form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
  form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

  // Hidden status holders (still updated by Monado / runtime paths).
  runtime_label_ = new QLabel(status);
  runtime_label_->hide();
  if (!runtime_json_.isEmpty()) runtime_label_->setText(runtime_json_);
  monado_label_ = new QLabel(tr("Stopped"), status);
  monado_label_->hide();

  health_label_ = new QLabel(tr("Wait"));
  sync_status_label_ = new QLabel(QStringLiteral("-"));
  sync_status_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  form->addRow(tr("Status"), health_label_);
  form->addRow(tr("Sync"), sync_status_label_);
  hmd_label_ = new QLabel(tr("HMD: none"));
  hmd_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  form->addRow(tr("HMD info"), hmd_label_);

  log_ = new QPlainTextEdit();
  log_->setReadOnly(true);
  log_->setMaximumBlockCount(4000);
  {
    const QFont term = makeLogTerminalFont();
    log_->setFont(term);
    if (log_->document()) log_->document()->setDefaultFont(term);
  }
  log_->setPlaceholderText(tr("Log"));

  auto* split = new QSplitter(Qt::Vertical);
  split->addWidget(status);
  split->addWidget(log_);
  split->setStretchFactor(0, 0);
  split->setStretchFactor(1, 1);
  split->setSizes({1, 10000});
  layout->addWidget(split);

  setCentralWidget(root);

  setSessionChrome(false);
  refreshAudioDevices();
  rebuildConnectDeviceMenu();
  QTimer* hmd_menu_timer = new QTimer(this);
  connect(hmd_menu_timer, &QTimer::timeout, this, [this] {
    if (!xr_running_.load()) rebuildConnectDeviceMenu();
    if (usingWivrn() && !audio_user_override_.load()) refreshAudioDevices(false);
  });
  hmd_menu_timer->start(2000);

  restoreWindowGeometry();
  loadAppConf();
  syncRuntimeRadios();
  if (usingWivrn()) refreshAudioDevices(false);
  rebuildFsrMenu();
}

namespace {

QString monasphereConfPath() {
  return QDir::homePath() + QStringLiteral("/.config/monasphere/monasphere.conf");
}

/** Parse zone.tab ISO-6709 coords: ±DDMMSS±DDDMMSS or ±DDMM±DDDMM. */
bool parseZoneTabCoords(const QByteArray& coord, float& lat_deg, float& lon_deg) {
  if (coord.size() < 5) return false;
  int i = 0;
  auto read_hem = [&](int& sign) -> bool {
    if (i >= coord.size()) return false;
    if (coord[i] == '+') {
      sign = 1;
      ++i;
      return true;
    }
    if (coord[i] == '-') {
      sign = -1;
      ++i;
      return true;
    }
    return false;
  };
  auto read_digits = [&](int n, int& out) -> bool {
    if (i + n > coord.size()) return false;
    int v = 0;
    for (int k = 0; k < n; ++k) {
      const char c = coord[i + k];
      if (c < '0' || c > '9') return false;
      v = v * 10 + (c - '0');
    }
    i += n;
    out = v;
    return true;
  };
  int lat_sign = 1, lon_sign = 1;
  if (!read_hem(lat_sign)) return false;
  const int lat_start = i;
  while (i < coord.size() && coord[i] >= '0' && coord[i] <= '9') ++i;
  const int lat_digits = i - lat_start;
  i = lat_start;
  int lat_d = 0, lat_m = 0, lat_s = 0;
  if (lat_digits == 4) {
    if (!read_digits(2, lat_d) || !read_digits(2, lat_m)) return false;
  } else if (lat_digits == 6) {
    if (!read_digits(2, lat_d) || !read_digits(2, lat_m) || !read_digits(2, lat_s)) return false;
  } else {
    return false;
  }
  if (!read_hem(lon_sign)) return false;
  const int lon_start = i;
  while (i < coord.size() && coord[i] >= '0' && coord[i] <= '9') ++i;
  const int lon_digits = i - lon_start;
  i = lon_start;
  int lon_d = 0, lon_m = 0, lon_s = 0;
  if (lon_digits == 5) {
    if (!read_digits(3, lon_d) || !read_digits(2, lon_m)) return false;
  } else if (lon_digits == 7) {
    if (!read_digits(3, lon_d) || !read_digits(2, lon_m) || !read_digits(2, lon_s)) return false;
  } else {
    return false;
  }
  lat_deg = lat_sign * (lat_d + lat_m / 60.f + lat_s / 3600.f);
  lon_deg = lon_sign * (lon_d + lon_m / 60.f + lon_s / 3600.f);
  return lat_deg >= -90.f && lat_deg <= 90.f && lon_deg >= -180.f && lon_deg <= 180.f;
}

/**
 * Observer on Earth from the host timezone (zone.tab reference city).
 * Celestial directions stay geocentric; only horizon tip uses latitude (sphere).
 */
bool observerFromHostTimezone(float& lat_deg, float& lon_deg, QString* zone_id_out = nullptr) {
  const QByteArray zid = QTimeZone::systemTimeZoneId();
  if (zone_id_out) *zone_id_out = QString::fromUtf8(zid);
  const QStringList tabs = {QStringLiteral("/usr/share/zoneinfo/zone1970.tab"),
                            QStringLiteral("/usr/share/zoneinfo/zone.tab")};
  for (const QString& path : tabs) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
    while (!f.atEnd()) {
      const QByteArray line = f.readLine().trimmed();
      if (line.isEmpty() || line.startsWith('#')) continue;
      const QList<QByteArray> cols = line.split('\t');
      if (cols.size() < 3) continue;
      if (cols[2] != zid) continue;
      if (parseZoneTabCoords(cols[1], lat_deg, lon_deg)) return true;
    }
  }
  // Fallback: longitude from current UTC offset; latitude unknown → equator.
  const QTimeZone tz = QTimeZone::systemTimeZone();
  if (!tz.isValid()) return false;
  const int off = tz.offsetFromUtc(QDateTime::currentDateTimeUtc());
  lon_deg = std::clamp(off / 240.f, -180.f, 180.f);  // 15°/h
  lat_deg = 0.f;
  return true;
}

// Insert keys that are absent. Existing lines stay as they are.
/** Rename legacy youtube_* conf keys → live_*; drop youtube_url; fix Virtual-* name. */
void migrateYoutubeConfKeysToLive() {
  const QString path = monasphereConfPath();
  QFile in(path);
  if (!in.open(QIODevice::ReadOnly | QIODevice::Text)) return;
  QStringList lines;
  QSet<QString> seen_new;
  bool changed = false;
  bool clear_token = false;
  static const QHash<QString, QString> rename{
      {QStringLiteral("youtube_capture"), QStringLiteral("live_capture")},
      {QStringLiteral("youtube_virtual"), QStringLiteral("live_virtual")},
      {QStringLiteral("youtube_virtual_size"), QStringLiteral("live_virtual_size")},
      {QStringLiteral("youtube_virtual_scale"), QStringLiteral("live_virtual_scale")},
      {QStringLiteral("youtube_output"), QStringLiteral("live_output")},
      {QStringLiteral("youtube_chrome_scale"), QStringLiteral("live_chrome_scale")},
      {QStringLiteral("youtube_cursor_scale"), QStringLiteral("live_cursor_scale")},
      {QStringLiteral("youtube_chrome_profile"), QStringLiteral("live_chrome_profile")},
      {QStringLiteral("youtube_restore_token"), QStringLiteral("live_restore_token")},
  };
  while (!in.atEnd()) {
    QString line = QString::fromUtf8(in.readLine()).trimmed();
    if (line.isEmpty()) continue;
    const int eq = line.indexOf(QLatin1Char('='));
    if (eq <= 0) {
      lines << line;
      continue;
    }
    const QString key = line.left(eq);
    QString val = line.mid(eq + 1);
    if (key == QLatin1String("youtube_url")) {
      changed = true;
      continue;
    }
    QString out_key = key;
    if (rename.contains(key)) {
      out_key = rename.value(key);
      changed = true;
    }
    if (out_key == QLatin1String("live_output") || out_key == QLatin1String("youtube_output")) {
      if (val.contains(QLatin1String("monaSphereYT"))) {
        val.replace(QStringLiteral("monaSphereYT"), QStringLiteral("monaSphere"));
        clear_token = true;
        changed = true;
      }
    }
    if (seen_new.contains(out_key)) {
      // Prefer already-migrated live_* over a second youtube_* copy.
      changed = true;
      continue;
    }
    seen_new.insert(out_key);
    lines << out_key + QLatin1Char('=') + val;
  }
  in.close();
  if (clear_token) {
    for (QString& line : lines) {
      if (line.startsWith(QStringLiteral("live_restore_token"))) {
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq > 0) {
          line = line.left(eq + 1);
          changed = true;
        }
      }
    }
  }
  if (!changed) return;
  QFile out(path);
  if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return;
  QTextStream ts(&out);
  for (const QString& line : lines) ts << line << '\n';
}

void ensureMonasphereConfKeys(const QHash<QString, QString>& defaults) {
  const QString path = monasphereConfPath();
  QDir().mkpath(QDir::homePath() + QStringLiteral("/.config/monasphere"));
  QStringList lines;
  QSet<QString> seen;
  QFile in(path);
  if (in.open(QIODevice::ReadOnly | QIODevice::Text)) {
    while (!in.atEnd()) {
      const QString line = QString::fromUtf8(in.readLine()).trimmed();
      if (line.isEmpty()) continue;
      lines << line;
      const int eq = line.indexOf(QLatin1Char('='));
      if (eq > 0) seen.insert(line.left(eq));
    }
    in.close();
  }
  bool added = lines.isEmpty() && !QFile::exists(path);
  for (auto it = defaults.cbegin(); it != defaults.cend(); ++it) {
    if (seen.contains(it.key())) continue;
    lines << it.key() + QLatin1Char('=') + it.value();
    added = true;
  }
  if (!added) return;
  QFile out(path);
  if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return;
  QTextStream ts(&out);
  for (const QString& line : lines) ts << line << '\n';
}

void setMonasphereConfKey(const QString& key, const QString& value) {
  ensureMonasphereConfKeys({{key, value}});
  const QString path = monasphereConfPath();
  QStringList lines;
  bool replaced = false;
  QFile in(path);
  if (in.open(QIODevice::ReadOnly | QIODevice::Text)) {
    while (!in.atEnd()) {
      const QString line = QString::fromUtf8(in.readLine()).trimmed();
      if (line.isEmpty()) continue;
      if (line.startsWith(key + QLatin1Char('='))) {
        lines << key + QLatin1Char('=') + value;
        replaced = true;
      } else {
        lines << line;
      }
    }
    in.close();
  }
  if (!replaced) lines << key + QLatin1Char('=') + value;
  QFile out(path);
  if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return;
  QTextStream ts(&out);
  for (const QString& line : lines) ts << line << '\n';
}

QString monasphereConfValue(const QString& key) {
  QFile in(monasphereConfPath());
  if (!in.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
  const QString prefix = key + QLatin1Char('=');
  while (!in.atEnd()) {
    const QString line = QString::fromUtf8(in.readLine()).trimmed();
    if (line.startsWith(prefix)) return line.mid(prefix.size());
  }
  return {};
}

// Read a value already stored by Qt. Does not create monaSphere.conf.
QString legacyQtValue(const QString& group, const QString& key) {
  const QStringList paths{
      QDir::homePath() + QStringLiteral("/.config/monasphere/monaSphere.conf"),
      QDir::homePath() + QStringLiteral("/.config/monaSphere/monaSphere.conf"),
  };
  const QString prefix = key + QLatin1Char('=');
  for (const QString& path : paths) {
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
    QString section;
    while (!in.atEnd()) {
      const QString line = QString::fromUtf8(in.readLine()).trimmed();
      if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
        section = line.mid(1, line.size() - 2);
        continue;
      }
      if (section != group || !line.startsWith(prefix)) continue;
      QString value = line.mid(prefix.size()).trimmed();
      if (value.size() >= 2 && value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"'))) {
        value = value.mid(1, value.size() - 2);
      }
      if (value.startsWith(QLatin1String("@ByteArray(")) && value.endsWith(QLatin1Char(')'))) {
        value = value.mid(11, value.size() - 12);
      }
      return value;
    }
  }
  return {};
}

QString expandLiveArgPath(QString a) {
  a = a.trimmed();
  if (a.startsWith(QStringLiteral("~/"))) return QDir::homePath() + a.mid(1);
  if (a == QLatin1String("~")) return QDir::homePath();
  return a;
}

/**
 * Conf args are `|`-separated. A field may also be shell-like argv
 * (`-L ~/core.so ~/rom.zip`) so QProcess gets real separate arguments.
 */
QStringList normalizeLiveExecArgs(const QStringList& fields) {
  QStringList out;
  for (const QString& field : fields) {
    const QString f = field.trimmed();
    if (f.isEmpty()) continue;
    const bool multi = f.contains(QLatin1Char(' ')) || f.contains(QLatin1Char('\t')) ||
                       f.contains(QLatin1Char('"')) || f.contains(QLatin1Char('\''));
    const QStringList toks = multi ? QProcess::splitCommand(f) : QStringList{f};
    for (const QString& t : toks) {
      const QString e = expandLiveArgPath(t);
      if (!e.isEmpty()) out.push_back(e);
    }
  }
  return out;
}

}  // namespace

void HostWindow::saveAppConf() const {
  setMonasphereConfKey(QStringLiteral("fsr"), QString::number(static_cast<int>(preferred_fsr_)));
  setMonasphereConfKey(QStringLiteral("runtime"), usingWivrn() ? QStringLiteral("wivrn") : QStringLiteral("monado"));
  setMonasphereConfKey(QStringLiteral("flat_fov"), QString::number(static_cast<double>(opt_.flat_fov_deg), 'f', 2));
  setMonasphereConfKey(QStringLiteral("flat_distance"),
                       QString::number(static_cast<double>(opt_.screen_distance), 'f', 2));
  setMonasphereConfKey(QStringLiteral("milkyway_gain"),
                       QString::number(static_cast<double>(milkyway_gain_), 'f', 2));
  setMonasphereConfKey(QStringLiteral("surround_gain"),
                       QString::number(static_cast<double>(surround_gain_), 'f', 2));
  setMonasphereConfKey(QStringLiteral("sky_from_timezone"), sky_from_timezone_ ? QStringLiteral("1") : QStringLiteral("0"));
  setMonasphereConfKey(QStringLiteral("sky_lat"), QString::number(static_cast<double>(sky_lat_), 'f', 2));
  setMonasphereConfKey(QStringLiteral("sky_lon"), QString::number(static_cast<double>(sky_lon_), 'f', 2));
}

void HostWindow::loadAppConf() {
  int fsr_migrated = -1;
  QString runtime_migrated;
  QString last_dir_migrated;
  const QString legacy = QDir::homePath() + QStringLiteral("/.config/monasphere/conf");
  QFile f(legacy);
  if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
    while (!f.atEnd()) {
      const QString line = QString::fromUtf8(f.readLine()).trimmed();
      if (line.startsWith(QStringLiteral("fsr="))) {
        bool ok = false;
        const int v = line.mid(4).toInt(&ok);
        if (ok && v >= 0 && v <= 3) fsr_migrated = v;
      } else if (line.startsWith(QStringLiteral("runtime="))) {
        runtime_migrated = line.mid(8);
      } else if (line.startsWith(QStringLiteral("last_dir="))) {
        last_dir_migrated = line.mid(9);
      }
    }
    f.close();
  }

  QHash<QString, QString> defaults{
      {QStringLiteral("volume"), QStringLiteral("0.2")},
      {QStringLiteral("fsr"),
       QString::number(fsr_migrated >= 0 ? fsr_migrated : static_cast<int>(vrp::FsrMode::Quality))},
      {QStringLiteral("hz"), QStringLiteral("0")},
      {QStringLiteral("runtime"),
       runtime_migrated == QLatin1String("wivrn") ? QStringLiteral("wivrn") : QStringLiteral("monado")},
      {QStringLiteral("pad"), QStringLiteral("auto")},
      {QStringLiteral("hand"), QStringLiteral("right")},
      {QStringLiteral("gamepad"), QStringLiteral("1")},
      {QStringLiteral("thumb_count"), QString::number(vrp::kSeekThumbCount)},
      {QStringLiteral("thumb_edge"), QString::number(vrp::kSeekThumbMaxEdge)},
      {QStringLiteral("hwaccel"), QStringLiteral("1")},
      {QStringLiteral("thumb_jpeg"), QStringLiteral("1")},
      {QStringLiteral("flat_fov"), QString::number(static_cast<double>(opt_.flat_fov_deg), 'f', 2)},
      {QStringLiteral("flat_distance"),
       QString::number(static_cast<double>(opt_.screen_distance), 'f', 2)},
      {QStringLiteral("milkyway_gain"), QStringLiteral("1.00")},
      {QStringLiteral("surround_gain"), QStringLiteral("0.55")},
      {QStringLiteral("sky_from_timezone"), QStringLiteral("1")},
      {QStringLiteral("sky_lat"), QStringLiteral("35.68")},
      {QStringLiteral("sky_lon"), QStringLiteral("139.65")},
      {QStringLiteral("live_restore_token"), QString()},
      {QStringLiteral("live_setup_spans"), QStringLiteral("1,2,3")},
      {QStringLiteral("live_capture"), QStringLiteral("monitor")},
      {QStringLiteral("live_output"), QString()},
      {QStringLiteral("live_virtual"), QStringLiteral("1")},
      {QStringLiteral("live_virtual_size"), QStringLiteral("1280x720")},
      {QStringLiteral("live_virtual_scale"), QStringLiteral("2")},
      {QStringLiteral("live_chrome_scale"), QStringLiteral("1")},
      {QStringLiteral("live_cursor_scale"), QStringLiteral("1")},
      {QStringLiteral("live_chrome_profile"), QStringLiteral("default")},
  };
  if (!last_dir_migrated.isEmpty()) defaults.insert(QStringLiteral("last_dir"), last_dir_migrated);
  migrateYoutubeConfKeysToLive();
  ensureMonasphereConfKeys(defaults);
  bool migrated = QFile::exists(monasphereConfPath()) && last_dir_migrated.isEmpty();
  QFile check(monasphereConfPath());
  if (!migrated && check.open(QIODevice::ReadOnly | QIODevice::Text)) {
    while (!check.atEnd()) {
      const QString line = QString::fromUtf8(check.readLine()).trimmed();
      if (line.startsWith(QStringLiteral("last_dir="))) {
        migrated = true;
        break;
      }
    }
  }
  if (migrated) QFile::remove(legacy);

  QFile user(monasphereConfPath());
  if (!user.open(QIODevice::ReadOnly | QIODevice::Text)) return;
  bool legacy_pad_gamepad = false;
  bool saw_gamepad_key = false;
  live_apps_.clear();
  live_desktop_apps_.clear();
  QMap<int, LiveAppEntry> live_by_index;
  QVector<LiveAppEntry> live_unindexed;
  QMap<int, LiveDesktopAppEntry> desktop_by_index;
  QVector<LiveDesktopAppEntry> desktop_unindexed;
  QMap<int, QMap<int, LiveDesktopTitle>> desktop_titles_by_app;
  QString legacy_youtube_url;
  bool saw_live_app_key = false;
  while (!user.atEnd()) {
    const QString line = QString::fromUtf8(user.readLine()).trimmed();
    if (line.startsWith(QStringLiteral("fsr="))) {
      bool ok = false;
      const int v = line.mid(4).toInt(&ok);
      if (ok && v >= 0 && v <= 3) preferred_fsr_ = static_cast<vrp::FsrMode>(v);
    } else if (line.startsWith(QStringLiteral("hz="))) {
      bool ok = false;
      const int v = line.mid(3).toInt(&ok);
      if (ok && (v == 0 || v == 90 || v == 120)) preferred_hz_ = v;
    } else if (line.startsWith(QStringLiteral("runtime="))) {
      runtime_kind_.store(line.mid(8) == QLatin1String("wivrn") ? 1 : 0);
    } else if (line.startsWith(QStringLiteral("pad="))) {
      const QString v = line.mid(4);
      if (v == QLatin1String("auto")) opt_.controller_profile = vrp::ControllerProfile::Auto;
      else if (v == QLatin1String("simple")) opt_.controller_profile = vrp::ControllerProfile::Simple;
      else if (v == QLatin1String("oculus_touch")) opt_.controller_profile = vrp::ControllerProfile::OculusTouch;
      else if (v == QLatin1String("ms_motion")) opt_.controller_profile = vrp::ControllerProfile::MicrosoftMotion;
      else if (v == QLatin1String("vive")) opt_.controller_profile = vrp::ControllerProfile::ViveController;
      else if (v == QLatin1String("index")) opt_.controller_profile = vrp::ControllerProfile::ValveIndex;
      else if (v == QLatin1String("gamepad")) {
        legacy_pad_gamepad = true;
        opt_.controller_profile = vrp::ControllerProfile::Auto;
      }
    } else if (line.startsWith(QStringLiteral("gamepad="))) {
      saw_gamepad_key = true;
      gamepad_enabled_.store(line.mid(8) != QLatin1String("0"));
    } else if (line.startsWith(QStringLiteral("hand="))) {
      opt_.controller_hand = line.mid(5) == QLatin1String("left") ? vrp::ControllerHand::Left
                                                                  : vrp::ControllerHand::Right;
    } else if (line.startsWith(QStringLiteral("thumb_count="))) {
      bool ok = false;
      const int v = line.mid(12).toInt(&ok);
      if (ok && v >= vrp::kSeekThumbCountMin && v <= vrp::kSeekThumbCountMax) seek_thumb_count_ = v;
    } else if (line.startsWith(QStringLiteral("thumb_edge="))) {
      bool ok = false;
      int v = line.mid(11).toInt(&ok);
      if (ok && v >= vrp::kSeekThumbEdgeMin && v <= vrp::kSeekThumbEdgeMax) {
        v &= ~1;
        if (v >= vrp::kSeekThumbEdgeMin) seek_thumb_edge_ = v;
      }
    } else if (line.startsWith(QStringLiteral("hwaccel="))) {
      bool ok = false;
      const int v = line.mid(8).toInt(&ok);
      if (ok && (v == 0 || v == 1)) hwaccel_ = v != 0;
    } else if (line.startsWith(QStringLiteral("thumb_jpeg="))) {
      bool ok = false;
      const int v = line.mid(11).toInt(&ok);
      if (ok && (v == 0 || v == 1)) thumb_jpeg_ = v != 0;
    } else if (line.startsWith(QStringLiteral("flat_fov="))) {
      bool ok = false;
      const float v = line.mid(9).toFloat(&ok);
      if (ok && v >= 20.f && v <= 140.f) opt_.flat_fov_deg = v;
    } else if (line.startsWith(QStringLiteral("flat_distance="))) {
      bool ok = false;
      const float v = line.mid(14).toFloat(&ok);
      if (ok && v >= 2.f && v <= 40.f) opt_.screen_distance = v;
    } else if (line.startsWith(QStringLiteral("milkyway_gain="))) {
      bool ok = false;
      const float v = line.mid(14).toFloat(&ok);
      if (ok && v >= 0.2f && v <= 2.5f) milkyway_gain_ = v;
    } else if (line.startsWith(QStringLiteral("surround_gain="))) {
      bool ok = false;
      const float v = line.mid(14).toFloat(&ok);
      if (ok && v >= 0.f && v <= 2.f) surround_gain_ = v;
    } else if (line.startsWith(QStringLiteral("sky_from_timezone="))) {
      bool ok = false;
      const int v = line.mid(18).toInt(&ok);
      if (ok) sky_from_timezone_ = v != 0;
    } else if (line.startsWith(QStringLiteral("sky_lat="))) {
      bool ok = false;
      const float v = line.mid(8).toFloat(&ok);
      if (ok && v >= -90.f && v <= 90.f) sky_lat_ = v;
    } else if (line.startsWith(QStringLiteral("sky_lon="))) {
      bool ok = false;
      const float v = line.mid(8).toFloat(&ok);
      if (ok && v >= -180.f && v <= 180.f) sky_lon_ = v;
    } else if (line.startsWith(QStringLiteral("live_restore_token"))) {
      // live_restore_token[xN]=…  or legacy live_restore_token=
      const QRegularExpression span_tok(
          QStringLiteral("^live_restore_token\\[x([1-3])\\]=(.*)$"));
      const QRegularExpressionMatch sm = span_tok.match(line);
      if (sm.hasMatch()) {
        const int span = sm.captured(1).toInt();
        const QString tok = sm.captured(2);
        live_restore_tokens_.insert(span, tok);
        if (span == 1) live_restore_token_ = tok;
      } else if (line.startsWith(QStringLiteral("live_restore_token="))) {
        live_restore_token_ = line.mid(19);
        live_restore_tokens_.insert(1, live_restore_token_);
      }
    } else if (line.startsWith(QStringLiteral("youtube_url="))) {
      // Legacy — migrate into live_app[0] only when no live_app entries exist.
      const QString u = line.mid(12).trimmed();
      if (!u.isEmpty()) legacy_youtube_url = u;
    } else if (line.startsWith(QStringLiteral("live_app"))) {
      saw_live_app_key = true;
      // live_app[N]=Name|URL  or legacy live_app=Name|URL
      const QRegularExpression indexed(
          QStringLiteral("^live_app\\[(\\d+)\\]=(.*)$"));
      const QRegularExpressionMatch im = indexed.match(line);
      QString rest;
      int idx = -1;
      if (im.hasMatch()) {
        idx = im.captured(1).toInt();
        rest = im.captured(2).trimmed();
      } else if (line.startsWith(QStringLiteral("live_app="))) {
        rest = line.mid(9).trimmed();
      } else {
        continue;
      }
      if (rest.isEmpty()) continue;  // empty slot e.g. live_app[1]=
      const int bar = rest.indexOf(QLatin1Char('|'));
      if (bar <= 0) continue;
      const QString name = rest.left(bar).trimmed();
      const QString url = rest.mid(bar + 1).trimmed();
      if (name.isEmpty() ||
          !(url.startsWith(QLatin1String("http://")) ||
            url.startsWith(QLatin1String("https://")))) {
        continue;
      }
      if (idx >= 0) live_by_index.insert(idx, {name, url});
      else live_unindexed.push_back({name, url});
    } else if (line.startsWith(QStringLiteral("live_desktop_app"))) {
      // live_desktop_app[N].title[M]=Name|x1…x3|arg…  (monitor span + launch options)
      const QRegularExpression title_re(
          QStringLiteral("^live_desktop_app\\[(\\d+)\\]\\.title\\[(\\d+)\\]=(.*)$"));
      const QRegularExpressionMatch tm = title_re.match(line);
      if (tm.hasMatch()) {
        const int app_i = tm.captured(1).toInt();
        const int title_i = tm.captured(2).toInt();
        const QString rest = tm.captured(3).trimmed();
        if (rest.isEmpty()) continue;
        const QStringList parts = rest.split(QLatin1Char('|'));
        if (parts.isEmpty() || parts[0].trimmed().isEmpty()) continue;
        LiveDesktopTitle t;
        t.name = parts[0].trimmed();
        t.monitor_span = 1;
        if (parts.size() >= 2) {
          const QString spec = parts[1].trimmed().toLower();
          const QRegularExpression span_re(QStringLiteral("^x([1-3])$"));
          const QRegularExpressionMatch sm = span_re.match(spec);
          if (sm.hasMatch()) {
            t.monitor_span = sm.captured(1).toInt();
          } else {
            // Legacy Name|WxH|… → infer span from height (width is fixed now).
            const QRegularExpression size_re(QStringLiteral("^(\\d+)x(\\d+)$"));
            const QRegularExpressionMatch zm = size_re.match(spec);
            if (zm.hasMatch()) {
              const int tw = zm.captured(1).toInt();
              const int th = zm.captured(2).toInt();
              t.monitor_span = spanFromVirtualSize(tw, th);
            }
          }
        }
        QStringList raw_args;
        for (int i = 2; i < parts.size(); ++i) {
          const QString a = parts[i].trimmed();
          if (!a.isEmpty()) raw_args.push_back(a);
        }
        t.args = normalizeLiveExecArgs(raw_args);
        desktop_titles_by_app[app_i].insert(title_i, t);
        continue;
      }
      // live_desktop_app[N]=Name|exec[|arg…]  or live_desktop_app=Name|exec
      const QRegularExpression indexed(
          QStringLiteral("^live_desktop_app\\[(\\d+)\\]=(.*)$"));
      const QRegularExpressionMatch im = indexed.match(line);
      QString rest;
      int idx = -1;
      if (im.hasMatch()) {
        idx = im.captured(1).toInt();
        rest = im.captured(2).trimmed();
      } else if (line.startsWith(QStringLiteral("live_desktop_app="))) {
        rest = line.mid(17).trimmed();
      } else {
        continue;
      }
      if (rest.isEmpty()) continue;
      const QStringList parts = rest.split(QLatin1Char('|'));
      if (parts.size() < 2) continue;
      const QString name = parts[0].trimmed();
      const QString exec = parts[1].trimmed();
      if (name.isEmpty() || exec.isEmpty()) continue;
      LiveDesktopAppEntry e;
      e.name = name;
      e.exec = expandLiveArgPath(exec);
      QStringList raw_args;
      for (int i = 2; i < parts.size(); ++i) {
        const QString a = parts[i].trimmed();
        if (!a.isEmpty()) raw_args.push_back(a);
      }
      e.args = normalizeLiveExecArgs(raw_args);
      if (idx >= 0) desktop_by_index.insert(idx, e);
      else desktop_unindexed.push_back(e);
    } else if (line.startsWith(QStringLiteral("live_capture="))) {
      const QString v = line.mid(13).trimmed().toLower();
      live_capture_monitor_ = (v != QLatin1String("window"));
    } else if (line.startsWith(QStringLiteral("live_output="))) {
      live_output_ = line.mid(12).trimmed();
    } else if (line.startsWith(QStringLiteral("live_virtual="))) {
      bool ok = false;
      const int v = line.mid(13).toInt(&ok);
      if (ok) live_virtual_ = v != 0;
    } else if (line.startsWith(QStringLiteral("live_virtual_size="))) {
      const QString v = line.mid(18).trimmed().toLower();
      const QRegularExpression re(QStringLiteral("^(\\d+)x(\\d+)$"));
      const QRegularExpressionMatch m = re.match(v);
      if (m.hasMatch()) {
        const int w = m.captured(1).toInt();
        const int h = m.captured(2).toInt();
        if (w >= 640 && w <= 7680 && h >= 360 && h <= 4320) {
          live_virtual_w_ = w;
          live_virtual_h_ = h;
        }
      }
    } else if (line.startsWith(QStringLiteral("live_virtual_scale="))) {
      bool ok = false;
      const float v = line.mid(19).toFloat(&ok);
      if (ok && v >= 1.f && v <= 3.f) live_virtual_scale_ = v;
    } else if (line.startsWith(QStringLiteral("live_chrome_scale="))) {
      bool ok = false;
      const float v = line.mid(17).toFloat(&ok);
      if (ok && v >= 1.f && v <= 3.f) live_chrome_scale_ = v;
    } else if (line.startsWith(QStringLiteral("live_cursor_scale="))) {
      bool ok = false;
      const int v = line.mid(17).toInt(&ok);
      if (ok && v >= 1 && v <= 6) live_cursor_scale_ = v;
    } else if (line.startsWith(QStringLiteral("live_chrome_profile="))) {
      const QString v = line.mid(19).trimmed().toLower();
      live_chrome_default_profile_ = (v != QLatin1String("isolated"));
    }
  }
  user.close();

  if (!live_by_index.isEmpty()) {
    for (auto it = live_by_index.cbegin(); it != live_by_index.cend(); ++it) {
      live_apps_.push_back(it.value());
    }
  } else {
    live_apps_ = live_unindexed;
  }

  if (!desktop_by_index.isEmpty()) {
    for (auto it = desktop_by_index.cbegin(); it != desktop_by_index.cend(); ++it) {
      LiveDesktopAppEntry e = it.value();
      const auto tit = desktop_titles_by_app.constFind(it.key());
      if (tit != desktop_titles_by_app.cend()) {
        for (auto jt = tit->cbegin(); jt != tit->cend(); ++jt) e.titles.push_back(jt.value());
      }
      live_desktop_apps_.push_back(e);
    }
  } else {
    live_desktop_apps_ = desktop_unindexed;
  }

  const bool need_live_seed = live_apps_.isEmpty();
  const bool need_live_rewrite =
      need_live_seed || !legacy_youtube_url.isEmpty() ||
      (saw_live_app_key && live_by_index.isEmpty() && !live_unindexed.isEmpty());
  if (need_live_seed) {
    const QString url = !legacy_youtube_url.isEmpty()
                            ? legacy_youtube_url
                            : QStringLiteral("https://www.youtube.com/");
    live_apps_.push_back({QStringLiteral("YouTube"), url});
  }
  if (need_live_rewrite) {
    // Normalize to live_app[0]=… / live_app[1]= (empty slot for user adds).
    const QString path = monasphereConfPath();
    QFile in(path);
    QStringList lines;
    if (in.open(QIODevice::ReadOnly | QIODevice::Text)) {
      while (!in.atEnd()) {
        const QString line = QString::fromUtf8(in.readLine()).trimmed();
        if (line.isEmpty()) continue;
        if (line.startsWith(QStringLiteral("live_app"))) continue;
        if (line.startsWith(QStringLiteral("youtube_url="))) continue;
        lines << line;
      }
      in.close();
    }
    for (int i = 0; i < live_apps_.size(); ++i) {
      const LiveAppEntry& a = live_apps_[i];
      lines << QStringLiteral("live_app[%1]=%2|%3").arg(i).arg(a.name, a.url);
    }
    lines << QStringLiteral("live_app[%1]=").arg(live_apps_.size());
    QFile out(path);
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
      QTextStream ts(&out);
      for (const QString& line : lines) ts << line << '\n';
    }
  }

  if (sky_from_timezone_) {
    float lat = sky_lat_, lon = sky_lon_;
    QString zid;
    if (observerFromHostTimezone(lat, lon, &zid)) {
      sky_lat_ = lat;
      sky_lon_ = lon;
      setMonasphereConfKey(QStringLiteral("sky_lat"), QString::number(static_cast<double>(sky_lat_), 'f', 4));
      setMonasphereConfKey(QStringLiteral("sky_lon"), QString::number(static_cast<double>(sky_lon_), 'f', 4));
      VRP_LOG("sky observer from timezone %s → lat=%.4f lon=%.4f (geocentric dirs, horizon tip only)",
              zid.toUtf8().constData(), static_cast<double>(sky_lat_), static_cast<double>(sky_lon_));
    }
  }
  if (legacy_pad_gamepad) {
    if (!saw_gamepad_key) gamepad_enabled_.store(true);
    setMonasphereConfKey(QStringLiteral("pad"), QStringLiteral("auto"));
    setMonasphereConfKey(QStringLiteral("gamepad"), gamepad_enabled_.load() ? QStringLiteral("1") : QStringLiteral("0"));
  }
}

void HostWindow::restoreWindowGeometry() {
  QString stored = monasphereConfValue(QStringLiteral("geometry"));
  if (stored.isEmpty()) {
    stored = legacyQtValue(QStringLiteral("ui"), QStringLiteral("geometry"));
    if (!stored.isEmpty()) setMonasphereConfKey(QStringLiteral("geometry"), stored);
  }
  const QByteArray geo = QByteArray::fromBase64(stored.toLatin1());
  if (!geo.isEmpty() && restoreGeometry(geo)) return;
  resize(640, 480);
}

void HostWindow::saveWindowGeometry() const {
  setMonasphereConfKey(QStringLiteral("geometry"), QString::fromLatin1(saveGeometry().toBase64()));
}

void HostWindow::buildMenus() {
  auto* ops = menuBar()->addMenu(tr("Actions"));

  auto* runtime_row = new QWidget;
  auto* runtime_layout = new QHBoxLayout(runtime_row);
  runtime_layout->setContentsMargins(12, 4, 8, 4);
  runtime_layout->setSpacing(12);
  runtime_monado_rb_ = new QRadioButton(QStringLiteral("Monado"), runtime_row);
  runtime_wivrn_rb_ = new QRadioButton(QStringLiteral("WiVRn"), runtime_row);
  runtime_monado_rb_->setChecked(true);
  runtime_layout->addWidget(runtime_monado_rb_);
  runtime_layout->addWidget(runtime_wivrn_rb_);
  runtime_layout->addStretch(1);
  auto* runtime_action = new QWidgetAction(ops);
  runtime_action->setDefaultWidget(runtime_row);
  ops->addAction(runtime_action);
  connect(runtime_monado_rb_, &QRadioButton::toggled, this, [this](bool on) {
    if (on) onRuntimeChosen(false);
  });
  connect(runtime_wivrn_rb_, &QRadioButton::toggled, this, [this](bool on) {
    if (on) onRuntimeChosen(true);
  });

  act_start_runtime_ = ops->addAction(tr("Start Monado"), this, &HostWindow::startMonado);
  act_stop_runtime_ = ops->addAction(tr("Stop Monado"), this, &HostWindow::stopMonado);
  connect_menu_ = ops->addMenu(tr("Connect"));
  connect(connect_menu_, &QMenu::aboutToShow, this, &HostWindow::rebuildConnectDeviceMenu);
  act_disconnect_ = ops->addAction(tr("Disconnect"), this, &HostWindow::disconnectHeadset);
  act_disconnect_->setEnabled(false);
  ops->addSeparator();
  ops->addAction(tr("Quit"), this, &QWidget::close);

  auto* settings = menuBar()->addMenu(tr("Settings"));
  audio_menu_ = settings->addMenu(tr("Audio"));
  audio_group_ = new QActionGroup(this);
  audio_group_->setExclusive(true);
  connect(audio_menu_, &QMenu::aboutToShow, this, &HostWindow::rebuildAudioMenu);
  connect(audio_group_, &QActionGroup::triggered, this, &HostWindow::onAudioDeviceAction);

  fsr_menu_ = settings->addMenu(tr("FSR"));
  fsr_group_ = new QActionGroup(this);
  fsr_group_->setExclusive(true);
  connect(fsr_menu_, &QMenu::aboutToShow, this, &HostWindow::rebuildFsrMenu);
  connect(fsr_group_, &QActionGroup::triggered, this, &HostWindow::onFsrAction);
  rebuildFsrMenu();

  hz_menu_ = settings->addMenu(tr("Hz"));
  hz_group_ = new QActionGroup(this);
  hz_group_->setExclusive(true);
  connect(hz_menu_, &QMenu::aboutToShow, this, &HostWindow::rebuildHzMenu);
  connect(hz_group_, &QActionGroup::triggered, this, &HostWindow::onHzAction);
  rebuildHzMenu();

  controller_menu_ = settings->addMenu(tr("Pad"));
  controller_group_ = new QActionGroup(this);
  controller_group_->setExclusive(true);
  struct ProfileItem {
    vrp::ControllerProfile profile;
    const char* label;
  };
  const ProfileItem profiles[] = {
      {vrp::ControllerProfile::Auto, QT_TR_NOOP("Auto")},
      {vrp::ControllerProfile::OculusTouch, QT_TR_NOOP("Touch")},
      {vrp::ControllerProfile::Simple, QT_TR_NOOP("Simple")},
      {vrp::ControllerProfile::MicrosoftMotion, QT_TR_NOOP("MS Motion")},
      {vrp::ControllerProfile::ViveController, QT_TR_NOOP("Vive")},
      {vrp::ControllerProfile::ValveIndex, QT_TR_NOOP("Index")},
  };
  for (const auto& p : profiles) {
    auto* a = controller_menu_->addAction(tr(p.label));
    a->setCheckable(true);
    a->setData(static_cast<int>(p.profile));
    controller_group_->addAction(a);
    if (p.profile == opt_.controller_profile) a->setChecked(true);
  }
  connect(controller_group_, &QActionGroup::triggered, this, &HostWindow::onControllerProfileAction);

  hand_menu_ = settings->addMenu(tr("Hand"));
  hand_group_ = new QActionGroup(this);
  hand_group_->setExclusive(true);
  auto* right = hand_menu_->addAction(tr("Right"));
  right->setCheckable(true);
  right->setData(static_cast<int>(vrp::ControllerHand::Right));
  hand_group_->addAction(right);
  auto* left = hand_menu_->addAction(tr("Left"));
  left->setCheckable(true);
  left->setData(static_cast<int>(vrp::ControllerHand::Left));
  hand_group_->addAction(left);
  if (opt_.controller_hand == vrp::ControllerHand::Left) left->setChecked(true);
  else right->setChecked(true);
  connect(hand_group_, &QActionGroup::triggered, this, &HostWindow::onHandAction);

  act_gamepad_ = settings->addAction(tr("SDL gamepad"));
  act_gamepad_->setCheckable(true);
  act_gamepad_->setChecked(gamepad_enabled_.load());
  connect(act_gamepad_, &QAction::toggled, this, [this](bool on) {
    gamepad_enabled_.store(on);
    setMonasphereConfKey(QStringLiteral("gamepad"), on ? QStringLiteral("1") : QStringLiteral("0"));
    appendLog(on ? tr("SDL gamepad on") : tr("SDL gamepad off"));
  });
  connect(settings, &QMenu::aboutToShow, this, [this] { refreshGamepadActionLabel(); });

  auto* tools = menuBar()->addMenu(tr("Tools"));
  tools->addAction(tr("Set up live capture (x1–x3)…"), this, &HostWindow::setupYouTubeCapture);

  auto* help = menuBar()->addMenu(tr("Help"));
  help->addAction(tr("About"), this, &HostWindow::showAboutDialog);
}

void HostWindow::refreshGamepadActionLabel() {
  if (!act_gamepad_) return;
  QString name;
  if (gamepad_sdl_owned_.load()) {
    std::lock_guard<std::mutex> lock(gamepad_name_mu_);
    name = gamepad_seen_name_;
  } else {
    const auto names = vrp::GamepadInput::list_names();
    if (!names.empty()) name = QString::fromStdString(names.front());
  }
  if (name.isEmpty()) name = tr("SDL gamepad");
  else name.replace(QLatin1Char('&'), QStringLiteral("&&"));
  act_gamepad_->setText(name);
}

QString HostWindow::tokenForSpan(int span) const {
  const int s = std::clamp(span, 1, 3);
  if (vdisp_mgr_) {
    const QString t = vdisp_mgr_->token(s);
    if (!t.isEmpty()) return t;
  }
  const auto it = live_restore_tokens_.constFind(s);
  if (it != live_restore_tokens_.cend() && !it.value().isEmpty()) return it.value();
  if (s == 1 && !live_restore_token_.isEmpty()) return live_restore_token_;
  return {};
}

void HostWindow::saveTokenForSpan(int span, const QString& token) {
  const int s = std::clamp(span, 1, 3);
  // Prefer manager::setToken from session; this path syncs conf + manager without re-emit loop.
  if (vdisp_mgr_) vdisp_mgr_->loadToken(s, token);
  live_restore_tokens_.insert(s, token);
  setMonasphereConfKey(QStringLiteral("live_restore_token[x%1]").arg(s), token);
  if (s == 1) {
    live_restore_token_ = token;
    setMonasphereConfKey(QStringLiteral("live_restore_token"), token);
  }
}

void HostWindow::syncTokensIntoManager() {
  if (!vdisp_mgr_) return;
  for (int s = 1; s <= 3; ++s) {
    QString t = live_restore_tokens_.value(s);
    if (t.isEmpty() && s == 1) t = live_restore_token_;
    if (!t.isEmpty()) vdisp_mgr_->loadToken(s, t);
  }
}

void HostWindow::virtualSizeForSpan(int span, int* out_w, int* out_h) const {
  const int s = std::clamp(span, 1, 3);
  const int w = live_virtual_w_ > 0 ? live_virtual_w_ : 1280;
  const int h1 = live_virtual_h_ > 0 ? live_virtual_h_ : 720;
  int h = h1;
  if (s == 2) {
    // 8:3 into fixed width (3840×1440 when w=3840).
    h = std::max(360, (w * 3) / 8);
  } else if (s == 3) {
    // 4:1 into fixed width (3840×960 when w=3840).
    h = std::max(360, w / 4);
  }
  if (out_w) *out_w = w;
  if (out_h) *out_h = h;
}

int HostWindow::spanFromVirtualSize(int /*width*/, int height) const {
  if (height <= 0) return 1;
  int w1 = 0, h1 = 0, h2 = 0, h3 = 0;
  virtualSizeForSpan(1, &w1, &h1);
  virtualSizeForSpan(2, &w1, &h2);
  virtualSizeForSpan(3, &w1, &h3);
  const int d1 = std::abs(height - h1);
  const int d2 = std::abs(height - h2);
  const int d3 = std::abs(height - h3);
  if (d2 <= d1 && d2 <= d3) return 2;
  if (d3 <= d1 && d3 <= d2) return 3;
  return 1;
}

vrp::VirtualDisplayManager::Spec HostWindow::virtualSpecForSpan(int span) const {
  vrp::VirtualDisplayManager::Spec s;
  const int sp = std::clamp(span, 1, 3);
  s.span = sp;
  s.name = vrp::KdeVirtualDisplay::nameForSpan(sp);
  virtualSizeForSpan(sp, &s.width, &s.height);
  s.scale = live_virtual_scale_;
  return s;
}

void HostWindow::destroyVirtualForLeaveLive() {
  if (!vdisp_mgr_) return;
  vdisp_mgr_->destroy();
}

void HostWindow::ensureYouTubeSession() {
  if (!vdisp_mgr_) {
    vdisp_mgr_ = new vrp::VirtualDisplayManager(this);
    connect(vdisp_mgr_, &vrp::VirtualDisplayManager::logMessage, this, &HostWindow::appendLog);
    connect(vdisp_mgr_, &vrp::VirtualDisplayManager::busyStatus, this, [this](const QString& s) {
      // Empty must clear — otherwise 「切替中」/「削除中」stick and XR skips DMA forever.
      if (!yt_session_) return;
      if (s.isEmpty()) yt_session_->clear_busy_status();
      else yt_session_->set_busy_status(s);
    });
    connect(vdisp_mgr_, &vrp::VirtualDisplayManager::tokenChanged, this,
            [this](int span, const QString& t) {
              const int s = std::clamp(span, 1, 3);
              live_session_span_ = s;
              live_restore_tokens_.insert(s, t);
              if (s == 1) live_restore_token_ = t;
              // Persist non-empty; empty clear stays in-memory (other spans untouched in conf).
              if (!t.isEmpty()) {
                setMonasphereConfKey(QStringLiteral("live_restore_token[x%1]").arg(s), t);
                if (s == 1) setMonasphereConfKey(QStringLiteral("live_restore_token"), t);
                appendLog(tr("Live: saved restore token for x%1 (%2 chars)").arg(s).arg(t.size()));
              }
            });
    syncTokensIntoManager();
  }
  if (yt_session_) return;
  yt_session_ = new vrp::YouTubeSession(this);
  yt_session_->set_virtual_manager(vdisp_mgr_);
  connect(yt_session_, &vrp::YouTubeSession::logMessage, this, &HostWindow::appendLog);
  connect(yt_session_, &vrp::YouTubeSession::toast, this, [this](const QString& m) {
    showHudToast(m);
  });
  connect(yt_session_, &vrp::YouTubeSession::restoreTokenChanged, this, [this](const QString& t) {
    // Session → manager (Host persists via manager::tokenChanged).
    if (!vdisp_mgr_) return;
    const int span = vdisp_mgr_->active_span() > 0
                         ? vdisp_mgr_->active_span()
                         : (yt_session_ ? spanFromVirtualSize(yt_session_->virtual_w(),
                                                              yt_session_->virtual_h())
                                        : live_session_span_);
    if (t.isEmpty()) vdisp_mgr_->clearToken(span);
    else vdisp_mgr_->setToken(span, t);
  });
  connect(yt_session_, &vrp::YouTubeSession::failed, this, [this](const QString& e) {
    pad_hook_.stop();
    pad_hook_active_ = false;
    pad_filter_.stop();
    appendLog(e);
    if (yt_setup_phase_ == YtSetupPhase::Refresh) {
      yt_setup_phase_ = YtSetupPhase::None;
      yt_setup_queue_.clear();
      yt_setup_continue_ = false;
      appendLog(tr("Live setup: restore verify failed — HMD live apps may ask host to approve again"));
      showHudToast(tr("Setup: restore verify failed"));
    } else if (yt_setup_phase_ == YtSetupPhase::Capture) {
      yt_setup_phase_ = YtSetupPhase::None;
      yt_setup_queue_.clear();
      yt_setup_continue_ = false;
      showHudToast(tr("Live setup failed"));
    } else {
      showHudToast(tr("Live capture failed"));
    }
  });
  connect(yt_session_, &vrp::YouTubeSession::streamingStarted, this, [this] {
    if (yt_session_) {
      live_output_ = yt_session_->output_name();
      if (!live_output_.isEmpty()) {
        setMonasphereConfKey(QStringLiteral("live_output"), live_output_);
      }
    }
    // Host Tools setup: Capture (interactive token) then optional Refresh (silent restore).
    if (yt_setup_phase_ == YtSetupPhase::Capture) {
      const QString tok = tokenForSpan(live_session_span_);
      if (tok.isEmpty()) {
        yt_setup_phase_ = YtSetupPhase::None;
        yt_setup_queue_.clear();
        yt_setup_continue_ = false;
        yt_host_setup_stop_after_stream_ = false;
        appendLog(tr("Live setup FAILED — portal returned no restore_token for x%1. "
                     "On the KDE share dialog, enable “Remember” / allow restore, "
                     "then run Tools → Set up live capture again.")
                      .arg(live_session_span_));
        showHudToast(tr("No token — check Remember on host dialog"));
        QTimer::singleShot(800, this, [this] { stopYouTubeCapture(); });
        return;
      }
      appendLog(tr("Live setup: x%1 token saved (%2 chars) — stopping, then verifying restore…")
                    .arg(live_session_span_)
                    .arg(tok.size()));
      showHudToast(tr("Token x%1 saved — verifying…").arg(live_session_span_));
      QTimer::singleShot(1000, this, [this] { stopYouTubeCapture(); });
      return;
    }
    if (yt_setup_phase_ == YtSetupPhase::Refresh) {
      yt_host_setup_stop_after_stream_ = false;
      appendLog(tr("Live setup OK — x%1 restore verified (token %2 chars)")
                    .arg(live_session_span_)
                    .arg(tokenForSpan(live_session_span_).size()));
      if (!yt_setup_queue_.isEmpty()) {
        const int next = yt_setup_queue_.front();
        yt_setup_continue_ = true;
        yt_setup_phase_ = YtSetupPhase::None;
        showHudToast(tr("x%1 OK — next: approve x%2 on host").arg(live_session_span_).arg(next));
        appendLog(tr("Live setup: continuing with x%1…").arg(next));
      } else {
        yt_setup_continue_ = false;
        yt_setup_phase_ = YtSetupPhase::None;
        showHudToast(tr("Setup OK — all spans ready"));
        appendLog(tr("Live setup: all queued spans registered"));
      }
      QTimer::singleShot(1000, this, [this] { stopYouTubeCapture(); });
      return;
    }
    if (yt_host_setup_stop_after_stream_) {
      yt_host_setup_stop_after_stream_ = false;
      QTimer::singleShot(1000, this, [this] { stopYouTubeCapture(); });
    }
  });
  connect(yt_session_, &vrp::YouTubeSession::streamLost, this, [this] {
    // Portal Session.Closed often arrives *before* QProcess::finished when RetroArch
    // exits. Arm [アプリ] return here so stopped() still opens the menu after Virtual
    // delete (same procedure as desktopAppExited).
    if (yt_session_ && yt_session_->desktop_app_mode()) {
      yt_return_apps_menu_.store(true, std::memory_order_release);
    }
    yt_stop_requested_.store(true, std::memory_order_release);
  });
  connect(yt_session_, &vrp::YouTubeSession::desktopAppExited, this, [this] {
    // Ignore exit while a Live/[アプリ] switch is queued — stop() killed the old app on purpose.
    if (yt_pending_kind_ != YtPendingKind::None) {
      appendLog(tr("Desktop app exited during switch — ignored (queued session owns teardown)"));
      return;
    }
    // 「終了中」→ XR DMA drop → stop() (Virtual stays) → stopped() destroys Virtual → [アプリ].
    yt_return_apps_menu_.store(true, std::memory_order_release);
    pad_hook_.set_block_guest(false);
    pad_filter_.stop();  // legacy uinput grab only
    yt_stop_requested_.store(true, std::memory_order_release);
    appendLog(tr("Desktop app exited — Virtual 削除後に [アプリ]"));
    showHudToast(tr("終了中…"));
    const uint64_t stop_gen = yt_stop_gen_.load(std::memory_order_acquire);
    QTimer::singleShot(4000, this, [this, stop_gen] {
      if (yt_stop_gen_.load(std::memory_order_acquire) != stop_gen) return;
      if (yt_pending_kind_ != YtPendingKind::None) return;
      if (!yt_session_) return;
      if (yt_session_->state() == vrp::YouTubeSession::State::Idle) {
        if (yt_return_apps_menu_.exchange(false, std::memory_order_acq_rel)) {
          destroyVirtualForLeaveLive();
          yt_show_apps_after_stop_.store(true, std::memory_order_release);
        }
        return;
      }
      yt_session_->clear_busy_status();
      yt_session_->stop();
    });
  });
  connect(yt_session_, &vrp::YouTubeSession::stopped, this, [this] {
    pad_hook_.stop();
    pad_hook_active_ = false;
    pad_filter_.stop();
    // No fixed settle — next start waits only if still before this timestamp (usually now).
    yt_restore_ready_ms_ = QDateTime::currentMSecsSinceEpoch();
    // Video queue wins over [アプリ] return (open_video while live).
    if (yt_await_video_after_stop_.load(std::memory_order_acquire) ||
        yt_pending_kind_ == YtPendingKind::Video) {
      yt_pending_kind_ = YtPendingKind::None;
      yt_return_apps_menu_.store(false, std::memory_order_release);
      appendLog(tr("Live cleanup done — removing Virtual before video"));
      finishQueuedVideoAfterVirtualGone();
      return;
    }
    if (yt_pending_kind_ != YtPendingKind::None) {
      // Next start() → VirtualDisplayManager::ensure (reuse or recreate by resolution).
      yt_return_apps_menu_.store(false, std::memory_order_release);
      appendLog(tr("Live cleanup done — starting queued session (Virtual manager decides)"));
      flushPendingLiveStart();
      return;
    }
    if (yt_return_apps_menu_.exchange(false, std::memory_order_acq_rel)) {
      destroyVirtualForLeaveLive();
      if (yt_session_) yt_session_->clear_busy_status();
      yt_show_apps_after_stop_.store(true, std::memory_order_release);
      appendLog(tr("Live cleanup done — Virtual gone, opening [アプリ]"));
      return;
    }
    if (yt_setup_phase_ == YtSetupPhase::Capture && !tokenForSpan(live_session_span_).isEmpty()) {
      yt_setup_phase_ = YtSetupPhase::Refresh;
      appendLog(tr("Live setup: verifying ScreenCast restore for x%1 (no HMD preview)…")
                    .arg(live_session_span_));
      const int span = live_session_span_;
      QTimer::singleShot(3500, this, [this, span] {
        if (yt_setup_phase_ != YtSetupPhase::Refresh) return;
        // Silent restore while still in setup — proves token works before HMD use.
        yt_hmd_suppress_live_.store(true, std::memory_order_release);
        beginYouTubeCapture(/*host_setup=*/false, /*url_override=*/{}, /*desktop_exec=*/{},
                            /*desktop_args=*/{}, span);
      });
      return;
    }
    if (yt_setup_continue_) {
      yt_setup_continue_ = false;
      QTimer::singleShot(3500, this, [this] {
        if (yt_setup_queue_.isEmpty()) return;
        beginNextSetupSpan(/*first=*/false);
      });
      return;
    }
    if (yt_setup_phase_ == YtSetupPhase::Refresh) {
      // Refresh restore failed before Streaming — clear phase; keep suppress until HMD.
      yt_setup_phase_ = YtSetupPhase::None;
      yt_setup_queue_.clear();
    }
    // Plain stop (Tools / no queued switch): drop Virtual.
    // Never reach here after a switch — pending path returns above. A stale fallback
    // stop() used to land here and destroy Virtual under a new x3 session.
    destroyVirtualForLeaveLive();
  });
}

void HostWindow::beginNextSetupSpan(bool first) {
  if (yt_setup_queue_.isEmpty()) {
    yt_setup_phase_ = YtSetupPhase::None;
    yt_setup_continue_ = false;
    yt_hmd_suppress_live_.store(true, std::memory_order_release);
    appendLog(tr("Live setup: finished (no spans left in queue)"));
    return;
  }
  const int span = yt_setup_queue_.takeFirst();
  live_session_span_ = span;
  yt_setup_phase_ = YtSetupPhase::Capture;
  yt_setup_continue_ = false;
  yt_hmd_suppress_live_.store(true, std::memory_order_release);
  int setup_w = 0, setup_h = 0;
  virtualSizeForSpan(span, &setup_w, &setup_h);
  QStringList left;
  for (int s : yt_setup_queue_) left << QStringLiteral("x%1").arg(s);
  appendLog(tr("Live setup: registering token for x%1 (%2×%3)%4")
                .arg(span)
                .arg(setup_w)
                .arg(setup_h)
                .arg(left.isEmpty() ? QString()
                                    : tr(" — then %1").arg(left.join(QStringLiteral(", ")))));
  if (!first) {
    showHudToast(tr("Host: pick %1 (Remember)")
                     .arg(vrp::KdeVirtualDisplay::outputNameForSpan(span)));
  }
  beginYouTubeCapture(/*host_setup=*/true, /*url_override=*/{}, /*desktop_exec=*/{},
                      /*desktop_args=*/{}, span);
}

void HostWindow::setupYouTubeCapture() {
  // Host Tools: for each span — interactive auth → token → stop → silent restore verify.
  // Default queue x1→x2→x3 so ultrawide tokens are registered in one Tools run.
  // Override with live_setup_spans=1,2 or live_setup_spans=2,3 etc.
  reloadLiveCaptureConf();
  yt_setup_queue_.clear();
  yt_setup_continue_ = false;
  {
    const QString list = monasphereConfValue(QStringLiteral("live_setup_spans")).trimmed();
    if (!list.isEmpty()) {
      for (const QString& part : list.split(QLatin1Char(','))) {
        bool ok = false;
        const int v = part.trimmed().toInt(&ok);
        if (ok && v >= 1 && v <= 3 && !yt_setup_queue_.contains(v)) yt_setup_queue_.push_back(v);
      }
    }
  }
  if (yt_setup_queue_.isEmpty()) yt_setup_queue_ = {1, 2, 3};
  QStringList shown;
  for (int s : yt_setup_queue_) shown << QStringLiteral("x%1").arg(s);
  appendLog(tr("Live setup: registering tokens for %1").arg(shown.join(QStringLiteral(" → "))));
  beginNextSetupSpan(/*first=*/true);
}

QString HostWindow::firstLiveAppUrl() const {
  if (!live_apps_.isEmpty() && !live_apps_.front().url.isEmpty()) return live_apps_.front().url;
  return QStringLiteral("https://www.youtube.com/");
}

void HostWindow::flushPendingLiveStart() {
  if (yt_pending_kind_ == YtPendingKind::None) return;
  if (!xr_running_.load(std::memory_order_acquire)) {
    appendLog(tr("Live: queued switch aborted — XR session not running (HMD lost during teardown)"));
    yt_pending_kind_ = YtPendingKind::None;
    yt_pending_url_.clear();
    yt_pending_exec_.clear();
    yt_pending_args_.clear();
    yt_pending_label_.clear();
    yt_pending_span_ = 1;
    if (yt_session_) yt_session_->clear_busy_status();
    return;
  }
  const YtPendingKind kind = yt_pending_kind_;
  const QString url = yt_pending_url_;
  const QString exec = yt_pending_exec_;
  const QStringList args = yt_pending_args_;
  const int span = yt_pending_span_;
  const QString label = yt_pending_label_;
  yt_pending_kind_ = YtPendingKind::None;
  yt_pending_url_.clear();
  yt_pending_exec_.clear();
  yt_pending_args_.clear();
  yt_pending_label_.clear();
  yt_pending_span_ = 1;
  yt_hmd_start_waits_ = 0;

  const bool kept_virtual = yt_session_ && yt_session_->virtual_display_alive();
  appendLog(kept_virtual ? tr("Live: Virtual kept → start queued %1 (x%2)")
                               .arg(kind == YtPendingKind::Live
                                        ? tr("browser")
                                        : (kind == YtPendingKind::Desktop ? tr("app") : tr("video")))
                               .arg(span)
                         : tr("Live: after Virtual gone → start queued %1 (x%2)")
                               .arg(kind == YtPendingKind::Live
                                        ? tr("browser")
                                        : (kind == YtPendingKind::Desktop ? tr("app") : tr("video")))
                               .arg(span));

  // Virtual destroy/ensure already waited for topology; start on next event-loop turn.
  auto xr_ok = [this] {
    return xr_running_.load(std::memory_order_acquire) &&
           !xr_stop_.load(std::memory_order_acquire);
  };
  if (kind == YtPendingKind::Live) {
    QTimer::singleShot(0, this, [this, url, xr_ok] {
      if (!xr_ok()) {
        appendLog(tr("Live: queued browser aborted — XR not healthy after Virtual change"));
        if (yt_session_) yt_session_->clear_busy_status();
        return;
      }
      startLiveFromHmd(url);
    });
  } else if (kind == YtPendingKind::Desktop) {
    QTimer::singleShot(0, this, [this, exec, args, span, label, xr_ok] {
      if (!xr_ok()) {
        appendLog(tr("Live: queued app aborted — XR not healthy after Virtual change"));
        if (yt_session_) yt_session_->clear_busy_status();
        return;
      }
      startLiveDesktopApp(exec, args, span, label);
    });
  } else if (kind == YtPendingKind::Video) {
    QTimer::singleShot(0, this, [this, xr_ok] {
      if (!xr_ok()) {
        appendLog(tr("Live: queued video aborted — XR not healthy after Virtual change"));
        return;
      }
      finishQueuedVideoAfterVirtualGone();
    });
  }
}

void HostWindow::finishQueuedVideoAfterVirtualGone() {
  destroyVirtualForLeaveLive();
  const auto deadline = QDateTime::currentMSecsSinceEpoch() + 8000;
  while (QDateTime::currentMSecsSinceEpoch() < deadline) {
    if (!vrp::KdeVirtualDisplay::anyManagedOutputExists()) break;
    QThread::msleep(200);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  }
  const bool gone = !vrp::KdeVirtualDisplay::anyManagedOutputExists();
  yt_await_video_after_stop_.store(false, std::memory_order_release);
  if (yt_session_) yt_session_->clear_busy_status();
  if (!gone) {
    appendLog(tr("Virtual still listed — opening queued video anyway"));
  } else {
    appendLog(tr("Virtual removed — opening queued video"));
  }
  video_pending_.store(true, std::memory_order_release);
}

void HostWindow::startLiveFromHmd(const QString& url) {
  const QString open_url = url.trimmed().isEmpty() ? firstLiveAppUrl() : url.trimmed();

  // Prefer conf on disk (Tools setup may have just rewritten it).
  reloadLiveCaptureConf();
  const QString tok = tokenForSpan(1);
  if (!tok.isEmpty()) {
    live_restore_token_ = tok;
    live_restore_tokens_.insert(1, tok);
  }

  yt_hmd_suppress_live_.store(false, std::memory_order_release);
  yt_setup_phase_ = YtSetupPhase::None;

  if (tokenForSpan(1).isEmpty()) {
    appendLog(tr("Live: no restore token for x1 — use Tools → Set up live capture on the host first "
                 "(KDE: enable Remember on the share dialog)"));
    showHudToast(tr("Host: Set up live capture first (x1)"));
    yt_pending_kind_ = YtPendingKind::None;
    if (yt_session_ &&
        (yt_session_->active() || yt_session_->loading() ||
         yt_session_->state() != vrp::YouTubeSession::State::Idle)) {
      stopYouTubeCapture();
    }
    return;
  }

  if (yt_session_ &&
      (yt_session_->active() || yt_session_->loading() ||
       yt_session_->state() != vrp::YouTubeSession::State::Idle)) {
    yt_pending_kind_ = YtPendingKind::Live;
    yt_pending_url_ = open_url;
    yt_pending_exec_.clear();
    yt_pending_args_.clear();
    yt_pending_label_.clear();
    yt_pending_span_ = 1;
    const bool reuse =
        vdisp_mgr_ && vdisp_mgr_->matches(virtualSpecForSpan(1));
    yt_session_->set_busy_status(reuse ? tr("切替中") : tr("Virtual 削除中"));
    appendLog(reuse ? tr("Live: queueing browser (Virtual reuse if same resolution)")
                    : tr("Live: queueing browser (Virtual will recreate on start)"));
    if (yt_session_->state() != vrp::YouTubeSession::State::Stopping) {
      stopYouTubeCapture();
    }
    return;
  }
  yt_hmd_start_waits_ = 0;

  const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
  if (now_ms < yt_restore_ready_ms_) {
    const int wait_ms = static_cast<int>(yt_restore_ready_ms_ - now_ms);
    QTimer::singleShot(wait_ms, this, [this, open_url] { startLiveFromHmd(open_url); });
    return;
  }

  appendLog(tr("Live HMD: %1 (token %2 chars)").arg(open_url).arg(live_restore_token_.size()));
  beginYouTubeCapture(/*host_setup=*/false, open_url);
}

void HostWindow::startLiveDesktopApp(const QString& exec, const QStringList& args, int monitor_span,
                                     const QString& label) {
  const QString open_exec = exec.trimmed();
  if (open_exec.isEmpty()) {
    appendLog(tr("Desktop app: empty exec"));
    return;
  }
  const int span = std::clamp(monitor_span > 0 ? monitor_span : 1, 1, 3);

  reloadLiveCaptureConf();
  yt_hmd_suppress_live_.store(false, std::memory_order_release);
  yt_setup_phase_ = YtSetupPhase::None;

  const QString tok = tokenForSpan(span);
  if (tok.isEmpty()) {
    appendLog(tr("Live: no restore token for x%1 — use Tools → Set up live capture at that "
                 "span on the host first (Remember on the share dialog)")
                  .arg(span));
    showHudToast(tr("Host: Set up live capture first (x%1)").arg(span));
    yt_pending_kind_ = YtPendingKind::None;
    if (yt_session_ &&
        (yt_session_->active() || yt_session_->loading() ||
         yt_session_->state() != vrp::YouTubeSession::State::Idle)) {
      stopYouTubeCapture();
    }
    return;
  }

  if (yt_session_ &&
      (yt_session_->active() || yt_session_->loading() ||
       yt_session_->state() != vrp::YouTubeSession::State::Idle)) {
    yt_pending_kind_ = YtPendingKind::Desktop;
    yt_pending_url_.clear();
    yt_pending_exec_ = open_exec;
    yt_pending_args_ = args;
    yt_pending_span_ = span;
    yt_pending_label_ = label;
    const bool reuse =
        vdisp_mgr_ && vdisp_mgr_->matches(virtualSpecForSpan(span));
    yt_session_->set_busy_status(reuse ? tr("切替中") : tr("Virtual 削除中"));
    appendLog(reuse ? tr("Live: queueing desktop app (Virtual reuse if same resolution)")
                    : tr("Live: queueing desktop app (Virtual will recreate on start)"));
    if (yt_session_->state() != vrp::YouTubeSession::State::Stopping) {
      stopYouTubeCapture();
    }
    return;
  }
  yt_hmd_start_waits_ = 0;

  const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
  if (now_ms < yt_restore_ready_ms_) {
    const int wait_ms = static_cast<int>(yt_restore_ready_ms_ - now_ms);
    QTimer::singleShot(wait_ms, this, [this, open_exec, args, span, label] {
      startLiveDesktopApp(open_exec, args, span, label);
    });
    return;
  }

  int use_w = 0, use_h = 0;
  virtualSizeForSpan(span, &use_w, &use_h);
  appendLog(tr("Desktop app HMD: %1 x%2 (%3×%4, token %5 chars)")
                .arg(label.isEmpty() ? open_exec : label)
                .arg(span)
                .arg(use_w)
                .arg(use_h)
                .arg(tok.size()));
  beginYouTubeCapture(/*host_setup=*/false, /*url_override=*/{}, open_exec, args, span, label);
}

void HostWindow::reloadLiveCaptureConf() {
  QFile user(monasphereConfPath());
  if (!user.open(QIODevice::ReadOnly | QIODevice::Text)) return;
  while (!user.atEnd()) {
    const QString line = QString::fromUtf8(user.readLine()).trimmed();
    if (line.startsWith(QStringLiteral("live_restore_token"))) {
      const QRegularExpression span_tok(
          QStringLiteral("^live_restore_token\\[x([1-3])\\]=(.*)$"));
      const QRegularExpressionMatch sm = span_tok.match(line);
      if (sm.hasMatch()) {
        const int span = sm.captured(1).toInt();
        const QString tok = sm.captured(2);
        live_restore_tokens_.insert(span, tok);
        if (span == 1) live_restore_token_ = tok;
      } else if (line.startsWith(QStringLiteral("live_restore_token="))) {
        live_restore_token_ = line.mid(19);
        live_restore_tokens_.insert(1, live_restore_token_);
      }
    } else if (line.startsWith(QStringLiteral("live_capture="))) {
      const QString v = line.mid(13).trimmed().toLower();
      live_capture_monitor_ = (v != QLatin1String("window"));
    } else if (line.startsWith(QStringLiteral("live_output="))) {
      live_output_ = line.mid(12).trimmed();
    } else if (line.startsWith(QStringLiteral("live_virtual="))) {
      bool ok = false;
      const int v = line.mid(13).toInt(&ok);
      if (ok) live_virtual_ = v != 0;
    } else if (line.startsWith(QStringLiteral("live_virtual_size="))) {
      const QString v = line.mid(18).trimmed().toLower();
      const QRegularExpression re(QStringLiteral("^(\\d+)x(\\d+)$"));
      const QRegularExpressionMatch m = re.match(v);
      if (m.hasMatch()) {
        const int w = m.captured(1).toInt();
        const int h = m.captured(2).toInt();
        if (w >= 640 && w <= 7680 && h >= 360 && h <= 4320) {
          live_virtual_w_ = w;
          live_virtual_h_ = h;
        }
      }
    } else if (line.startsWith(QStringLiteral("live_virtual_scale="))) {
      bool ok = false;
      const float v = line.mid(19).toFloat(&ok);
      if (ok && v >= 1.f && v <= 3.f) live_virtual_scale_ = v;
    } else if (line.startsWith(QStringLiteral("live_chrome_scale="))) {
      bool ok = false;
      const float v = line.mid(17).toFloat(&ok);
      if (ok && v >= 1.f && v <= 3.f) live_chrome_scale_ = v;
    } else if (line.startsWith(QStringLiteral("live_cursor_scale="))) {
      bool ok = false;
      const int v = line.mid(17).toInt(&ok);
      if (ok && v >= 1 && v <= 6) live_cursor_scale_ = v;
    } else if (line.startsWith(QStringLiteral("live_chrome_profile="))) {
      const QString v = line.mid(19).trimmed().toLower();
      live_chrome_default_profile_ = (v != QLatin1String("isolated"));
    }
  }
  syncTokensIntoManager();
}

void HostWindow::beginYouTubeCapture(bool host_setup, const QString& url_override,
                                     const QString& desktop_exec, const QStringList& desktop_args,
                                     int monitor_span, const QString& desktop_label) {
#if !defined(VRP_HAS_PIPEWIRE)
  appendLog(tr("Live capture requires PipeWire (libpipewire-0.3)"));
  return;
#else
  // Pick up conf edits (esp. live_virtual_scale / size) without restarting the app.
  reloadLiveCaptureConf();
  if (!live_capture_monitor_) {
    appendLog(tr("Live WINDOW capture disabled — MONITOR + virtual display only"));
    return;
  }
  // Invalidate stale stop fallbacks from the previous session switch.
  yt_stop_gen_.fetch_add(1, std::memory_order_acq_rel);
  const int span = std::clamp(monitor_span > 0 ? monitor_span : 1, 1, 3);
  live_session_span_ = span;
  int use_w = 0, use_h = 0;
  virtualSizeForSpan(span, &use_w, &use_h);
  appendLog(tr("Live Virtual config x%1 → %2×%3 @ scale %4 (%5)")
                .arg(span)
                .arg(use_w)
                .arg(use_h)
                .arg(QString::number(static_cast<double>(live_virtual_scale_), 'f', 2))
                .arg(vrp::KdeVirtualDisplay::outputNameForSpan(span)));
  yt_host_setup_stop_after_stream_ = host_setup;
  ensureYouTubeSession();

  if (host_setup) {
    // Force interactive picker for this run. Do not wipe monasphere.conf —
    // only replace live_restore_token[xN] when Start returns a new one.
    if (!tokenForSpan(span).isEmpty()) {
      appendLog(tr("Live host setup — interactive re-auth for x%1 (keeping other span tokens)")
                    .arg(span));
    }
    yt_hmd_suppress_live_.store(true, std::memory_order_release);
  }

  vrp::YouTubeSession::Config cfg;
  cfg.exec = expandLiveArgPath(desktop_exec.trimmed());
  cfg.exec_args = normalizeLiveExecArgs(desktop_args);
  cfg.desktop_label = desktop_label.trimmed();
  if (cfg.exec.isEmpty()) {
    cfg.url = !url_override.isEmpty() ? url_override : firstLiveAppUrl();
  } else {
    cfg.url.clear();
    if (cfg.desktop_label.isEmpty()) {
      const QFileInfo fi(cfg.exec);
      cfg.desktop_label = fi.completeBaseName().isEmpty() ? cfg.exec : fi.completeBaseName();
    }
  }
  cfg.restore_token = host_setup ? QString() : tokenForSpan(span);
  // Per-span Virtual name → independent ScreenCast Remember tokens (same name = last auth wins).
  cfg.virtual_name = vrp::KdeVirtualDisplay::nameForSpan(span);
  cfg.output_hint = vrp::KdeVirtualDisplay::outputNameForSpan(span);
  cfg.virtual_display = live_virtual_;
  cfg.virtual_w = use_w;
  cfg.virtual_h = use_h;
  cfg.virtual_scale = live_virtual_scale_;
  cfg.chrome_scale = live_chrome_scale_;
  cfg.use_default_chrome_profile = live_chrome_default_profile_;
  // Flat OpenAL live path disabled: Soft HRTF JIT SEGV while live_thread feeds
  // (cores after live_ops_mu_ serialization). Chrome stays on the system sink.
  // Re-enable only after OpenAL lifetime is proven safe on Stop.
  cfg.enable_audio = false;
  cfg.drm_modifiers = live_drm_modifiers_;
  appendLog(tr("Live audio: system sink only (Flat OpenAL deferred)"));
  if (!live_drm_modifiers_.empty()) {
    appendLog(tr("ScreenCast: announcing %1 Vulkan-sampleable DRM modifier(s)")
                  .arg(live_drm_modifiers_.size()));
  }
  if (cfg.exec.isEmpty() && live_chrome_default_profile_) {
    appendLog(tr("Chrome: using default profile (logins/cookies)"));
  }
  if (host_setup) {
    appendLog(tr("Live setup — pick %1, enable Remember/allow restore, "
                 "then capture will stop and Chrome will close")
                  .arg(cfg.output_hint));
    showHudToast(tr("Host: share %1 + Remember").arg(cfg.output_hint));
  }
  // [アプリ] pad: prefer LD_PRELOAD hook (no uinput). Legacy grab+uinput if hook .so missing.
  pad_hook_active_ = false;
  if (!cfg.exec.isEmpty()) {
    const QString hook_lib =
        QCoreApplication::applicationDirPath() + QStringLiteral("/libmonasphere_padhook.so");
    if (QFile::exists(hook_lib) && pad_hook_.start()) {
      pad_hook_active_ = true;
      cfg.pad_hook_lib = hook_lib;
      cfg.pad_hook_sock = QString::fromStdString(pad_hook_.socket_path());
      appendLog(tr("Pad hook: LD_PRELOAD %1 (PS long=menu, L3+R3=recenter; no uinput)")
                    .arg(hook_lib));
    } else if (pad_filter_.start()) {
      const QString node = QString::fromStdString(pad_filter_.uinput_event_path());
      cfg.sdl_joystick_device = node;
      appendLog(tr("Pad filter (legacy uinput): %1")
                    .arg(node.isEmpty() ? tr("monaSphere Virtual Pad") : node));
    } else {
      appendLog(tr("Pad hook unavailable — falling back to PS+R1/PS+L1 chords"));
    }
  }
  yt_session_->start(cfg);
  live_output_ = yt_session_->output_name();
#endif
}

void HostWindow::stopYouTubeCapture() {
  if (QThread::currentThread() != thread()) {
    QMetaObject::invokeMethod(this, [this] { stopYouTubeCapture(); }, Qt::QueuedConnection);
    return;
  }
  // Do not stop pad_hook here while XR still drains DMA — early stop made the XR
  // thread fall into pass-through mid-teardown. Hook/filter end in stopped().
  pad_filter_.stop();
  yt_audio_pending_log_ = false;
  yt_audio_fail_retries_ = 0;
  if (yt_session_) yt_session_->clear_audio_wanted();
  appendLog(tr("Stopping live capture… (DMA → PipeWire → app → Virtual)"));
  // While XR is running, tear down on the XR thread: reset DMA → stop PipeWire →
  // then finish portal/Chrome here. Doing capture->stop() on the UI thread races
  // VkImage imports (SEGV after tray "Stop sharing" / Tools → Stop).
  if (xr_running_.load(std::memory_order_acquire)) {
    const uint64_t stop_gen = yt_stop_gen_.fetch_add(1, std::memory_order_acq_rel) + 1;
    yt_stop_requested_.store(true, std::memory_order_release);
    // Fallback if XR never drains the flag (was wedged on audio lock).
    // Only force-stop while still Stopping — never touch a new session that already
    // started after pending was flushed (x3→x3 was killed here + Virtual destroyed).
    QTimer::singleShot(2500, this, [this, stop_gen] {
      if (yt_stop_gen_.load(std::memory_order_acquire) != stop_gen) return;
      if (!yt_session_) return;
      if (yt_session_->state() != vrp::YouTubeSession::State::Stopping) return;
      appendLog(tr("Live stop fallback (UI) — still Stopping"));
      yt_session_->stop();
      std::thread([] { (void)vrp::PulseMonitor::restore_browser_routing(); }).detach();
    });
    return;
  }
  if (yt_session_) yt_session_->stop();
  std::thread([] { (void)vrp::PulseMonitor::restore_browser_routing(); }).detach();
}

void HostWindow::showAboutDialog() {
  const QString text = tr(
      "<h3>monaSphere v%1</h3>"
      "<p>Linux VR video player (OpenXR / Monado)</p>"
      "<p>Copyright (c) 2026 flex</p>"
      "<p>GitHub:<br>"
      "<a href=\"https://github.com/3d4m0t0/monaSphere\">https://github.com/3d4m0t0/monaSphere</a></p>"
      "<p>Source license: MIT</p>"
      "<p>FFmpeg / Qt and others are linked dynamically. "
      "See THIRD_PARTY.md when distributing a binary.</p>")
                           .arg(QStringLiteral("1.0.0-dev.20261014"));
  QString html = text;
  html.replace(QStringLiteral("<h3>"), QStringLiteral("<h3 style=\"margin:0;\">"));
  html.replace(QStringLiteral("<p>"), QStringLiteral("<p style=\"margin:0;\">"));

  QDialog dlg(this);
  dlg.setWindowTitle(tr("About"));
  dlg.setWindowIcon(windowIcon());

  QTextDocument measure;
  measure.setDefaultFont(dlg.font());
  measure.setDocumentMargin(0);
  measure.setHtml(html);
  const int text_w = static_cast<int>(measure.idealWidth()) + 2;

  auto* icon = new QLabel(&dlg);
  const QPixmap pixmap =
      QPixmap(QStringLiteral(":/icons/monasphere.png")).scaled(128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation);
  icon->setPixmap(pixmap);
  icon->setFixedSize(128, 128);

  auto* body = new QLabel(html, &dlg);
  body->setTextFormat(Qt::RichText);
  body->setTextInteractionFlags(Qt::TextBrowserInteraction);
  body->setOpenExternalLinks(true);
  body->setAlignment(Qt::AlignLeft | Qt::AlignTop);
  body->setWordWrap(false);
  body->setMargin(0);
  body->setFixedWidth(text_w);
  if (auto* doc = body->findChild<QTextDocument*>()) doc->setDocumentMargin(0);

  auto* row = new QHBoxLayout();
  row->setSpacing(8);
  row->addWidget(icon, 0, Qt::AlignTop);
  row->addWidget(body, 0, Qt::AlignTop);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok, &dlg);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);

  auto* layout = new QVBoxLayout(&dlg);
  layout->setContentsMargins(8, 8, 8, 8);
  layout->setSpacing(6);
  layout->setSizeConstraint(QLayout::SetFixedSize);
  layout->addLayout(row);
  layout->addWidget(buttons);
  dlg.exec();
}

void HostWindow::rebuildConnectDeviceMenu() {
  if (!connect_menu_) return;
  connect_menu_->clear();
  if (usingWivrn()) {
    auto* a = connect_menu_->addAction(tr("WiVRn headset"));
    a->setEnabled(connect_ui_enabled_);
    connect(a, &QAction::triggered, this, [this] { connectHeadset(); });
    return;
  }
  const auto devices = listConnectedHmds();
  if (devices.empty()) {
    auto* none = connect_menu_->addAction(tr("(none)"));
    none->setEnabled(false);
    return;
  }
  for (const auto& d : devices) {
    auto* a = connect_menu_->addAction(d.label);
    a->setEnabled(connect_ui_enabled_);
    connect(a, &QAction::triggered, this, [this, label = d.label] {
          connectHeadset();
    });
  }
  if (devices.size() >= 2) {
    connect_menu_->addSeparator();
    auto* note = connect_menu_->addAction(tr("One HMD only"));
    note->setEnabled(false);
  }
}

void HostWindow::rebuildAudioMenu() {
  if (!audio_menu_ || !audio_group_) return;
  if (audio_device_ids_.isEmpty()) refreshAudioDevices(false);
  const auto old_actions = audio_group_->actions();
  for (QAction* a : old_actions) {
    audio_group_->removeAction(a);
    audio_menu_->removeAction(a);
    a->deleteLater();
  }
  audio_menu_->clear();
  if (audio_device_ids_.isEmpty()) {
    auto* none = audio_menu_->addAction(tr("No device"));
    none->setEnabled(false);
    return;
  }
  auto* refresh = audio_menu_->addAction(tr("Reload"));
  connect(refresh, &QAction::triggered, this, [this] {
    refreshAudioDevices(false);
    rebuildAudioMenu();
  });
  audio_menu_->addSeparator();
  for (int i = 0; i < audio_device_ids_.size(); ++i) {
    auto* a = audio_menu_->addAction(audio_device_labels_.value(i));
    a->setCheckable(true);
    a->setData(audio_device_ids_[i]);
    audio_group_->addAction(a);
    if (audio_device_ids_[i] == selected_audio_device_) a->setChecked(true);
  }
}

void HostWindow::rebuildFsrMenu() {
  if (!fsr_menu_ || !fsr_group_) return;
  const auto old = fsr_group_->actions();
  for (QAction* a : old) {
    fsr_group_->removeAction(a);
    fsr_menu_->removeAction(a);
    a->deleteLater();
  }
  fsr_menu_->clear();
  struct Item {
    vrp::FsrMode mode;
    const char* label;
  };
  const Item items[] = {
      {vrp::FsrMode::Off, QT_TR_NOOP("Off")},
      {vrp::FsrMode::UltraQuality, QT_TR_NOOP("UQ (~1.3x)")},
      {vrp::FsrMode::Quality, QT_TR_NOOP("Q (~1.5x)")},
      {vrp::FsrMode::Performance, QT_TR_NOOP("P (~2.0x)")},
  };
  for (const auto& it : items) {
    auto* a = fsr_menu_->addAction(tr(it.label));
    a->setCheckable(true);
    a->setData(static_cast<int>(it.mode));
    fsr_group_->addAction(a);
    if (it.mode == preferred_fsr_) a->setChecked(true);
  }
}

void HostWindow::rebuildHzMenu() {
  if (!hz_menu_ || !hz_group_) return;
  const auto old = hz_group_->actions();
  for (QAction* a : old) {
    hz_group_->removeAction(a);
    hz_menu_->removeAction(a);
    a->deleteLater();
  }
  hz_menu_->clear();
  struct Item {
    int hz;
    const char* label;
  };
  const Item items[] = {
      {0, QT_TR_NOOP("Auto (90 Hz)")},
      {90, QT_TR_NOOP("90 Hz")},
      {120, QT_TR_NOOP("120 Hz")},
  };
  for (const auto& it : items) {
    auto* a = hz_menu_->addAction(tr(it.label));
    a->setCheckable(true);
    a->setData(it.hz);
    hz_group_->addAction(a);
    if (it.hz == preferred_hz_) a->setChecked(true);
  }
}

void HostWindow::onAudioDeviceAction(QAction* action) {
  if (!action) return;
  const QString id = action->data().toString();
  if (id.isEmpty()) return;
  selected_audio_device_ = id;
  {
    std::lock_guard<std::mutex> lock(audio_mu_);
    audio_device_ = id.toStdString();
  }
  audio_user_override_.store(true);
  audio_reopen_.store(true);
  appendLog(tr("Audio: %1").arg(action->text()));
}

void HostWindow::onFsrAction(QAction* action) {
  if (!action) return;
  const auto mode = static_cast<vrp::FsrMode>(action->data().toInt());
  preferred_fsr_ = mode;
  saveAppConf();
  fsr_req_.store(static_cast<int>(mode));
  appendLog(QStringLiteral("FSR1: %1").arg(QString::fromUtf8(vrp::fsr_mode_name(mode))));
  {
    std::lock_guard<std::mutex> lock(menu_mu_);
    if (vr_menu_) vr_menu_->set_fsr(mode);
  }
  requestHudPaint();
}

void HostWindow::onHzAction(QAction* action) {
  if (!action) return;
  const int hz = action->data().toInt();
  preferred_hz_ = hz;
  setMonasphereConfKey(QStringLiteral("hz"), QString::number(hz));
  monado_hz_restarted_ = false;
  monado_hz_mismatch_retried_ = false;
  {
    std::lock_guard<std::mutex> lock(menu_mu_);
    if (vr_menu_) vr_menu_->set_preferred_hz(hz);
  }
  requestHudPaint();
  applyPreferredRefreshRate(/*from_ui=*/true, /*force_monado_restart=*/true);
}

void HostWindow::setSessionChrome(bool session_active) {
  connect_ui_enabled_ = !session_active;
  if (connect_menu_) {
    connect_menu_->setEnabled(!session_active);
    rebuildConnectDeviceMenu();
  }
  if (act_disconnect_) act_disconnect_->setEnabled(session_active);
  if (controller_menu_) controller_menu_->setEnabled(!session_active);
  if (hand_menu_) hand_menu_->setEnabled(!session_active);
}

void HostWindow::onControllerProfileAction(QAction* action) {
  if (!action) return;
  opt_.controller_profile = static_cast<vrp::ControllerProfile>(action->data().toInt());
  setMonasphereConfKey(QStringLiteral("pad"),
                       QString::fromUtf8(vrp::controller_profile_name(opt_.controller_profile)));
  appendLog(tr("Pad: %1 (applies on reconnect)").arg(QString::fromUtf8(vrp::controller_profile_name(opt_.controller_profile))));
}

void HostWindow::onHandAction(QAction* action) {
  if (!action) return;
  opt_.controller_hand = static_cast<vrp::ControllerHand>(action->data().toInt());
  setMonasphereConfKey(QStringLiteral("hand"),
                       QString::fromUtf8(vrp::controller_hand_name(opt_.controller_hand)));
  appendLog(tr("Hand: %1 (applies on reconnect)").arg(QString::fromUtf8(vrp::controller_hand_name(opt_.controller_hand))));
}

void HostWindow::appendLog(const QString& line) {
  if (shutting_down_.load() || !log_) return;
  log_->appendPlainText(line);
}

void HostWindow::presentToFront(const QString& activation_token) {
  if (shutting_down_.load()) return;
  if (isMinimized()) showNormal();
  show();
  raise();

  QWindow* handle = windowHandle();
  bool activated = false;
  if (handle && QGuiApplication::platformName() == QLatin1String("wayland") && !activation_token.isEmpty()) {
#if defined(VRP_HAS_QT_GUI_PRIVATE)
    // QWaylandWindow stores the token and the next requestActivate() spends it on
    // xdg_activation_v1. A token requested by the already-focused app is rejected.
    auto* native = handle->nativeInterface<QNativeInterface::Private::QWaylandWindow>();
    using SetToken = void (*)(void*, const QString&);
    static auto set_token = [] {
      void* lib = dlopen("libQt6WaylandClient.so.6", RTLD_LAZY | RTLD_GLOBAL);
      if (!lib) return static_cast<SetToken>(nullptr);
      return reinterpret_cast<SetToken>(
          dlsym(lib, "_ZN15QtWaylandClient14QWaylandWindow21setXdgActivationTokenERK7QString"));
    }();
    if (native && set_token) {
      set_token(native, activation_token);
      handle->requestActivate();
      activated = true;
      VRP_LOG("Wayland activate with launcher token");
    } else {
      VRP_LOG("Wayland activation token was not applied (window=%d symbol=%d)", native ? 1 : 0,
              set_token ? 1 : 0);
    }
#else
    VRP_LOG("Wayland activation token ignored (Qt GuiPrivate not available)");
#endif
  }
  if (!activated) {
    activateWindow();
    if (handle) handle->requestActivate();
    if (QGuiApplication::platformName() == QLatin1String("wayland")) {
      VRP_LOG("Wayland activate without XDG_ACTIVATION_TOKEN");
    }
  }
  appendLog(tr("Already running — brought to front"));
}

namespace {
QString UiTr(const QString& s) {
  if (s.isEmpty()) return s;
  const QByteArray u = s.toUtf8();
  return QCoreApplication::translate("HostWindow", u.constData());
}
}  // namespace

void HostWindow::setHealth(const QString& health, const QString& detail) {
  const QString h = UiTr(health);
  const QString d = UiTr(detail);
  health_label_->setText(d.isEmpty() ? h : h + QStringLiteral(" — ") + d);
  if (health == QLatin1String("OK")) {
    health_label_->setStyleSheet("color: #3dd68c; font-weight: 600;");
  } else if (health == QLatin1String("Error")) {
    health_label_->setStyleSheet("color: #ff6b6b; font-weight: 600;");
  } else {
    health_label_->setStyleSheet("color: #e6c15a; font-weight: 600;");
  }
}

void HostWindow::applyStatus(const QString& hmd, const QString& session, const QString& health,
                             const QString& detail, bool tracking, int width, int height,
                             double display_hz, double frame_ms, const QString& sync_note,
                             bool openxr_hz_switchable, const QString& available_hz) {
  // Drop late XR status ticks after the session has already ended.
  if (!xr_running_.load()) return;
  (void)session;
  (void)tracking;
  (void)width;
  (void)height;
  (void)openxr_hz_switchable;
  (void)available_hz;

  const bool hmd_changed = !hmd.isEmpty() && hmd != last_xr_system_;
  if (hmd.isEmpty()) hmd_label_->setText(tr("HMD: none"));
  else hmd_label_->setText(tr("HMD: %1").arg(hmd));

  const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
  const bool incoming_lag = sync_note == QLatin1String("lag");
  if (incoming_lag) sync_lag_hold_until_ms_ = now_ms + 1000;
  const bool show_lag = incoming_lag || now_ms < sync_lag_hold_until_ms_;
  QString note = QStringLiteral("—");
  if (show_lag) note = tr("lag");
  else if (sync_note == QLatin1String("OK")) note = tr("sync OK");
  else if (!sync_note.isEmpty()) note = UiTr(sync_note);

  QString hz_text;
  if (display_hz > 0.5) {
    hz_text = QStringLiteral("%1 Hz").arg(display_hz, 0, 'f', 0);
  } else {
    hz_text = QStringLiteral("- Hz");
  }
  QString ms_text;
  if (frame_ms >= 0.05) {
    ms_text = tr("%1 ms").arg(frame_ms, 0, 'f', 1);
  } else {
    ms_text = tr("— ms");
  }
  if (display_hz > 0.5) {
    const double limit_ms = (1000.0 / display_hz) * 1.15;
    ms_text += tr(" (≤%1)").arg(limit_ms, 0, 'f', 1);
  }
  const QString sync = hz_text + QStringLiteral(" — ") + ms_text + QStringLiteral(" — ") + note;
  if (sync_status_label_) {
    sync_status_label_->setText(sync);
    if (show_lag) {
      sync_status_label_->setStyleSheet("color: #ff6b6b; font-weight: 600;");
    } else if (sync_note == QLatin1String("OK")) {
      sync_status_label_->setStyleSheet("color: #3dd68c; font-weight: 600;");
    } else {
      sync_status_label_->setStyleSheet({});
    }
  }
  setHealth(health, detail);
  if (hmd_changed) {
    applyHmdAudioRoute(hmd);
  }
}

void HostWindow::refreshAudioDevices() { refreshAudioDevices(false); }

void HostWindow::refreshAudioDevices(bool prefer_hmd) {
  const QString prev = selectedAudioDevice();
  const auto devices = vrp::AudioPlayer::list_devices();
  audio_device_ids_.clear();
  audio_device_labels_.clear();
  int hmd_count = 0;
  int tv_count = 0;
  for (const auto& d : devices) {
    const QString name = QString::fromStdString(d.name);
    const QString label = d.description.empty()
                              ? name
                              : QString::fromStdString(d.description) + "  [" + name + "]";
    audio_device_ids_.append(name);
    audio_device_labels_.append(label);
    if (d.kind == vrp::AudioRouteKind::HmdHdmi) ++hmd_count;
    if (d.kind == vrp::AudioRouteKind::TvHdmi) ++tv_count;
  }
  (void)tv_count;

  int idx = -1;
  // WiVRn's virtual sink appears only after the headset app connects.
  const bool want_wivrn = usingWivrn() && !audio_user_override_.load();
  const std::string wivrn_pick = want_wivrn ? vrp::AudioPlayer::find_wivrn_device(devices) : std::string{};
  if (!wivrn_pick.empty()) {
    idx = audio_device_ids_.indexOf(QString::fromStdString(wivrn_pick));
    if (idx >= 0 && audio_device_ids_[idx] != prev) {
      appendLog(tr("Audio → WiVRn: %1").arg(audio_device_labels_[idx]));
    }
  }

  // Only auto-pick HMD HDMI while connecting/connected — not after disconnect.
  const bool pick_hmd = !want_wivrn && (prefer_hmd || (xr_running_.load() && !last_xr_system_.isEmpty() &&
                                                       !audio_user_override_.load()));
  if (idx < 0 && pick_hmd && !audio_user_override_.load()) {
    const std::string pick =
        vrp::AudioPlayer::prefer_hmd_device(devices, last_xr_system_.toStdString());
    if (!pick.empty()) {
      idx = audio_device_ids_.indexOf(QString::fromStdString(pick));
      if (idx >= 0) {
        audio_user_override_.store(false);
        if (audio_device_ids_[idx] != prev) {
          appendLog(tr("Audio → HMD: %1").arg(audio_device_labels_[idx]));
        }
      }
    }
  }

  const bool wivrn_sink_gone =
      want_wivrn && wivrn_pick.empty() && prev.contains(QStringLiteral("wivrn"), Qt::CaseInsensitive);
  // After disconnect: prefer a non-HMD sink so PSVR2 HDMI / WiVRn does not stick.
  if (idx < 0 && !pick_hmd && (!want_wivrn || wivrn_sink_gone)) {
    for (const auto& d : devices) {
      if (d.kind == vrp::AudioRouteKind::HmdHdmi) continue;
      if (vrp::AudioPlayer::is_wivrn_device(d)) continue;
      idx = audio_device_ids_.indexOf(QString::fromStdString(d.name));
      if (idx >= 0) break;
    }
    if (idx >= 0 && audio_device_ids_[idx] != prev) {
      appendLog(tr("Audio → desktop: %1").arg(audio_device_labels_[idx]));
    }
  }

  if (idx < 0) {
    const int prev_idx = audio_device_ids_.indexOf(prev);
    if (prev_idx >= 0) {
      bool prev_is_hmd = false;
      for (const auto& d : devices) {
        if (QString::fromStdString(d.name) == prev &&
            (d.kind == vrp::AudioRouteKind::HmdHdmi || vrp::AudioPlayer::is_wivrn_device(d))) {
          prev_is_hmd = true;
          break;
        }
      }
      if (pick_hmd || !prev_is_hmd) idx = prev_idx;
    }
  }
  if (idx < 0 && !audio_device_ids_.isEmpty()) idx = 0;
  if (idx >= 0) selected_audio_device_ = audio_device_ids_[idx];
  else selected_audio_device_ = QStringLiteral("default");

  {
    std::lock_guard<std::mutex> lock(audio_mu_);
    audio_device_ = selected_audio_device_.toStdString();
  }
  if (selected_audio_device_ != prev && xr_running_.load()) {
    audio_reopen_.store(true);
  }
  if (pick_hmd && hmd_count > 0) {
    audio_reopen_.store(true);
  }
  if (hmd_count == 0 && pick_hmd) {
    appendLog(tr("No HMD HDMI audio"));
  }
}

void HostWindow::applyHmdAudioRoute(const QString& xr_system_name) {
  if (!xr_system_name.isEmpty() && xr_system_name != last_xr_system_) {
    last_xr_system_ = xr_system_name;
  }
  if (audio_user_override_.load()) return;
  refreshAudioDevices(true);
}

QString HostWindow::selectedAudioDevice() const {
  if (!selected_audio_device_.isEmpty()) return selected_audio_device_;
  return QStringLiteral("default");
}

void HostWindow::reportVideoOpen(const vrp::VideoInfo& info) {
  const QString path = QString::fromStdString(info.path);
  if (!info.ok) {
    const QString err = QString::fromStdString(info.error.empty() ? "unknown error" : info.error);
    appendLog(tr("Cannot play: %1").arg(path));
    appendLog(tr("Reason: %1").arg(err));
    return;
  }
  appendLog(tr("Video: %1 — %2")
                .arg(path)
                .arg(QString::fromStdString(info.summary_line())));
}

void HostWindow::reportThumbnails(const vrp::ThumbnailStrip& strip) {
  if (strip.ready) {
    appendLog(tr("Seek thumbs ready: %1").arg(QString::fromStdString(strip.status)));
  } else {
    appendLog(tr("Seek thumbs failed: %1").arg(QString::fromStdString(strip.status)));
  }
}

// Same strip the XR playback path starts. Count and long edge come from monasphere.conf.
void HostWindow::beginThumbnailBuild(const std::string& path) {
  if (path.empty()) return;
  thumb_cancel_.store(true);
  if (thumb_thread_.joinable()) thumb_thread_.detach();
  thumb_cancel_.store(false);

  const int thumb_count = seek_thumb_count_;
  const int thumb_edge = seek_thumb_edge_;
  const bool allow_hw = hwaccel_;
  thumb_thread_ = std::thread([this, path, thumb_count, thumb_edge, allow_hw] {
    vrp::ThumbnailStrip strip =
        vrp::build_thumbnail_strip(path, thumb_count, 0, 0, {}, {}, thumb_edge, allow_hw);
    if (thumb_cancel_.load() || shutting_down_.load()) return;
    QMetaObject::invokeMethod(
        this,
        [this, strip = std::move(strip)] {
          if (shutting_down_.load()) return;
          reportThumbnails(strip);
        },
        Qt::QueuedConnection);
  });
}

void HostWindow::onRuntimeChosen(bool wivrn) {
  if (runtime_ui_guard_) return;
  setRuntimeKind(wivrn);
}

void HostWindow::syncRuntimeRadios() {
  runtime_ui_guard_ = true;
  if (runtime_monado_rb_) runtime_monado_rb_->setChecked(!usingWivrn());
  if (runtime_wivrn_rb_) runtime_wivrn_rb_->setChecked(usingWivrn());
  runtime_ui_guard_ = false;
  refreshRuntimeActions();
}

void HostWindow::refreshRuntimeActions() {
  if (!act_start_runtime_ || !act_stop_runtime_) return;
  if (usingWivrn()) {
    act_start_runtime_->setText(tr("Start WiVRn"));
    act_stop_runtime_->setText(tr("Stop WiVRn"));
  } else {
    act_start_runtime_->setText(tr("Start Monado"));
    act_stop_runtime_->setText(tr("Stop Monado"));
  }
}

void HostWindow::releaseRuntimeForSwitch() {
  if (xr_running_.load() || xr_thread_.joinable()) {
    appendLog(tr("Disconnecting HMD before runtime switch"));
    if (!shutdownXrSession(8000, /*stop_monado=*/false)) {
      if (xr_thread_.joinable()) xr_thread_.detach();
      xr_running_.store(false);
    }
    if (!shutting_down_.load()) syncDisconnectedUi(tr("Session ended"));
  }
  last_usb_hmd_count_ = -1;

  // Leave an already-running monado-service / wivrn-server (and its user unit) alone.
  if (monado_external_ || !monado_ || monado_->state() == QProcess::NotRunning) return;

  appendLog(usingWivrn() ? tr("Stopping WiVRn") : tr("Stopping Monado"));
  stopMonadoProcess();
  if (monado_label_) monado_label_->setText(tr("Stopped"));
}

void HostWindow::setRuntimeKind(bool wivrn) {
  const int next = wivrn ? 1 : 0;
  if (runtime_kind_.load() == next) {
    refreshRuntimeActions();
    return;
  }
  releaseRuntimeForSwitch();
  runtime_kind_.store(next);
  rebuildConnectDeviceMenu();
  runtime_json_ = findRuntimeJson();
  if (!runtime_json_.isEmpty()) {
    qputenv("XR_RUNTIME_JSON", runtime_json_.toUtf8());
    if (runtime_label_) runtime_label_->setText(runtime_json_);
  } else {
    qunsetenv("XR_RUNTIME_JSON");
    if (runtime_label_) runtime_label_->clear();
  }
  if (usingWivrn()) qunsetenv("XRT_COMPOSITOR_DESIRED_MODE");
  refreshRuntimeActions();
  saveAppConf();
  appendLog(usingWivrn() ? tr("Runtime: WiVRn") : tr("Runtime: Monado"));
  if (usingWivrn()) {
    audio_user_override_.store(false);
    refreshAudioDevices(false);
    if (!shutting_down_.load()) startOwnedWivrn(/*allow_external_adopt=*/true);
    return;
  }
  if (selected_audio_device_.contains(QStringLiteral("wivrn"), Qt::CaseInsensitive)) {
    audio_user_override_.store(false);
    refreshAudioDevices(false);
  }
  if (!shutting_down_.load() && !xr_running_.load()) {
    setHealth("Wait", QStringLiteral("Waiting for USB HMD"));
  }
  if (!user_suppressed_auto_connect_ && !shutting_down_.load()) {
    QTimer::singleShot(400, this, [this] { maybeAutoConnectHeadset(); });
  }
}

QString HostWindow::findMonadoBinary() const {
  const QString from_path = QStandardPaths::findExecutable("monado-service");
  if (!from_path.isEmpty()) return from_path;
  const char* extra[] = {"/usr/local/bin/monado-service", "/usr/bin/monado-service", nullptr};
  for (int i = 0; extra[i]; ++i) {
    if (QFileInfo::exists(extra[i])) return extra[i];
  }
  return {};
}

QString HostWindow::findWivrnBinary() const {
  const QString from_path = QStandardPaths::findExecutable(QStringLiteral("wivrn-server"));
  if (!from_path.isEmpty()) return from_path;
  const char* extra[] = {"/usr/local/bin/wivrn-server", "/usr/bin/wivrn-server", nullptr};
  for (int i = 0; extra[i]; ++i) {
    if (QFileInfo::exists(extra[i])) return extra[i];
  }
  return {};
}

QString HostWindow::findRuntimeJson() const {
  if (usingWivrn()) {
    const char* paths[] = {
        "/usr/local/share/openxr/1/openxr_wivrn.json",
        "/usr/share/openxr/1/openxr_wivrn.json",
        nullptr,
    };
    for (int i = 0; paths[i]; ++i) {
      if (QFileInfo::exists(paths[i])) return paths[i];
    }
    return {};
  }
  const QByteArray env = qgetenv("XR_RUNTIME_JSON");
  if (!env.isEmpty() && QFileInfo::exists(env) &&
      !QString::fromUtf8(env).contains(QStringLiteral("wivrn"))) {
    return QString::fromUtf8(env);
  }
  const char* paths[] = {
      "/usr/local/share/openxr/1/openxr_monado.json",
      "/usr/share/openxr/1/openxr_monado.json",
      nullptr,
  };
  for (int i = 0; paths[i]; ++i) {
    if (QFileInfo::exists(paths[i])) return paths[i];
  }
  return {};
}

QString HostWindow::monadoIpcPath() const {
  const QByteArray runtime = qgetenv("XDG_RUNTIME_DIR");
  if (!runtime.isEmpty()) {
    return QString::fromUtf8(runtime) + "/monado_comp_ipc";
  }
  return QString("/run/user/%1/monado_comp_ipc").arg(getuid());
}

bool HostWindow::isMonadoIpcLive() const {
  const QString path = monadoIpcPath();
  if (!QFileInfo::exists(path)) return false;

  const QByteArray path_bytes = path.toUtf8();
  if (path_bytes.size() >= static_cast<int>(sizeof(sockaddr_un{}.sun_path))) return false;

  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return false;

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, path_bytes.constData(), static_cast<size_t>(path_bytes.size()) + 1);
  const bool ok = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
  ::close(fd);
  return ok;
}

bool HostWindow::clearStaleMonadoSocket() {
  const QString path = monadoIpcPath();
  if (!QFileInfo::exists(path)) return true;
  if (isMonadoIpcLive()) return false;
  if (QFile::remove(path)) {
    appendLog(tr("Removed stale Monado IPC: %1").arg(path));
    return true;
  }
  appendLog(tr("Cannot remove Monado IPC: %1").arg(path));
  return false;
}

bool HostWindow::adoptExternalMonado() {
  if (!isMonadoIpcLive()) return false;
  monado_external_ = true;
  monado_label_->setText(tr("External"));
  setHealth("Wait", "Using existing Monado");
  monado_multi_hmd_logged_ = false;
  appendLog(tr("Using external Monado (%1)").arg(monadoIpcPath()));
  QTimer::singleShot(300, this, [this] { maybeAutoConnectHeadset(); });
  return true;
}

bool HostWindow::isWivrnServerLive() const {
  if (monado_ && monado_->state() != QProcess::NotRunning) return true;
  QProcess probe;
  probe.setProcessEnvironment(hostToolEnvironment());
  probe.start(QStringLiteral("pgrep"), {QStringLiteral("-x"), QStringLiteral("wivrn-server")});
  if (!probe.waitForFinished(800)) return false;
  return probe.exitCode() == 0;
}

bool HostWindow::adoptExternalWivrn() {
  if (monado_ && monado_->state() != QProcess::NotRunning) return true;
  if (!isWivrnServerLive()) return false;
  monado_external_ = true;
  monado_label_->setText(tr("External"));
  setHealth("Wait", "Using existing WiVRn");
  appendLog(tr("Using external WiVRn"));
  appendLog(tr("Start the headset app, then Connect"));
  return true;
}

void HostWindow::startOwnedWivrn(bool allow_external_adopt) {
  if (monado_->state() != QProcess::NotRunning) {
    appendLog(tr("WiVRn already running"));
    return;
  }
  if (allow_external_adopt && adoptExternalWivrn()) return;

  const QString bin = findWivrnBinary();
  if (bin.isEmpty()) {
    monado_label_->setText(tr("No binary"));
    setHealth("Error", "wivrn-server not found");
    appendLog(tr("wivrn-server not in PATH"));
    return;
  }
  runtime_json_ = findRuntimeJson();
  if (runtime_json_.isEmpty()) {
    appendLog(tr("openxr_wivrn.json not found"));
  }
  QProcessEnvironment env = hostToolEnvironment();
  env.remove(QStringLiteral("XRT_COMPOSITOR_DESIRED_MODE"));
  qunsetenv("XRT_COMPOSITOR_DESIRED_MODE");
  if (!runtime_json_.isEmpty()) {
    env.insert(QStringLiteral("XR_RUNTIME_JSON"), runtime_json_);
    qputenv("XR_RUNTIME_JSON", runtime_json_.toUtf8());
    runtime_label_->setText(runtime_json_);
  }
  monado_external_ = false;
  monado_->setProcessEnvironment(env);
  monado_->setProgram(bin);
  monado_->setArguments({});
  appendLog(tr("Start WiVRn: %1").arg(bin));
  monado_->start();
  if (!monado_->waitForStarted(3000)) {
    monado_label_->setText(tr("Start failed"));
    setHealth("Error", monado_->errorString());
    appendLog(tr("Start failed: %1").arg(monado_->errorString()));
    return;
  }
  monado_label_->setText(tr("Running pid %1").arg(monado_->processId()));
  setHealth("Wait", "WiVRn up. Waiting for headset");
  appendLog(tr("Start the headset app, then Connect"));
}

void HostWindow::applyMonadoPacingEnv(QProcessEnvironment& env) const {
  if (!env.contains("XRT_COMPOSITOR_USE_PRESENT_WAIT")) {
    env.insert("XRT_COMPOSITOR_USE_PRESENT_WAIT", "1");
  }
  if (!env.contains("U_PACING_COMP_TIME_FRACTION_PERCENT")) {
    env.insert("U_PACING_COMP_TIME_FRACTION_PERCENT", "90");
  }
  // Needed to discover 90/120 Hz mode indices from Monado logs.
  env.insert("XRT_COMPOSITOR_PRINT_MODES", "1");
  if (monado_desired_mode_ >= 0) {
    env.insert("XRT_COMPOSITOR_DESIRED_MODE", QString::number(monado_desired_mode_));
  } else {
    env.remove("XRT_COMPOSITOR_DESIRED_MODE");
  }
}

void HostWindow::resolvePreferredDisplayMode() {
  if (usingWivrn() || monado_desired_mode_ >= 0) return;
  if (monado_mode_hz_.isEmpty()) ensureMonadoModeTable();
  const int idx = findModeIndexForHz(effectivePreferredHz());
  if (idx >= 0) monado_desired_mode_ = idx;
}

int HostWindow::findModeIndexForHz(int hz) const {
  if (hz <= 0 || monado_mode_hz_.isEmpty()) return -1;
  int best = -1;
  double best_err = 1e9;
  qint64 best_pixels = -1;
  for (auto it = monado_mode_hz_.constBegin(); it != monado_mode_hz_.constEnd(); ++it) {
    const double err = std::abs(it.value() - static_cast<double>(hz));
    if (err > 3.0) continue;
    const qint64 pixels = monado_mode_pixels_.value(it.key(), 0);
    // Prefer closer Hz; on tie prefer higher resolution (PSVR2 90/120 share panel size).
    if (err < best_err - 0.05 || (std::abs(err - best_err) <= 0.05 && pixels > best_pixels)) {
      best_err = err;
      best_pixels = pixels;
      best = it.key();
    }
  }
  return best;
}

void HostWindow::loadMonadoModeTable() {
  QString raw = monasphereConfValue(QStringLiteral("modes"));
  if (raw.isEmpty()) {
    raw = legacyQtValue(QStringLiteral("monado"), QStringLiteral("mode_table"));
    if (!raw.isEmpty()) setMonasphereConfKey(QStringLiteral("modes"), raw);
  }
  if (raw.isEmpty()) return;
  monado_mode_hz_.clear();
  monado_mode_pixels_.clear();
  for (const QString& part : raw.split(';', Qt::SkipEmptyParts)) {
    const QStringList f = part.split(':');
    if (f.size() < 2) continue;
    const int idx = f[0].toInt();
    const double hz = f[1].toDouble();
    const qint64 pixels = f.size() >= 3 ? f[2].toLongLong() : 0;
    if (idx < 0 || hz <= 0.0) continue;
    monado_mode_hz_.insert(idx, hz);
    if (pixels > 0) monado_mode_pixels_.insert(idx, pixels);
  }
}

void HostWindow::saveMonadoModeTable() const {
  if (monado_mode_hz_.isEmpty()) return;
  QStringList parts;
  for (auto it = monado_mode_hz_.constBegin(); it != monado_mode_hz_.constEnd(); ++it) {
    parts << QStringLiteral("%1:%2:%3")
                 .arg(it.key())
                 .arg(it.value(), 0, 'f', 3)
                 .arg(monado_mode_pixels_.value(it.key(), 0));
  }
  setMonasphereConfKey(QStringLiteral("modes"), parts.join(';'));
}

bool HostWindow::ensureMonadoModeTable() {
  if (!monado_mode_hz_.isEmpty()) return true;
  loadMonadoModeTable();
  if (!monado_mode_hz_.isEmpty()) return true;

  // Probe HMD VkDisplay modes before Monado start (PRINT_MODES only appears after compositor init).
  QProcess probe;
  probe.setProcessEnvironment(hostToolEnvironment());
  probe.start(QStringLiteral("vulkaninfo"), {});
  if (!probe.waitForStarted(2000)) {
    appendLog(tr("vulkaninfo failed — wait for Monado modes"));
    return false;
  }
  if (!probe.waitForFinished(20000)) {
    probe.kill();
    appendLog(tr("vulkaninfo timeout — wait for Monado modes"));
    return false;
  }
  const QString out = QString::fromLocal8Bit(probe.readAllStandardOutput()) +
                      QString::fromLocal8Bit(probe.readAllStandardError());

  // Prefer known HMDs; fall back to any non-TV-looking display with VR-sized modes.
  static const QRegularExpression hmd_name_re(
      QStringLiteral(R"(displayName\s*=\s*(.+)$)"), QRegularExpression::CaseInsensitiveOption);
  static const QRegularExpression mode_re(
      QStringLiteral(R"(Display Mode\s*:\s*(\d+)\s*\((\d+)\s*x\s*(\d+)\))"),
      QRegularExpression::CaseInsensitiveOption);
  static const QRegularExpression rate_re(
      QStringLiteral(R"(refreshRate\s*=\s*(\d+))"), QRegularExpression::CaseInsensitiveOption);

  QMap<int, double> best_hz;
  QMap<int, qint64> best_px;
  QMap<int, double> cur_hz;
  QMap<int, qint64> cur_px;
  bool in_hmd = false;
  int pending_mode = -1;
  int pending_w = 0, pending_h = 0;
  int hmd_score = -1;
  int best_score = -1;

  auto score_name = [](const QString& lower) -> int {
    if (lower.contains(QStringLiteral("ps vr")) || lower.contains(QStringLiteral("psvr"))) return 100;
    if (lower.contains(QStringLiteral("sony")) && lower.contains(QStringLiteral("vr"))) return 90;
    if (lower.contains(QStringLiteral("vive")) || lower.contains(QStringLiteral("index")) ||
        lower.contains(QStringLiteral("beyond")) || lower.contains(QStringLiteral("quest")))
      return 50;
    if (lower.contains(QStringLiteral("hmd"))) return 20;
    return -1;  // TV / monitor — ignore
  };

  auto flush_block = [&] {
    if (cur_hz.isEmpty() || hmd_score < 0) return;
    if (hmd_score > best_score) {
      best_score = hmd_score;
      best_hz = cur_hz;
      best_px = cur_px;
    }
  };

  for (QString line : out.split('\n')) {
    line = line.trimmed();
    const auto nm = hmd_name_re.match(line);
    if (nm.hasMatch()) {
      flush_block();
      cur_hz.clear();
      cur_px.clear();
      pending_mode = -1;
      const QString name = nm.captured(1).trimmed();
      hmd_score = score_name(name.toLower());
      in_hmd = hmd_score >= 0;
      continue;
    }
    if (!in_hmd) continue;
    const auto mm = mode_re.match(line);
    if (mm.hasMatch()) {
      pending_mode = mm.captured(1).toInt();
      pending_w = mm.captured(2).toInt();
      pending_h = mm.captured(3).toInt();
      continue;
    }
    if (pending_mode >= 0) {
      const auto rm = rate_re.match(line);
      if (rm.hasMatch()) {
        // vulkaninfo refreshRate is milli-Hz (89910 → 89.91 Hz).
        const double hz = rm.captured(1).toDouble() / 1000.0;
        cur_hz.insert(pending_mode, hz);
        cur_px.insert(pending_mode, static_cast<qint64>(pending_w) * pending_h);
        pending_mode = -1;
      }
    }
  }
  flush_block();

  if (best_hz.isEmpty()) {
    appendLog(tr("No HMD modes from vulkaninfo"));
    return false;
  }
  monado_mode_hz_ = best_hz;
  monado_mode_pixels_ = best_px;
  saveMonadoModeTable();
  QStringList summary;
  for (auto it = monado_mode_hz_.constBegin(); it != monado_mode_hz_.constEnd(); ++it) {
    summary << QStringLiteral("%1:@%2Hz")
                   .arg(it.key())
                   .arg(it.value(), 0, 'f', 2);
  }
  appendLog(tr("HMD modes %1: %2").arg(monado_mode_hz_.size()).arg(summary.join(QStringLiteral(", "))));
  return true;
}

void HostWindow::applyPreferredRefreshRate(bool from_ui, bool force_monado_restart) {
  if (usingWivrn()) {
    if (QThread::currentThread() != thread()) {
      QMetaObject::invokeMethod(
          this, [this, from_ui] { applyPreferredRefreshRate(from_ui, false); }, Qt::QueuedConnection);
      return;
    }
    if (from_ui) {
      appendLog(tr("Refresh: %1").arg(preferred_hz_ <= 0 ? tr("Auto (90 Hz)")
                                                         : tr("%1 Hz").arg(preferred_hz_)));
    }
    std::lock_guard<std::mutex> lock(xr_app_mu_);
    if (xr_app_ && xr_app_->has_display_refresh_rate_ext()) {
      const float pref = static_cast<float>(effectivePreferredHz());
      xr_app_->set_preferred_display_refresh_hz(pref);
      if (!xr_app_->request_display_refresh_rate(pref)) {
        appendLog(tr("OpenXR Hz switch failed"));
      }
    } else if (from_ui || force_monado_restart) {
      appendLog(tr("WiVRn refresh follows the headset"));
    }
    return;
  }
  // XR thread must not join itself via disconnect / Monado stop.
  if (QThread::currentThread() != thread()) {
    const bool ui = from_ui;
    const bool force = force_monado_restart;
    QMetaObject::invokeMethod(
        this, [this, ui, force] { applyPreferredRefreshRate(ui, force); }, Qt::QueuedConnection);
    return;
  }

  const int want_hz = effectivePreferredHz();
  const QString want_label =
      preferred_hz_ <= 0 ? tr("Auto (90 Hz)") : tr("%1 Hz").arg(preferred_hz_);
  if (from_ui) {
    appendLog(tr("Refresh: %1").arg(want_label));
  }

  // Live OpenXR request (desktop combo / soft path). HMD [再起動] always restarts Monado.
  if (!force_monado_restart) {
    std::lock_guard<std::mutex> lock(xr_app_mu_);
    if (xr_app_) {
      const float pref = static_cast<float>(want_hz);
      xr_app_->set_preferred_display_refresh_hz(pref);
      if (xr_app_->has_display_refresh_rate_ext()) {
        if (xr_app_->request_display_refresh_rate(pref)) {
          const auto rates = xr_app_->list_display_refresh_rates();
          QString list;
          for (float r : rates) {
            if (!list.isEmpty()) list += ", ";
            list += QString::number(r, 'f', 1);
          }
          appendLog(tr("OpenXR refresh → %1 (rates: %2)").arg(want_label).arg(list.isEmpty() ? tr("none") : list));
          // FB get can lie / lag — verify measured period shortly; fall back to Monado restart.
          const int conf_hz = preferred_hz_;
          const int expect = want_hz;
          QTimer::singleShot(1200, this, [this, conf_hz, expect] {
            if (shutting_down_.load() || preferred_hz_ != conf_hz) return;
            double measured = 0.0;
            {
              std::lock_guard<std::mutex> lock(xr_app_mu_);
              if (xr_app_) measured = xr_app_->status().display_hz;
            }
            if (measured > 0.0 && std::fabs(measured - static_cast<double>(expect)) > 5.0) {
              appendLog(tr("Still %1 Hz after OpenXR — restart Monado")
                            .arg(measured, 0, 'f', 1));
              applyPreferredRefreshRate(/*from_ui=*/false, /*force_monado_restart=*/true);
            }
          });
          return;
        }
        appendLog(tr("OpenXR Hz switch failed — restart Monado"));
      } else if (from_ui) {
        appendLog(tr("No XR_FB_display_refresh_rate — restart Monado"));
      }
    }
  } else {
    appendLog(tr("Restart Monado for refresh → %1").arg(want_label));
  }

  // Pick DESIRED_MODE while mode table is still available (probe if empty).
  // Auto resolves to 90 Hz so Monado does not boot into 120 by default.
  ensureMonadoModeTable();
  {
    const int idx = findModeIndexForHz(want_hz);
    if (idx >= 0) {
      monado_desired_mode_ = idx;
      appendLog(QString("XRT_COMPOSITOR_DESIRED_MODE=%1 (≈%2 Hz)")
                    .arg(idx)
                    .arg(monado_mode_hz_.value(idx), 0, 'f', 1));
      monado_hz_restarted_ = true;  // already decided; avoid double-restart on PRINT_MODES
    } else {
      monado_desired_mode_ = -1;
      appendLog(tr("No mode for %1 Hz — rescan after restart").arg(want_hz));
      monado_mode_hz_.clear();
      monado_mode_pixels_.clear();
      monado_hz_restarted_ = false;
    }
  }
  monado_hz_mismatch_retried_ = false;

  // End XR session first (do not stop Monado here — we restart it with new env next).
  if (xr_running_.load() || xr_thread_.joinable()) {
    resume_media_after_hz_.store(true);
    user_suppressed_auto_connect_ = false;  // reconnect after Monado comes back
    if (!shutdownXrSession(8000, /*stop_monado=*/false)) {
      appendLog(tr("XR detach timeout — restart continues"));
      if (xr_thread_.joinable()) xr_thread_.detach();
      xr_running_.store(false);
    }
    syncDisconnectedUi(tr("Hz change — restarting"));
  }

  if (monado_external_ && preferred_hz_ <= 0 && monado_desired_mode_ < 0) {
    appendLog(tr("External Monado: restart it, then reconnect"));
    return;
  }
  if (monado_external_) {
    appendLog(tr("Drop external Monado and restart with Hz"));
    monado_external_ = false;
  }

  restartOwnedMonadoForHz();
  QTimer::singleShot(600, this, [this] {
    if (!shutting_down_.load() && !xr_running_.load()) maybeAutoConnectHeadset();
  });
}

void HostWindow::maybeRestartMonadoForHz() {
  if (monado_external_ || monado_hz_restarted_) return;
  if (monado_mode_hz_.isEmpty()) return;
  const int want_hz = effectivePreferredHz();
  const int idx = findModeIndexForHz(want_hz);
  if (idx < 0) {
    appendLog(tr("No display mode near %1 Hz")
                  .arg(want_hz));
    monado_hz_restarted_ = true;
    return;
  }
  if (idx == monado_desired_mode_) {
    appendLog(tr("Using mode %1 ≈ %2 Hz").arg(idx).arg(monado_mode_hz_.value(idx), 0, 'f', 1));
    monado_hz_restarted_ = true;
    return;
  }
  monado_desired_mode_ = idx;
  monado_hz_restarted_ = true;
  appendLog(tr("Restart Monado for Hz (mode %1 ≈ %2 Hz)")
                .arg(idx)
                .arg(monado_mode_hz_.value(idx), 0, 'f', 1));
  QMetaObject::invokeMethod(
      this,
      [this] {
        if (xr_running_.load() || xr_thread_.joinable()) {
          resume_media_after_hz_.store(true);
          user_suppressed_auto_connect_ = false;
          if (!shutdownXrSession(8000, /*stop_monado=*/false)) {
            if (xr_thread_.joinable()) xr_thread_.detach();
            xr_running_.store(false);
          }
          syncDisconnectedUi(tr("Hz change — restarting"));
        }
        restartOwnedMonadoForHz();
        QTimer::singleShot(600, this, [this] {
          if (!shutting_down_.load() && !xr_running_.load()) maybeAutoConnectHeadset();
        });
      },
      Qt::QueuedConnection);
}

void HostWindow::startMonado() { startOwnedMonado(/*allow_external_adopt=*/true); }

void HostWindow::startOwnedMonado(bool allow_external_adopt) {
  if (usingWivrn()) {
    startOwnedWivrn(allow_external_adopt);
    return;
  }
  if (monado_->state() != QProcess::NotRunning) {
    if (monado_hz_require_owned_) {
      appendLog(tr("Replace owned Monado with Hz mode"));
      monado_intentional_stop_ = true;
      monado_->terminate();
      if (!monado_->waitForFinished(3000)) {
        monado_->kill();
        monado_->waitForFinished(1000);
      }
      monado_intentional_stop_ = false;
      clearStaleMonadoSocket();
      stopSystemdMonadoForOwnedStart();
      // fall through to start fresh
    } else {
      appendLog(tr("Monado already running (owned)"));
      monado_hz_require_owned_ = false;
      if (isMonadoIpcLive() && !xr_running_.load() && !user_suppressed_auto_connect_) {
        QTimer::singleShot(300, this, [this] { maybeAutoConnectHeadset(); });
      }
      return;
    }
  }
  resolvePreferredDisplayMode();
  // When DESIRED_MODE must stick, never attach to a pre-existing service.
  if (monado_hz_require_owned_ || (preferred_hz_ > 0 && monado_desired_mode_ >= 0)) {
    allow_external_adopt = false;
  }
  if (allow_external_adopt && adoptExternalMonado()) return;

  clearStaleMonadoSocket();
  if (allow_external_adopt && adoptExternalMonado()) return;

  // If IPC is still live after our kill, an external service grabbed it — DESIRED_MODE won't apply.
  if (isMonadoIpcLive()) {
    appendLog(tr("Monado IPC still up — retry stop for Hz"));
    stopSystemdMonadoForOwnedStart();
    if (isMonadoIpcLive()) {
      appendLog(tr("External Monado holds IPC. Stop monado.socket"));
      if (allow_external_adopt) {
        adoptExternalMonado();
        return;
      }
      monado_label_->setText(tr("IPC busy"));
      setHealth("Error", "External Monado blocks Hz");
      return;
    }
  }

  const QString bin = findMonadoBinary();
  if (bin.isEmpty()) {
    monado_label_->setText(tr("No binary"));
    setHealth("Error", "monado-service not found");
    appendLog(tr("monado-service not in PATH"));
    return;
  }
  if (runtime_json_.isEmpty()) {
    runtime_json_ = findRuntimeJson();
  }
  QProcessEnvironment env = hostToolEnvironment();
  if (!runtime_json_.isEmpty()) {
    env.insert("XR_RUNTIME_JSON", runtime_json_);
    qputenv("XR_RUNTIME_JSON", runtime_json_.toUtf8());
    runtime_label_->setText(runtime_json_);
  }
  applyMonadoPacingEnv(env);
  // Ensure child (and any nested tools) see DESIRED_MODE even if env tables differ.
  if (monado_desired_mode_ >= 0) {
    qputenv("XRT_COMPOSITOR_DESIRED_MODE", QByteArray::number(monado_desired_mode_));
  } else {
    qunsetenv("XRT_COMPOSITOR_DESIRED_MODE");
  }
  if (!monado_hz_restarted_) {
    monado_mode_hz_.clear();
    monado_mode_pixels_.clear();
  }
  monado_external_ = false;
  monado_auto_connect_in_progress_ = false;
  monado_multi_hmd_logged_ = false;
  monado_simulated_reject_logged_ = false;
  monado_auto_connect_cooldown_ms_ = 0;
  psvr2_compositor_ready_ms_ = 0;
  psvr2_grab_wait_logged_ = false;
  monado_->setProcessEnvironment(env);
  monado_->setProgram(bin);
  {
    QString msg = tr("Start Monado: %1").arg(bin);
    if (monado_desired_mode_ >= 0) {
      msg += QStringLiteral(" mode=%1").arg(monado_desired_mode_);
    }
    appendLog(msg);
  }
  monado_->start();
  if (!monado_->waitForStarted(3000)) {
    monado_label_->setText(tr("Start failed"));
    setHealth("Error", monado_->errorString());
    appendLog(tr("Start failed: %1").arg(monado_->errorString()));
    return;
  }
  monado_hz_require_owned_ = false;
  monado_label_->setText(tr("Running pid %1").arg(monado_->processId()));
  setHealth("Wait", "Monado up. Waiting for HMD");
  QTimer::singleShot(400, this, [this] { maybeAutoConnectHeadset(); });
}

void HostWindow::stopMonado() {
  signalXrStop();
  if (monado_external_) {
    if (usingWivrn()) {
      appendLog(tr("External WiVRn left running"));
      appendLog(tr("Or: systemctl --user stop wivrn.service"));
    } else {
      appendLog(tr("External Monado left running"));
      appendLog(tr("Or: systemctl --user stop monado.socket monado.service"));
    }
    if (!shutdownXrSession(8000, false)) {
      appendLog(tr("XR disconnect timeout"));
      if (xr_thread_.joinable()) xr_thread_.detach();
      xr_running_.store(false);
    }
    monado_external_ = false;
    const bool still_up = usingWivrn() ? isWivrnServerLive() : isMonadoIpcLive();
    monado_label_->setText(still_up ? tr("External") : tr("Stopped"));
    if (!shutting_down_.load()) {
      setSessionChrome(false);
    }
    return;
  }
  if (monado_ && monado_->state() != QProcess::NotRunning) {
    appendLog(usingWivrn() ? tr("Stopping WiVRn") : tr("Stopping Monado"));
  }
  if (!shutdownXrSession(8000, true)) {
    appendLog(tr("XR disconnect timeout"));
    if (xr_thread_.joinable()) xr_thread_.detach();
    xr_running_.store(false);
  }
  monado_label_->setText(tr("Idle"));
  if (!shutting_down_.load()) {
    setSessionChrome(false);
  }
}

void HostWindow::onMonadoOutput() {
  const QString text = QString::fromUtf8(monado_->readAllStandardOutput());
  // Example: "|  0 | 2000x2040@120.00"
  static const QRegularExpression mode_re(R"(\|\s*(\d+)\s*\|\s*(\d+)x(\d+)@([\d.]+))");
  // Monado probes every USB iface; Logitech Unifying etc. share bus/addr with
  // different product IDs → noisy P_ERROR with no HMD impact (no Monado knob to
  // suppress ERROR-only without hiding real failures).
  static const QString usb_addr_noise =
      QStringLiteral("USB device with same address but different");
  bool saw_mode = false;
  bool skip_usb_addr_noise_cont = false;
  for (const QString& line : text.split('\n', Qt::SkipEmptyParts)) {
    const QString trimmed = line.trimmed();
    if (skip_usb_addr_noise_cont) {
      // Continuation: "vendor: …" / "product: …"
      if (trimmed.startsWith(QStringLiteral("vendor:")) || trimmed.startsWith(QStringLiteral("product:"))) {
        continue;
      }
      skip_usb_addr_noise_cont = false;
    }
    if (trimmed.contains(usb_addr_noise)) {
      skip_usb_addr_noise_cont = true;
      continue;
    }
    notePsvr2DisplayGrabbed(trimmed);
    const QString lower = line.toLower();
    if (usingWivrn()) {
      if (lower.contains(QStringLiteral("error")) || lower.contains(QStringLiteral("warn")) ||
          lower.contains(QStringLiteral("fail"))) {
        appendLog(line);
      }
      continue;
    }
    const auto m = mode_re.match(line);
    const bool interesting =
        m.hasMatch() || lower.contains(QStringLiteral("error")) || lower.contains(QStringLiteral("warn")) ||
        lower.contains(QStringLiteral("fail")) || lower.contains(QStringLiteral("desired_mode"));
    if (interesting) appendLog(line);
    if (m.hasMatch()) {
      const int idx = m.captured(1).toInt();
      const qint64 w = m.captured(2).toLongLong();
      const qint64 h = m.captured(3).toLongLong();
      const double hz = m.captured(4).toDouble();
      monado_mode_hz_.insert(idx, hz);
      monado_mode_pixels_.insert(idx, w * h);
      saw_mode = true;
    }
  }
  if (saw_mode) {
    saveMonadoModeTable();
    maybeRestartMonadoForHz();
  }
}

void HostWindow::onMonadoFinished(int code, QProcess::ExitStatus status) {
  monado_label_->setText(tr("Exit %1").arg(code));
  appendLog(QString("%1 exited code=%2 status=%3")
                .arg(usingWivrn() ? QStringLiteral("wivrn-server") : QStringLiteral("monado-service"))
                .arg(code)
                .arg(status == QProcess::CrashExit ? "crash" : "normal"));
  if (monado_intentional_stop_ || shutting_down_.load()) {
    return;
  }
  if (code != 0) {
    if (usingWivrn()) {
      setHealth("Error", "WiVRn exited. See log");
      return;
    }
    if (isMonadoIpcLive()) {
      adoptExternalMonado();
      return;
    }
    // Retry once after clearing a stale socket (common after crash / hard kill).
    if (QFileInfo::exists(monadoIpcPath())) {
      appendLog(tr("Stale IPC socket — clear and retry"));
      clearStaleMonadoSocket();
    }
    setHealth("Error", "Monado exited. See log");
  }
}

void HostWindow::connectHeadset() {
  if (xr_running_.load()) {
    appendLog(tr("HMD session already running"));
    monado_auto_connect_in_progress_ = false;
    return;
  }
  // Manual connect re-enables auto-reconnect after future unexpected drops.
  if (!monado_auto_connect_in_progress_) {
    user_suppressed_auto_connect_ = false;
  }
  if (deferUntilPsvr2PanelReady()) {
    monado_auto_connect_in_progress_ = false;
    return;
  }
  if (deferUntilDisplayLeaseReady()) {
    monado_auto_connect_in_progress_ = false;
    return;
  }
  if (restartMonadoAfterDisplayLatch()) {
    monado_auto_connect_in_progress_ = false;
    return;
  }
  if (isMonadoIpcLive() && deferUntilPsvr2CompositorSettled()) {
    monado_auto_connect_in_progress_ = false;
    return;
  }
  // Prefer HMD headphones when starting a session (user can still override in the combo).
  audio_user_override_.store(false);
  refreshAudioDevices(true);
  // Finished threads stay joinable; assigning again would std::terminate().
  if (xr_thread_.joinable() || xr_running_.load()) {
    appendLog(tr("Waiting for previous session…"));
    if (!shutdownXrSession(8000, false)) {
      appendLog(tr("Previous XR thread hung — detach"));
      if (xr_thread_.joinable()) xr_thread_.detach();
      xr_running_.store(false);
    }
  }
  if (!ensureMonadoReady()) {
    if (!shutting_down_.load()) {
      syncDisconnectedUi(usingWivrn() ? tr("WiVRn not responding") : tr("Monado not responding"));
    }
    monado_auto_connect_in_progress_ = false;
    if (!user_suppressed_auto_connect_) {
      monado_auto_connect_cooldown_ms_ = QDateTime::currentMSecsSinceEpoch() + 2000;
      QTimer::singleShot(500, this, [this] { maybeAutoConnectHeadset(); });
    }
    return;
  }
  if (deferUntilPsvr2CompositorSettled()) {
    monado_auto_connect_in_progress_ = false;
    return;
  }
  if (!runtime_json_.isEmpty()) {
    qputenv("XR_RUNTIME_JSON", runtime_json_.toUtf8());
  }
  // Controller / hand already live in opt_ via Settings menu actions.
  xr_stop_.store(false);
  setSessionChrome(true);
  setHealth("Wait", "Starting OpenXR session");
  appendLog(tr("Connect OpenXR (pad=%1 / %2)").arg(QString::fromUtf8(vrp::controller_profile_name(opt_.controller_profile))).arg(QString::fromUtf8(vrp::controller_hand_name(opt_.controller_hand))));
  xr_thread_ = std::thread([this] { xrThreadMain(); });
}

void HostWindow::disconnectHeadset() {
  // User clicked「切断」— do not auto-reconnect until they connect again (or restart).
  user_suppressed_auto_connect_ = true;
  monado_auto_connect_in_progress_ = false;
  disconnectHeadsetSession(/*resume_auto_poll=*/false);
  if (!shutting_down_.load()) {
    appendLog(tr("HMD session closed (auto-connect off)"));
  }
}

void HostWindow::disconnectHeadsetSession(bool resume_auto_poll) {
  if (!xr_thread_.joinable() && !xr_running_.load()) {
    // Idle disconnect path: still drop owned Monado if it was left running.
    if (!monado_external_) {
      stopMonadoProcess();
      if (monado_label_) monado_label_->setText(tr("Idle"));
    }
    if (!shutting_down_.load()) {
      syncDisconnectedUi(tr("Not connected"));
      if (resume_auto_poll && !user_suppressed_auto_connect_) {
        QTimer::singleShot(500, this, [this] { maybeAutoConnectHeadset(); });
      }
    }
    return;
  }
  // Stop owned Monado with the XR session so the compositor releases GPU/DRM.
  if (!shutdownXrSession(8000, !monado_external_)) {
    appendLog(tr("Disconnect timeout — detach XR"));
    VRP_ERR("disconnect: XR join timed out — detaching");
    if (xr_thread_.joinable()) xr_thread_.detach();
    xr_running_.store(false);
    if (!monado_external_) stopMonadoProcess();
  }
  if (!monado_external_ && monado_label_) {
    monado_label_->setText(tr("Idle"));
  }
  if (!shutting_down_.load()) {
    syncDisconnectedUi(resume_auto_poll ? tr("Waiting for HMD") : tr("Session ended"));
    if (resume_auto_poll && !user_suppressed_auto_connect_) {
      QTimer::singleShot(500, this, [this] { maybeAutoConnectHeadset(); });
    }
  }
}

void HostWindow::xrThreadMain() {
  xr_running_.store(true);
  logged_session_refresh_hz_ = false;
  auto report_progress = [this](const std::string& msg) {
    const QString q = QString::fromStdString(msg);
    QMetaObject::invokeMethod(
        this,
        [this, q] {
          if (shutting_down_.load()) return;
          QString shown = UiTr(q);
          if (q.startsWith(QLatin1String("HMD found: "))) {
            shown = tr("HMD found: %1").arg(q.mid(11));
          }
          setHealth("Wait", shown);
          appendLog(shown);
        },
        Qt::QueuedConnection);
  };

  try {
    // Wait for Monado compositor IPC before touching OpenXR (avoids hung xrCreateInstance).
    // WiVRn has no monado_comp_ipc; the server process is enough, then xrGetSystem waits for the headset.
    if (!usingWivrn()) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
      bool logged_wait = false;
      while (!isMonadoIpcLive()) {
        if (xr_stop_.load()) throw std::runtime_error("Connect cancelled");
        if (std::chrono::steady_clock::now() >= deadline) {
          throw std::runtime_error("Monado IPC not responding");
        }
        if (!logged_wait) {
          report_progress("Waiting for Monado…");
          logged_wait = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
      if (logged_wait) report_progress("Monado connected");
    }

    vrp::XrVulkanApp xr;
    xr.set_controller_prefs(opt_.controller_profile, opt_.controller_hand);
    xr.set_preferred_display_refresh_hz(static_cast<float>(effectivePreferredHz()));
    xr.init(&xr_stop_, report_progress);
    {
      std::lock_guard<std::mutex> lock(xr_app_mu_);
      xr_app_ = &xr;
    }
    struct ClearXrApp {
      HostWindow* self;
      ~ClearXrApp() {
        std::lock_guard<std::mutex> lock(self->xr_app_mu_);
        self->xr_app_ = nullptr;
      }
    } clear_xr{this};

    auto st = xr.status();
    QMetaObject::invokeMethod(
        this,
        [this, st] {
          const QString name = QString::fromStdString(st.system_name);
          applyStatus(name, QString::fromStdString(st.session_state), "Wait",
                      "HMD found. Waiting for session", st.tracking_valid, st.view_width,
                      st.view_height, st.display_hz, st.app_frame_ms,
                      QString::fromStdString(st.sync_note), st.refresh_rate_ext,
                      QString::fromStdString(st.refresh_rates));
          onAutoConnectXrReady(name);
        },
        Qt::QueuedConnection);

    vrp::SceneRenderer scene;
    scene.init(xr, shader_dir_.toStdString());
    scene.set_projection(opt_.projection, opt_.stereo, opt_.flat_fov_deg, opt_.screen_distance);
    scene.set_content_aspect(16.f / 9.f);
    scene.set_milkyway_gain(milkyway_gain_);
    scene.set_sky_observer(sky_lat_, sky_lon_);

    vrp::Fsr1Upscaler fsr;
    fsr.init(xr.physical_device(), xr.device(), xr.queue(), xr.queue_family(), &xr.queue_mutex(),
             shader_dir_.toStdString());
    fsr.set_mode(preferred_fsr_);

    vrp::VideoDecoder decoder;
    vrp::AudioPlayer audio;
    audio.set_surround_gain(surround_gain_);
    // Keep miniaudio up for the whole HMD session (silence when idle / switching).
    if (!opt_.mute) {
      std::string adev;
      {
        std::lock_guard<std::mutex> lock(audio_mu_);
        adev = audio_device_;
      }
      if (audio.ensure_output(adev)) {
        const QString dev = QString::fromStdString(audio.device_name());
        (void)dev;
      } else {
        const QString err = QString::fromStdString(audio.last_error());
        QMetaObject::invokeMethod(
            this, [this, err] { appendLog(tr("Audio start failed: %1").arg(err)); }, Qt::QueuedConnection);
      }
    }
    vrp::VideoTexture texture;
    vrp::CudaNv12Texture cuda_tex;
    vrp::VaapiNv12Texture vaapi_tex;
#if defined(VRP_HAS_PIPEWIRE)
    vrp::PipewireDmaTexture pw_dma_tex;
#endif
    vrp::VideoFrame pending_upload;
    texture.init(xr.physical_device(), xr.device(), xr.queue(), xr.queue_family(), &xr.queue_mutex());
    vrp::VideoTexture seek_tex;
    seek_tex.init(xr.physical_device(), xr.device(), xr.queue(), xr.queue_family(), &xr.queue_mutex());
    vrp::Bc7ThumbCache bc7;
    bc7.init(xr.physical_device(), xr.device(), xr.compute_queue(), xr.queue(), xr.queue_family(),
             &xr.queue_mutex(), shader_dir_.toStdString());
    VkPhysicalDeviceProperties gpu_props{};
    vkGetPhysicalDeviceProperties(xr.physical_device(), &gpu_props);
    const bool nvidia_bc7 = !thumb_jpeg_ && gpu_props.vendorID == 0x10DEu && bc7.ok();
    VRP_LOG("Seek thumbs: %s (GPU vendor 0x%x, %s)", nvidia_bc7 ? "BC1" : "JPEG", gpu_props.vendorID,
            gpu_props.deviceName);
    const bool gpu_nv12 =
        cuda_tex.init(xr.physical_device(), xr.device(), xr.queue(), xr.queue_family(), &xr.queue_mutex(),
                      xr.external_memory_fd());
    const bool vaapi_nv12 =
        vaapi_tex.init(xr.physical_device(), xr.device(), xr.queue(), xr.queue_family(), &xr.queue_mutex(),
                       xr.dma_buf_import());
#if defined(VRP_HAS_PIPEWIRE)
    const bool pw_dma =
        pw_dma_tex.init(xr.physical_device(), xr.device(), xr.queue(), xr.queue_family(), &xr.queue_mutex(),
                        xr.dma_buf_import());
    const bool pw_dma_nvidia = pw_dma && pw_dma_tex.nvidia();
    if (pw_dma) {
      live_drm_modifiers_ = pw_dma_tex.sampleable_modifiers();
    } else {
      live_drm_modifiers_ =
          vrp::PipewireDmaTexture::query_sampleable_modifiers(xr.physical_device());
    }
#else
    const bool pw_dma = false;
    const bool pw_dma_nvidia = false;
#endif
    decoder.set_cuda_texture(gpu_nv12 ? &cuda_tex : nullptr);
    decoder.set_vaapi_texture(vaapi_nv12 ? &vaapi_tex : nullptr);
    QMetaObject::invokeMethod(
        this,
        [this, gpu_nv12, vaapi_nv12, pw_dma, pw_dma_nvidia] {
          if (gpu_nv12) appendLog(tr("Video: GPU NV12"));
          else if (vaapi_nv12) appendLog(tr("Video: VA-API dma-buf"));
          else appendLog(tr("Video: CPU RGBA"));
          if (pw_dma_nvidia)
            appendLog(tr("ScreenCast: DMA-BUF import armed (NVIDIA experimental)"));
          else if (pw_dma)
            appendLog(tr("ScreenCast: DMA-BUF import armed"));
        },
        Qt::QueuedConnection);
    scene.set_texture(texture.view(), texture.sampler());

    vrp::VideoTexture hud_tex;
    hud_tex.init(xr.physical_device(), xr.device(), xr.queue(), xr.queue_family(), &xr.queue_mutex(),
                 /*nearest_filter=*/false);
    vrp::VideoTexture thumb_tex;
    thumb_tex.init(xr.physical_device(), xr.device(), xr.queue(), xr.queue_family(), &xr.queue_mutex(),
                   /*nearest_filter=*/false);
    vrp::VrMenu menu;
    {
      std::filesystem::path start;
      if (!opt_.video_path.empty()) {
        start = std::filesystem::path(opt_.video_path).parent_path();
      } else {
        const char* home = std::getenv("HOME");
        const std::filesystem::path conf =
            std::filesystem::path(home ? home : "") / ".config" / "monasphere" / "monasphere.conf";
        std::ifstream in(conf);
        std::string line;
        while (std::getline(in, line)) {
          if (line.rfind("last_dir=", 0) == 0) {
            start = line.substr(9);
            break;
          }
        }
        if (start.empty()) {
          start = home ? std::filesystem::path(home) : std::filesystem::current_path();
        }
      }
      menu.init(start);
      {
        std::vector<vrp::VrMenu::LiveApp> apps;
        apps.reserve(static_cast<size_t>(live_apps_.size()));
        for (const LiveAppEntry& a : live_apps_) {
          if (a.name.isEmpty() || a.url.isEmpty()) continue;
          apps.push_back({a.name.toStdString(), a.url.toStdString()});
        }
        menu.set_live_apps(std::move(apps));
      }
      {
        std::vector<vrp::VrMenu::DesktopApp> apps;
        apps.reserve(static_cast<size_t>(live_desktop_apps_.size()));
        for (const LiveDesktopAppEntry& a : live_desktop_apps_) {
          if (a.name.isEmpty() || a.exec.isEmpty()) continue;
          vrp::VrMenu::DesktopApp d;
          d.name = a.name.toStdString();
          d.exec = a.exec.toStdString();
          d.args.reserve(static_cast<size_t>(a.args.size()));
          for (const QString& arg : a.args) d.args.push_back(arg.toStdString());
          d.titles.reserve(static_cast<size_t>(a.titles.size()));
          for (const LiveDesktopTitle& t : a.titles) {
            if (t.name.isEmpty()) continue;
            vrp::VrMenu::DesktopTitle dt;
            dt.name = t.name.toStdString();
            dt.monitor_span = std::clamp(t.monitor_span > 0 ? t.monitor_span : 1, 1, 3);
            dt.args.reserve(static_cast<size_t>(t.args.size()));
            for (const QString& arg : t.args) dt.args.push_back(arg.toStdString());
            d.titles.push_back(std::move(dt));
          }
          apps.push_back(std::move(d));
        }
        menu.set_desktop_apps(std::move(apps));
      }
      audio.set_volume(menu.volume());
      menu.set_format(opt_.projection, opt_.stereo, opt_.stereo_full);
      menu.set_fsr(preferred_fsr_);
      fsr.set_mode(preferred_fsr_);
      menu.set_preferred_hz(preferred_hz_);
    }
    {
      std::lock_guard<std::mutex> lock(menu_mu_);
      vr_menu_ = &menu;
    }
    struct ClearMenu {
      HostWindow* self;
      ~ClearMenu() {
        std::lock_guard<std::mutex> lock(self->menu_mu_);
        self->vr_menu_ = nullptr;
      }
    } clear_menu{this};
    auto hud_paint = [&] {
      menu.publish_hud_snapshot();
      requestHudPaint();
    };
    hud_paint();

    vrp::GamepadInput gamepad;
    bool gamepad_was = false;
    bool gamepad_logged = false;
    std::string last_gamepad_name;
    std::string last_gamepad_connection;

    std::atomic<bool> preview_busy{false};
    std::atomic<uint64_t> preview_gen{0};
    std::thread preview_thread;
    std::mutex preview_result_mu;
    struct PreviewResult {
      uint64_t gen = 0;
      bool pending = false;
      bool ok = false;
      std::string path;
      std::vector<uint8_t> rgba;
      int w = 0;
      int h = 0;
    } preview_result;
    std::string preview_path_done;
    std::string preview_wait_path;
    std::string highlight_row;
    auto preview_wait_since = std::chrono::steady_clock::now();
    auto join_preview_worker = [&] {
      preview_gen.fetch_add(1, std::memory_order_acq_rel);
      if (preview_thread.joinable()) preview_thread.join();
      preview_busy.store(false, std::memory_order_release);
      std::lock_guard<std::mutex> lock(preview_result_mu);
      preview_result = {};
    };
    auto request_preview = [&](const std::string& path) {
      if (path.empty() || path.rfind("http://", 0) == 0 || path.rfind("https://", 0) == 0) {
        if (!preview_path_done.empty() || preview_busy.load(std::memory_order_acquire)) {
          preview_gen.fetch_add(1, std::memory_order_acq_rel);
          preview_path_done.clear();
          menu.clear_preview();
          hud_paint();
        }
        return;
      }
      if (path == preview_path_done || preview_busy.load(std::memory_order_acquire)) return;
      if (preview_thread.joinable()) preview_thread.join();
      preview_busy.store(true, std::memory_order_release);
      const uint64_t gen = preview_gen.fetch_add(1, std::memory_order_acq_rel) + 1;
      {
        std::lock_guard<std::mutex> lock(preview_result_mu);
        preview_result = {};
      }
      preview_thread = std::thread([&, path, gen] {
        std::vector<uint8_t> rgba;
        int pw = 0;
        int ph = 0;
        const bool ok = vrp::extract_video_preview(path, 1024, rgba, pw, ph);
        {
          std::lock_guard<std::mutex> lock(preview_result_mu);
          if (preview_gen.load(std::memory_order_acquire) == gen) {
            preview_result.gen = gen;
            preview_result.pending = true;
            preview_result.ok = ok && pw > 0 && ph > 0;
            preview_result.path = path;
            preview_result.rgba = std::move(rgba);
            preview_result.w = pw;
            preview_result.h = ph;
          }
        }
        preview_busy.store(false, std::memory_order_release);
      });
    };
    auto apply_preview_result = [&] {
      PreviewResult done;
      {
        std::lock_guard<std::mutex> lock(preview_result_mu);
        if (!preview_result.pending) return;
        if (preview_result.gen != preview_gen.load(std::memory_order_acquire)) {
          preview_result.pending = false;
          return;
        }
        done = std::move(preview_result);
        preview_result = {};
        done.pending = true;
      }
      if (!done.ok || xr_stop_.load() || shutting_down_.load()) return;
      if (menu.highlighted_video_path() != done.path) return;
      menu.set_preview(std::move(done.rgba), done.w, done.h);
      preview_path_done = done.path;
      hud_paint();
    };

    bool vaapi_hud_pending = true;
    bool pending_video_rebind = false;
    bool fsr_need_process = false;
    VkImageView fsr_src_y = VK_NULL_HANDLE;
    VkImageView fsr_src_uv = VK_NULL_HANDLE;
    VkSampler fsr_src_samp = VK_NULL_HANDLE;
    int fsr_src_w = 0;
    int fsr_src_h = 0;
    int fsr_src_nv12 = 0;
    bool thumb_latched = false;
    bool seek_settle = false;
    std::chrono::steady_clock::time_point seek_settle_from{};
    bool wait_stop_frame = false;
    bool stop_frame_shown = false;
    bool stop_seek_sent = false;
    bool stop_upload_sent = false;
    uint64_t stop_seq = 0;
    double stop_pts = -1.0;
    // Translucent flat panel (startup / after Stop) until playback shows frames again.
    bool show_idle_screen = true;
    bool yt_suppress_idle_uploaded = false;
    float yt_spin_phase = 0.f;
    bool have_pending_thumb = false;
    int thumb_idx = -1;
    vrp::VideoFrame pending_thumb;
    auto open_media = [&](const std::string& path) {
      vaapi_hud_pending = true;
      thumb_latched = false;
      seek_settle = false;
      wait_stop_frame = false;
      stop_frame_shown = false;
      stop_seek_sent = false;
      stop_upload_sent = false;
      stop_seq = 0;
      stop_pts = -1.0;
      show_idle_screen = true;
      have_pending_thumb = false;
      thumb_idx = -1;
      pending_thumb = {};
      pending_video_rebind = false;
      fsr_need_process = false;
      fsr_src_y = fsr_src_uv = VK_NULL_HANDLE;
      fsr_src_samp = VK_NULL_HANDLE;
      fsr_src_w = fsr_src_h = 0;
      fsr_src_nv12 = 0;

      // Wait for the eye GPU to finish BEFORE halt_decode. Returning false after a
      // halt left the demux/decode threads dead while is_open() stayed true — play
      // after an Hz reconnect then did nothing / looked like "device not ready".
      if (!scene.wait_previous_submit(0)) {
        VRP_LOG("open_media: waiting for previous eye submit");
        while (!xr_stop_.load() && !scene.wait_previous_submit(100'000'000ull)) {
        }
      }
      if (xr_stop_.load()) {
        VRP_LOG("open_media: cancelled (xr_stop before halt)");
        return false;
      }
      if (!scene.wait_previous_submit(2'000'000'000ull)) {
        VRP_ERR("open_media: GPU still busy after wait");
        return false;
      }

      // Keep miniaudio alive and muted so silence keeps flushing HMD/Pulse during the switch.
      // Closing the device mid-switch is what left the previous file's audio in the sink.
      if (audio.is_open()) audio.mute_output();
      if (decoder.is_open()) decoder.pause();

      // Stop video decode but keep NVDEC CUDA context alive until after slot teardown.
      // Do NOT audio.close() here — device stays up pumping silence.
      decoder.halt_decode();
      // Release ffmpeg_offline_mu (seek thumbs) before joining the browser preview worker.
      decoder.stop_thumbnail_build();
      join_preview_worker();
      pending_upload = {};
      cuda_tex.drain_copies();

      // Stop further BC7 submits; images are safe to drop now that the eye fence is idle.
      const uint64_t bc7_build = bc7.begin_build();
      (void)texture.upload_idle_screen();
      scene.set_texture(texture.view(), texture.sampler());
      cuda_tex.reset_slots(decoder.cuda_context());
      vaapi_tex.reset();
      bc7.release_images();
      decoder.close();

      const bool ok = decoder.open(path, hwaccel_);
      const bool network_url =
          path.rfind("http://", 0) == 0 || path.rfind("https://", 0) == 0;
      if (ok && !network_url) {
        vrp::SeekThumbRawStep raw;
        if (nvidia_bc7) {
          raw = [&bc7, bc7_build](int index, double /*time_sec*/, int /*planned*/, double interval, int w, int h,
                                  const uint8_t* rgba, size_t /*rgba_bytes*/) {
            return bc7.compress(index, rgba, w, h, interval, bc7_build);
          };
        }
        decoder.start_thumbnail_build(
            [this](vrp::ThumbnailStrip strip) {
              QMetaObject::invokeMethod(
                  this,
                  [this, strip = std::move(strip)] {
                    if (!shutting_down_.load()) reportThumbnails(strip);
                  },
                  Qt::QueuedConnection);
            },
            seek_thumb_count_, 0, 0, raw, seek_thumb_edge_, hwaccel_);
      }
      const vrp::VideoInfo info = decoder.info();
      QMetaObject::invokeMethod(
          this, [this, info] { reportVideoOpen(info); }, Qt::QueuedConnection);
      if (ok) {
        vrp::detect_format_from_filename(path, opt_.projection, opt_.stereo, opt_.stereo_full);
        scene.set_projection(opt_.projection, opt_.stereo, opt_.flat_fov_deg, opt_.screen_distance);

        // Compact HUD fields (映像 / 音声 / 出力) — keep short for panel width.
        const std::string vcodec = info.codec.empty() ? "-" : info.codec;
        char res[32];
        std::snprintf(res, sizeof(res), "%dx%d", info.width, info.height);
        char fps[24];
        if (info.fps > 0) std::snprintf(fps, sizeof(fps), "%.3g", info.fps);
        else std::snprintf(fps, sizeof(fps), "-");
        std::string bitrate = "-";
        if (info.bitrate >= 1'000'000) {
          char br[24];
          std::snprintf(br, sizeof(br), "%.1fM", info.bitrate / 1'000'000.0);
          bitrate = br;
        } else if (info.bitrate > 0) {
          char br[24];
          std::snprintf(br, sizeof(br), "%.0fk", info.bitrate / 1000.0);
          bitrate = br;
        }
        const std::string acodec = info.audio_codec.empty() ? "-" : info.audio_codec;
        std::string arate = "-";
        if (info.audio_sample_rate >= 1000)
          arate = std::to_string(info.audio_sample_rate / 1000) + "kHz";
        else if (info.audio_sample_rate > 0)
          arate = std::to_string(info.audio_sample_rate) + "Hz";
        std::string ach = "-";
        if (info.audio_channels > 0) ach = std::to_string(info.audio_channels) + "ch";
        std::string hw = info.hwaccel.empty() ? "SW" : info.hwaccel;
        if (hw.find("cuda") != std::string::npos || hw.find("NVDEC") != std::string::npos)
          hw = "NVDEC";
        else if (hw.find("vaapi") != std::string::npos || hw.find("VAAPI") != std::string::npos)
          hw = "VAAPI";
        else if (hw.find("vdpau") != std::string::npos)
          hw = "VDPAU";
        else if (hw == "software" || hw == "none")
          hw = "SW";
        std::string out_note;
        if (info.display_note.find("zero-copy") != std::string::npos ||
            info.display_note.find("ゼロコピー") != std::string::npos ||
            info.display_note.find("NV12") != std::string::npos) {
          out_note = "0-copy NV12";
        } else if (info.display_width > 0 && info.width > 0 &&
                   (info.display_width != info.width || info.display_height != info.height)) {
          out_note = "CPU scale " + std::to_string(info.display_width) + "x" +
                     std::to_string(info.display_height);
        } else {
          out_note = "CPU blit";
        }
        menu.set_media(path, decoder.duration());
        menu.set_media_info(vcodec, res, fps, bitrate, acodec, arate, ach, hw, out_note);
        menu.set_preferred_hz(preferred_hz_);
        menu.set_display_hz(static_cast<float>(xr.status().display_hz));
        menu.set_format(opt_.projection, opt_.stereo, opt_.stereo_full);
        audio.set_volume(menu.volume());
        decoder.set_rate(1.f);
        audio.set_rate(1.f);
        menu.set_rate(1.f);
        scene.set_content_aspect(vrp::frame_display_aspect(static_cast<float>(info.width),
                                                           static_cast<float>(info.height), opt_.stereo,
                                                           opt_.stereo_full));
        QMetaObject::invokeMethod(
            this,
            [this] {
              appendLog(tr("Format guess: %1 / %2")
                            .arg(QString::fromUtf8(vrp::projection_name(opt_.projection)))
                            .arg(QString::fromUtf8(
                                vrp::stereo_detect_name(opt_.stereo, opt_.stereo_full))));
            },
            Qt::QueuedConnection);
        hud_paint();
        if (!opt_.mute) {
          std::string adev;
          {
            std::lock_guard<std::mutex> lock(audio_mu_);
            adev = audio_device_;
          }
          (void)audio.ensure_output(adev);
          // Demux swap only — device keeps pumping silence into the HMD.
          if (audio.switch_file(path)) {
            audio.set_volume(menu.volume());
            audio.set_rate(1.f);
            const QString dev = QString::fromStdString(audio.device_name());
            QMetaObject::invokeMethod(
                this, [this, dev] { appendLog(tr("Audio → %1").arg(dev)); }, Qt::QueuedConnection);
          } else {
            audio.clear_file();  // video-only / demux fail — keep silent device
            const QString err = QString::fromStdString(audio.last_error());
            QMetaObject::invokeMethod(
                this, [this, err] { appendLog(tr("No audio: %1").arg(err)); }, Qt::QueuedConnection);
          }
        } else {
          audio.clear_file();
        }
      }
      if (ok) {
        std::lock_guard<std::mutex> lock(session_media_mu_);
        session_video_path_ = path;
      }
      return ok;
    };

    // True while the user wants picture and sound together. A seek freezes video
    // until audio is armed at the landed frame, then presentation starts.
    bool play_wanted = false;
    bool was_scrubbing = false;

    std::string resume_path;
    double resume_pos = 0.0;
    bool resume_play = false;
    bool pending_hz_resume = false;
    int hz_resume_attempts = 0;
    auto last_hz_resume_try = std::chrono::steady_clock::time_point{};
    if (resume_media_after_hz_.exchange(false)) {
      std::lock_guard<std::mutex> lock(session_media_mu_);
      resume_path = session_video_path_;
      resume_pos = session_video_pos_;
      resume_play = session_play_wanted_.load();
      pending_hz_resume = !resume_path.empty() && !opt_.clear_only;
      // HMD Pulse sink often reappears only after the new session is focused —
      // refresh the route once the XR loop is pumping.
      if (pending_hz_resume) audio_reopen_.store(true);
    }

    // Defer Hz resume until the OpenXR session is RUNNING (see main loop). Opening
    // immediately after Monado restart raced display/audio bring-up and left play stuck.
    if (!pending_hz_resume && !opt_.video_path.empty() && !opt_.clear_only) {
      if (!open_media(opt_.video_path)) {
        QMetaObject::invokeMethod(
            this,
            [this] { appendLog(tr("Startup video failed. Pick another file")); },
            Qt::QueuedConnection);
      } else {
        play_wanted = true;
        show_idle_screen = false;
        session_play_wanted_.store(true);
        decoder.play();
      }
    }

    auto last = std::chrono::steady_clock::now();
    double ui_accum = 0;
    bool seek_paint_wait = false;
    uint64_t seek_paint_gen = 0;
    uint64_t seek_paint_pixels = 0;
    uint64_t hud_mesh_applied = 0;
    bool stop_seek_pending = false;
    double stop_seek_sec = 0;
    std::vector<XrCompositionLayerProjectionView> proj_views;
    XrCompositionLayerProjection layer{};
    auto last_status = std::chrono::steady_clock::now();
    bool force_fsr_refresh = true;
    float look_yaw = 0.f;
    float look_pitch = 0.f;
    int flat_scale_index = vrp::find_flat_scale_preset(opt_.flat_fov_deg, opt_.screen_distance);

    while (!xr_stop_.load() && xr.pump_events()) {
      bool help_shown = false;
      auto now = std::chrono::steady_clock::now();
      double dt = std::chrono::duration<double>(now - last).count();
      last = now;
      if (dt > 0.1) dt = 0.1;
      // UI state and HUD paints run at 60fps. The XR loop stays at the display rate.
      constexpr double kUiStep = 1.0 / 60.0;
      float ui_dt = 0.f;
      float seek_dt = 0.f;
      ui_accum += dt;
      if (ui_accum >= kUiStep) {
        ui_dt = static_cast<float>(kUiStep);
        // The seek clock keeps the host time this tick actually covered.
        seek_dt = static_cast<float>(ui_accum > 0.1 ? 0.1 : ui_accum);
        ui_accum = 0;
      }

      if (seek_paint_wait && seek_paint_pixels == 0 &&
          hud_paint_done_.load(std::memory_order_acquire) >= seek_paint_gen) {
        std::lock_guard<std::mutex> lock(hud_pixels_mu_);
        seek_paint_pixels = hud_pixels_gen_;
      }
      menu.set_seek_frame_dt(seek_dt);

      const int cmd = cmd_.exchange(0);
      if (cmd == 1) {
        // Keep A/V in lockstep — independent toggles desync after audio reopen.
        const bool active = decoder.is_playing() || decoder.is_priming();
        if (!active) {
          play_wanted = true;
          show_idle_screen = false;
          decoder.play();
        } else {
          play_wanted = false;
          decoder.pause();
          audio.pause();
        }
      }
      if (cmd == 2 || cmd == 3) {
        double target = 0;
        if (menu.arm_seek_delta(cmd == 2 ? -10.0 : 10.0, target)) {
          decoder.seek_to(target);
          audio.pause();
          hud_paint();
        }
      }
      if (cmd == 4 && xr.recenter()) {
        look_yaw = 0.f;
        look_pitch = 0.f;
      }

      const int p = proj_req_.exchange(-1);
      const int s = stereo_req_.exchange(-1);
      if (p >= 0 || s >= 0) {
        if (p >= 0) opt_.projection = static_cast<vrp::ProjectionMode>(p);
        if (s >= 0) opt_.stereo = static_cast<vrp::StereoLayout>(s);
        scene.set_projection(opt_.projection, opt_.stereo, opt_.flat_fov_deg, opt_.screen_distance);
        if (decoder.is_open()) {
          const auto& info = decoder.info();
          scene.set_content_aspect(vrp::frame_display_aspect(static_cast<float>(info.width),
                                                             static_cast<float>(info.height), opt_.stereo,
                                                             opt_.stereo_full));
        }
      }

      const int fsr_pick = fsr_req_.exchange(-1);
      if (fsr_pick >= 0) {
        const auto mode = static_cast<vrp::FsrMode>(fsr_pick);
        fsr.set_mode(mode);
        menu.set_fsr(mode);
        force_fsr_refresh = true;
        hud_paint();
      }

      if (pending_hz_resume && xr.is_session_running()) {
        const auto resume_now = std::chrono::steady_clock::now();
        if (hz_resume_attempts == 0 ||
            resume_now - last_hz_resume_try >= std::chrono::milliseconds(400)) {
          last_hz_resume_try = resume_now;
          ++hz_resume_attempts;
          if (open_media(resume_path)) {
            pending_hz_resume = false;
            if (resume_pos > 0.25) decoder.seek_to(resume_pos);
            play_wanted = resume_play;
            show_idle_screen = !play_wanted;
            session_play_wanted_.store(play_wanted);
            if (play_wanted) decoder.play();
            QMetaObject::invokeMethod(
                this,
                [this, resume_play] {
                  appendLog(resume_play ? tr("Resumed playback after Hz change")
                                        : tr("Restored video after Hz change"));
                },
                Qt::QueuedConnection);
          } else if (hz_resume_attempts >= 5) {
            pending_hz_resume = false;
            const QString err = QString::fromStdString(
                decoder.info().error.empty() ? "open failed" : decoder.info().error);
            QMetaObject::invokeMethod(
                this,
                [this, err] { appendLog(tr("Resume after Hz change failed: %1").arg(err)); },
                Qt::QueuedConnection);
          }
        }
      }

      if (video_pending_.exchange(false)) {
        std::string path;
        {
          std::lock_guard<std::mutex> lock(video_mu_);
          path = pending_video_;
          pending_video_.clear();
        }
        if (yt_session_) yt_session_->clear_busy_status();
        if (!path.empty()) {
          if (!open_media(path)) {
            const QString err = QString::fromStdString(
                decoder.info().error.empty() ? "GPU busy or open failed" : decoder.info().error);
            QMetaObject::invokeMethod(
                this,
                [this, err] { appendLog(tr("Cannot play video this session: %1").arg(err)); },
                Qt::QueuedConnection);
          } else {
            play_wanted = true;
            show_idle_screen = false;
            decoder.play();
            QMetaObject::invokeMethod(
                this, [this] { appendLog(tr("Queued video open after Virtual removal")); },
                Qt::QueuedConnection);
          }
        }
      }

      auto input = xr.poll_actions();
      bool pad_sony = false;
      bool pad_present = false;
      bool pad_nav_up = false;
      bool pad_nav_down = false;
      bool pad_nav_left = false;
      bool pad_nav_right = false;
      bool pad_face_y = false;
      // [アプリ]: LD_PRELOAD hook (preferred) or legacy uinput PadFilter, else PS+R1 chords.
      // Hook: guest owns pad while menu closed; host SDL navigates menu and sends BLOCK.
      const bool desk_app_live_pad =
          yt_session_ && yt_session_->desktop_app_mode() && yt_session_->active() &&
          !yt_session_->loading() && !yt_session_->show_busy_overlay();
      const bool desk_menu_open = menu.visible() || menu.controls_visible();
      const bool desk_app_pass = desk_app_live_pad && !desk_menu_open;
      const bool hook_on = pad_hook_active_ && pad_hook_.running();
      const bool filter_on = pad_filter_.running();
      // With hook: take SDL only while VR menu is open. With filter: never SDL while filtering.
      const bool want_gamepad =
          gamepad_enabled_.load() &&
          ((hook_on && desk_app_live_pad && desk_menu_open) ||
           (!hook_on && !filter_on && !desk_app_pass) ||
           (!hook_on && !desk_app_live_pad));

      auto apply_gp = [&](const vrp::GamepadInput::State& gp) {
        if (!gp.present) return;
        if (gp.name != last_gamepad_name || gp.connection != last_gamepad_connection) {
          const QString name = QString::fromStdString(gp.name);
          const QString how = QString::fromStdString(gp.connection);
          QMetaObject::invokeMethod(
              this,
              [this, name, how] {
                const QString shown = name.isEmpty() ? tr("SDL gamepad") : name;
                QString method = how;
                if (method.isEmpty()) method = tr("unknown connection");
                else if (method == QLatin1String("virtual")) method = tr("virtual");
                appendLog(tr("SDL gamepad: %1 (%2)").arg(shown, method));
              },
              Qt::QueuedConnection);
        }
        last_gamepad_name = gp.name;
        last_gamepad_connection = gp.connection;
        pad_present = true;
        pad_sony = gp.sony_face;
        {
          std::lock_guard<std::mutex> lock(gamepad_name_mu_);
          gamepad_seen_name_ = QString::fromStdString(gp.name);
        }
        pad_nav_up = gp.nav_up;
        pad_nav_down = gp.nav_down;
        pad_nav_left = gp.nav_left;
        pad_nav_right = gp.nav_right;
        if (std::fabs(gp.stick_x) > 0.15f || std::fabs(gp.stick_y) > 0.15f) {
          input.stick_x = gp.stick_x;
          input.stick_y = gp.stick_y;
        }
        if (std::fabs(gp.stick_rx) > 0.15f || std::fabs(gp.stick_ry) > 0.15f) {
          input.stick_rx = gp.stick_rx;
          input.stick_ry = gp.stick_ry;
        }
        input.confirm = input.confirm || gp.confirm;
        input.confirm_held = input.confirm_held || gp.confirm_held;
        input.back = input.back || gp.back;
        input.back_held = input.back_held || gp.back_held;
        input.menu_toggle = input.menu_toggle || gp.menu_toggle;
        input.seek_back = input.seek_back || gp.seek_back;
        input.seek_forward = input.seek_forward || gp.seek_forward;
        input.seek_back_held = input.seek_back_held || gp.seek_back_held;
        input.seek_forward_held = input.seek_forward_held || gp.seek_forward_held;
        input.l2_held = input.l2_held || gp.l2_held;
        input.r2_held = input.r2_held || gp.r2_held;
        input.seek_scrubbing = input.seek_scrubbing || gp.seek_scrubbing;
        input.recenter = input.recenter || gp.recenter;
        input.view_reset = input.view_reset || gp.view_reset;
        pad_face_y = gp.face_y;
      };

      if (desk_app_live_pad && hook_on) {
        if (gamepad.pass_through_watching()) gamepad.end_pass_through_watch();
        pad_hook_.set_block_guest(desk_menu_open);
        const auto edges = pad_hook_.pump();
        if (edges.menu_toggle) input.menu_toggle = true;
        if (edges.recenter) input.recenter = true;
        if (edges.client_hello) {
          QMetaObject::invokeMethod(
              this, [this] { appendLog(tr("Pad hook: guest connected")); }, Qt::QueuedConnection);
        }
        if (!desk_menu_open) gamepad_sdl_owned_.store(false);
      } else if (desk_app_live_pad && filter_on) {
        if (gamepad.pass_through_watching()) gamepad.end_pass_through_watch();
        gamepad_sdl_owned_.store(false);
        const auto edges = pad_filter_.pump(/*guest_forward=*/!desk_menu_open);
        if (edges.menu_toggle) input.menu_toggle = true;
        if (edges.recenter) input.recenter = true;
        if (desk_menu_open) apply_gp(pad_filter_.snapshot());
      } else if (desk_app_pass && !hook_on && !filter_on) {
        if (!gamepad.pass_through_watching()) {
          gamepad.begin_pass_through_watch();
          gamepad_sdl_owned_.store(false);
          gamepad_logged = false;
          last_gamepad_name.clear();
          last_gamepad_connection.clear();
          std::lock_guard<std::mutex> lock(gamepad_name_mu_);
          gamepad_seen_name_.clear();
        }
        const auto pass = gamepad.poll_pass_through();
        if (pass.menu_toggle) input.menu_toggle = true;
        if (pass.recenter) input.recenter = true;
      } else if (gamepad.pass_through_watching()) {
        gamepad.end_pass_through_watch();
      }

      if (want_gamepad) {
        gamepad_sdl_owned_.store(true);
        if (gamepad.ensure_open()) {
          const auto gp = gamepad.poll();
          if (gp.present) {
            apply_gp(gp);
          } else {
            last_gamepad_name.clear();
            last_gamepad_connection.clear();
            std::lock_guard<std::mutex> lock(gamepad_name_mu_);
            gamepad_seen_name_.clear();
          }
        } else {
          last_gamepad_name.clear();
          last_gamepad_connection.clear();
          {
            std::lock_guard<std::mutex> lock(gamepad_name_mu_);
            gamepad_seen_name_.clear();
          }
          if (!gamepad_logged) {
            gamepad_logged = true;
            QMetaObject::invokeMethod(
                this, [this] { appendLog(tr("Gamepad open failed. Recenter only")); },
                Qt::QueuedConnection);
          }
        }
      } else if (gamepad_was && !want_gamepad) {
        gamepad.close();
        gamepad_sdl_owned_.store(false);
        gamepad_logged = false;
        last_gamepad_name.clear();
        last_gamepad_connection.clear();
        std::lock_guard<std::mutex> lock(gamepad_name_mu_);
        gamepad_seen_name_.clear();
      }
      gamepad_was = want_gamepad;
      // L3+R3 recenter before live handlers (works during 起動中).
      if (input.recenter && xr.recenter()) {
        look_yaw = 0.f;
        look_pitch = 0.f;
        input.recenter = false;
      }
      {
        bool hmd_sony = false;
        const auto st = xr.status();
        if (st.vendor_id == 0x054Cu) {
          hmd_sony = true;
        } else {
          std::string n = st.system_name;
          for (char& c : n) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
          }
          hmd_sony = n.find("playstation") != std::string::npos || n.find("psvr") != std::string::npos ||
                     n.find("sony") != std::string::npos;
        }
        // Face icons follow HMD or pad. Options label follows the same Sony source.
        const bool sony = hmd_sony || pad_sony;
        const bool option = sony;
        if (face_sony_.exchange(sony) != sony || menu_option_.exchange(option) != option) hud_paint();
      }
      decoder.set_seek_scrubbing(false);

      // Flat scale: Y/△ cycles Near→Cinema→Far (fixed cinema screen width; FOV follows
      // distance). XR triangle is flat_scale; Options/menu stays menu_toggle.
      // Allowed with or without a video so the idle translucent screen can be seated too.
      const bool yt_live = yt_session_ && yt_session_->active();
      const bool yt_loading = yt_session_ && yt_session_->loading();
      // Setup spinner / portal wait: same session as live (no R-stick look).
      const bool yt_session = yt_live || yt_loading;
#if defined(VRP_HAS_PIPEWIRE)
      vrp::PipewireCapture* yt_cap = yt_session_ ? yt_session_->capture() : nullptr;
#else
      vrp::PipewireCapture* yt_cap = nullptr;
      (void)yt_cap;
#endif
      const bool menus_closed =
          !menu.visible() && !menu.controls_visible() && !controls_help_.load();
      const bool can_flat_scale =
          opt_.projection == vrp::ProjectionMode::Flat && (menus_closed || yt_session);
      const bool scale_press = pad_face_y || input.flat_scale;
      if (can_flat_scale && scale_press) {
        flat_scale_index = vrp::next_flat_scale_preset(flat_scale_index);
        const auto& preset = vrp::kFlatScalePresets[flat_scale_index];
        opt_.flat_fov_deg = preset.fov_deg;
        opt_.screen_distance = preset.distance_m;
        scene.set_projection(opt_.projection, opt_.stereo, opt_.flat_fov_deg, opt_.screen_distance);
        setMonasphereConfKey(QStringLiteral("flat_fov"),
                             QString::number(static_cast<double>(preset.fov_deg), 'f', 2));
        setMonasphereConfKey(QStringLiteral("flat_distance"),
                             QString::number(static_cast<double>(preset.distance_m), 'f', 2));
        const QString toast =
            QStringLiteral("%1  %2° / %3 m")
                .arg(QString::fromUtf8(preset.name))
                .arg(static_cast<double>(preset.fov_deg), 0, 'f', 1)
                .arg(static_cast<double>(preset.distance_m), 0, 'f', 0);
        showHudToast(toast);
        QMetaObject::invokeMethod(
            this, [this, toast] { appendLog(tr("Flat scale: %1").arg(toast)); }, Qt::QueuedConnection);
      }

      // Safe live teardown on XR: drop dma-buf imports before PipeWire stop.
      {
        const bool capture_dead =
            yt_session_ && yt_session_->active() && yt_cap && !yt_cap->running();
        if (capture_dead) yt_stop_requested_.store(true, std::memory_order_release);
        if (yt_stop_requested_.exchange(false, std::memory_order_acq_rel)) {
          // Keep yt_return_apps_menu_ until YouTubeSession::stopped — then [アプリ] menu.
          if (!scene.wait_previous_submit(0)) {
            while (!xr_stop_.load() && !scene.wait_previous_submit(50'000'000ull)) {
            }
          }
#if defined(VRP_HAS_PIPEWIRE)
          pw_dma_tex.reset();
#endif
          audio.request_stop_live_monitor();
          audio.wait_live_monitor_idle(800);
          if (yt_session_) yt_session_->stop_capture_from_xr();
          yt_audio_pending_log_ = false;
          yt_audio_fail_retries_ = 0;
          // Keep Flat busy spinner through teardown (stop() owns the status text).
          play_wanted = false;
          show_idle_screen = false;
          if (yt_session_) {
            const bool reuse_next =
                yt_pending_kind_ != YtPendingKind::None && vdisp_mgr_ &&
                vdisp_mgr_->matches(virtualSpecForSpan(yt_pending_span_));
            yt_session_->set_busy_status(reuse_next ? tr("切替中") : tr("Virtual 削除中"));
          }
          opt_.projection = vrp::ProjectionMode::Flat;
          opt_.stereo = vrp::StereoLayout::Mono;
          opt_.stereo_full = true;
          scene.set_projection(opt_.projection, opt_.stereo, opt_.flat_fov_deg, opt_.screen_distance);
          menu.set_format(opt_.projection, opt_.stereo, opt_.stereo_full);
          scene.set_content_aspect(16.f / 9.f);
          fsr_need_process = false;
          fsr_src_y = fsr_src_uv = VK_NULL_HANDLE;
          fsr_src_samp = VK_NULL_HANDLE;
          fsr_src_w = fsr_src_h = 0;
          fsr_src_nv12 = 0;
          (void)fsr.poll();
          look_yaw = 0.f;
          look_pitch = 0.f;
          scene.set_view_offset(0.f, 0.f);
          hud_paint();
          // Host setup keeps suppress; HMD open clears it in startLiveFromHmd.
          // Delay UI stop(): DMA reset first, then app/Virtual — avoids DEVICE_LOST
          // when KWin tears the output under a still-submitting XR frame.
          const uint64_t stop_gen = yt_stop_gen_.load(std::memory_order_acquire);
          QMetaObject::invokeMethod(
              this,
              [this, stop_gen] {
                QTimer::singleShot(350, this, [this, stop_gen] {
                  if (yt_stop_gen_.load(std::memory_order_acquire) != stop_gen) return;
                  if (!yt_session_) return;
                  if (yt_session_->state() == vrp::YouTubeSession::State::Idle &&
                      !yt_session_->active() && !yt_session_->loading()) {
                    return;
                  }
                  const bool reuse_next =
                      yt_pending_kind_ != YtPendingKind::None && vdisp_mgr_ &&
                      vdisp_mgr_->matches(virtualSpecForSpan(yt_pending_span_));
                  yt_session_->set_busy_status(reuse_next ? tr("切替中") : tr("Virtual 削除中"));
                  yt_session_->stop();
                  std::thread([] { (void)vrp::PulseMonitor::restore_browser_routing(); }).detach();
                  appendLog(tr("Live capture stopped (after DMA settle)"));
                });
              },
              Qt::QueuedConnection);
        }
      }

      // [アプリ] return: only after stop() destroyed Virtual / portal (see stopped()).
      if (yt_show_apps_after_stop_.exchange(false, std::memory_order_acq_rel)) {
        play_wanted = false;
        show_idle_screen = true;
        if (yt_session_) yt_session_->clear_busy_status();
        (void)texture.upload_idle_screen();
        if (texture.view()) scene.set_texture(texture.view(), texture.sampler());
        scene.set_content_aspect(16.f / 9.f);
        menu.show_apps_list();
        help_swallow_back_ = true;
        help_swallow_menu_ = true;
        hud_paint();
      }

      // Live audio → Flat stereo→4.0 OpenAL (never block XR on Pulse/pactl).
      // Chrome PCM is diverted to a hidden null sink, monitored, then the same
      // surround path as file Flat (side-diff rear + OpenAL Soft HRTF).
      if (yt_live && yt_session_ && yt_session_->audio_wanted() && !audio.live_monitor_active() &&
          !audio.live_monitor_busy()) {
        yt_audio_pending_log_ = true;
        audio.request_start_live_monitor();
      } else if (yt_audio_pending_log_ && !audio.live_monitor_busy()) {
        yt_audio_pending_log_ = false;
        if (audio.live_monitor_active()) {
          yt_audio_fail_retries_ = 0;
          if (yt_session_) yt_session_->clear_audio_wanted();
          QMetaObject::invokeMethod(
              this,
              [this] {
                appendLog(tr("Live audio: Chrome → Flat surround (OpenAL Soft HRTF 4.0)"));
              },
              Qt::QueuedConnection);
        } else {
          const QString err = QString::fromStdString(audio.last_error());
          if (yt_live && yt_audio_fail_retries_ < 40) {
            ++yt_audio_fail_retries_;
            if (yt_audio_fail_retries_ == 1 || yt_audio_fail_retries_ % 5 == 0) {
              QMetaObject::invokeMethod(
                  this,
                  [this, err, n = yt_audio_fail_retries_] {
                    appendLog(tr("Live audio: Flat path retry %1 (%2)")
                                  .arg(n)
                                  .arg(err.isEmpty() ? tr("waiting") : err));
                  },
                  Qt::QueuedConnection);
            }
          } else {
            if (yt_session_) yt_session_->clear_audio_wanted();
            QMetaObject::invokeMethod(
                this,
                [this, err] {
                  appendLog(tr("Live audio: Flat path failed (%1) — system sink only")
                                .arg(err.isEmpty() ? tr("unknown") : err));
                },
                Qt::QueuedConnection);
          }
        }
      } else if (!yt_live && audio.live_monitor_active() && !audio.live_monitor_busy()) {
        yt_audio_pending_log_ = false;
        yt_audio_fail_retries_ = 0;
        audio.request_stop_live_monitor();
      }
      if (yt_live || audio.live_monitor_active()) {
        for (const std::string& s : audio.drain_audio_status()) {
          const QString note = QString::fromStdString(s);
          QMetaObject::invokeMethod(
              this, [this, note] { appendLog(note); }, Qt::QueuedConnection);
        }
      }

      // Live session (loading or streaming): L stick = cursor, R stick = scroll (no look).
      // Options long = VR menu (stream keeps running). ○ does not stop live.
      // Switching app / opening video from the menu stops after Virtual is removed, then
      // launches the new target (see open_live / open_desktop_app / open_video below).
      if (yt_session) {
        auto exit_live_to_menu = [&] {
          // Same-frame: stop flag + busy spinner before ScreenCast can re-attach.
          yt_stop_requested_.store(true, std::memory_order_release);
          play_wanted = false;
          show_idle_screen = false;
          if (yt_session_) yt_session_->set_busy_status(tr("Virtual 削除中"));
          opt_.projection = vrp::ProjectionMode::Flat;
          opt_.stereo = vrp::StereoLayout::Mono;
          opt_.stereo_full = true;
          scene.set_projection(opt_.projection, opt_.stereo, opt_.flat_fov_deg, opt_.screen_distance);
          menu.set_format(opt_.projection, opt_.stereo, opt_.stereo_full);
          scene.set_content_aspect(16.f / 9.f);
          if (!scene.wait_previous_submit(0)) {
            while (!xr_stop_.load() && !scene.wait_previous_submit(50'000'000ull)) {
            }
          }
          pending_video_rebind = false;
          fsr_need_process = false;
          fsr_src_y = fsr_src_uv = VK_NULL_HANDLE;
          fsr_src_samp = VK_NULL_HANDLE;
          fsr_src_w = fsr_src_h = 0;
          fsr_src_nv12 = 0;
          (void)fsr.poll();
#if defined(VRP_HAS_PIPEWIRE)
          pw_dma_tex.reset();
#endif
          thumb_latched = false;
          wait_stop_frame = false;
          stop_frame_shown = false;
          look_yaw = 0.f;
          look_pitch = 0.f;
          scene.set_view_offset(0.f, 0.f);
          menu.show_file_browser();
          help_swallow_back_ = true;
          help_swallow_menu_ = true;
          hud_paint();
          QMetaObject::invokeMethod(this, [this] { stopYouTubeCapture(); }, Qt::QueuedConnection);
        };

        // YouTube live: L stick = mouse, R stick = scroll (paused while VR menu is open).
        // [アプリ] live: no remap — SDL pad is released so the app sees the controller.
        const bool desk_app_live = yt_session_->desktop_app_mode();
        const bool live_menu_open = menu.visible() || menu.controls_visible();
        {
          static bool yt_inject_logged = false;
          static bool yt_focus_armed = false;
          static bool yt_prev_up = false, yt_prev_down = false, yt_prev_left = false,
                      yt_prev_right = false;
          static bool desk_pass_logged = false;
          if (!yt_live) {
            yt_inject_logged = false;
            yt_focus_armed = false;
            yt_prev_up = yt_prev_down = yt_prev_left = yt_prev_right = false;
            desk_pass_logged = false;
          } else if (desk_app_live || live_menu_open) {
            if (desk_app_live && !desk_pass_logged) {
              desk_pass_logged = true;
              const bool hook = pad_hook_active_ && pad_hook_.running();
              const bool filt = pad_filter_.running();
              QMetaObject::invokeMethod(
                  this,
                  [this, hook, filt] {
                    if (hook) {
                      appendLog(tr("Desktop app: LD_PRELOAD pad hook — PS long=menu, L3+R3=recenter"));
                    } else if (filt) {
                      appendLog(tr("Desktop app: legacy uinput filter — PS long=menu, L3+R3=recenter"));
                    } else {
                      appendLog(tr("Desktop app: pad chords — PS+R1 menu, PS+L1 recenter"));
                    }
                  },
                  Qt::QueuedConnection);
            }
          } else if (!yt_session_->x11().can_inject()) {
            if (!yt_inject_logged) {
              yt_inject_logged = true;
              QMetaObject::invokeMethod(
                  this,
                  [this] {
                    appendLog(tr("Live: pointer inject unavailable (no EIS / inject target)"));
                  },
                  Qt::QueuedConnection);
            }
          } else {
            yt_inject_logged = false;
            // First input after live: re-activate Chrome (ScreenCast steals focus).
            const bool want_ptr = std::fabs(input.stick_x) > 0.12f ||
                                  std::fabs(input.stick_y) > 0.12f ||
                                  std::fabs(input.stick_ry) > 0.18f || input.confirm ||
                                  pad_nav_up || pad_nav_down || pad_nav_left || pad_nav_right;
            if (want_ptr && !yt_focus_armed) {
              yt_focus_armed = true;
              QMetaObject::invokeMethod(
                  this,
                  [this] {
                    QString err;
                    (void)vrp::KwinWindowPlacer::activate_youtube_window({}, &err);
                    if (yt_session_) (void)yt_session_->x11().ensure_inject_target();
                  },
                  Qt::QueuedConnection);
            }

            // Stick speed scales with Virtual mode width so 1080p and 4K feel alike.
            // live_cursor_scale multiplies gain (1–6). Full stick ≈ 0.45×width/s at scale 1.
            constexpr float kCursorDead = 0.16f;
            constexpr float kCursorExp = 3.4f;
            constexpr float kWidthsPerSec = 0.45f;
            const float cscale =
                static_cast<float>(std::clamp(live_cursor_scale_, 1, 6));
            const float px_w =
                static_cast<float>(std::max(320, yt_session_->place_w()));
            const float base_px = px_w * kWidthsPerSec;
            const float lx = input.stick_x;
            const float ly = input.stick_y;
            const float mag = std::sqrt(lx * lx + ly * ly);
            if (mag > kCursorDead) {
              const float t =
                  std::clamp((mag - kCursorDead) / (1.f - kCursorDead), 0.f, 1.f);
              const float shaped = std::pow(t, kCursorExp);
              const float speed = shaped * base_px * cscale;
              const float nx = lx / mag;
              const float ny = ly / mag;
              yt_session_->x11().move_cursor(nx * speed * static_cast<float>(dt),
                                             -ny * speed * static_cast<float>(dt));
            }
            // R stick (vertical) → mouse wheel.
            {
              constexpr float kScrollDead = 0.18f;
              constexpr float kScrollNotchesPerSec = 14.f;
              const float ry = input.stick_ry;
              if (std::fabs(ry) > kScrollDead) {
                const float t =
                    std::clamp((std::fabs(ry) - kScrollDead) / (1.f - kScrollDead), 0.f, 1.f);
                (void)yt_session_->x11().scroll(std::copysign(t, ry) * kScrollNotchesPerSec *
                                     static_cast<float>(dt));
              }
            }
            // D-pad: ~4% of mode width per tap, also × live_cursor_scale.
            const float kStep = std::max(24.f, px_w * 0.04f) * cscale;
            auto step = [&](bool now, bool& prev, float dx, float dy) {
              if (now && !prev) yt_session_->x11().move_cursor(dx, dy);
              prev = now;
            };
            step(pad_nav_up, yt_prev_up, 0.f, -kStep);
            step(pad_nav_down, yt_prev_down, 0.f, kStep);
            step(pad_nav_left, yt_prev_left, -kStep, 0.f);
            step(pad_nav_right, yt_prev_right, kStep, 0.f);
            if (input.confirm) yt_session_->x11().click();
          }
        }
        // Options long opens/closes the VR menu without stopping live (handled by VrMenu).
        // ○ never ends live; YouTube short ○ = browser Back. Loading: ○ cancels start.
        if (yt_live && !live_menu_open && !desk_app_live && input.back &&
            yt_session_->x11().can_inject()) {
          (void)yt_session_->x11().key_back();
        } else if (!yt_live && (input.back || input.back_held)) {
          // Loading: ○ cancels ScreenCast wait.
          exit_live_to_menu();
        }
      }

      if (hud_toast_active_.load() && !hudToastActive()) {
        {
          std::lock_guard<std::mutex> lock(hud_toast_mu_);
          hud_toast_.clear();
        }
        hud_toast_active_.store(false);
        hud_paint();
      }

      // Controls help (startup only): close with B/× only — no countdown, no other keys.
      if (controls_help_.load() && !help_shown && (input.back || input.back_held)) {
        help_swallow_back_ = true;
        hideControlsHelp();
      }

      vrp::VrMenu::PadInput pad;
      if (help_swallow_back_ && !input.back_held && !input.back) help_swallow_back_ = false;
      if (help_swallow_confirm_ && !input.confirm && !input.confirm_held)
        help_swallow_confirm_ = false;
      if (help_swallow_menu_ && !input.menu_toggle) help_swallow_menu_ = false;
      // Options long always reaches the menu (video + live + [アプリ]), like playback.
      // While live and the menu is closed, leave sticks/confirm for cursor or the app.
      // While the help card is still up, do not drive the VR menu underneath it.
      if (!controls_help_.load()) {
        if (!help_swallow_menu_) pad.menu_toggle = input.menu_toggle;
        const bool drive_menu = !yt_session || menu.visible() || menu.controls_visible();
        if (drive_menu) {
          pad.stick_x = input.stick_x;
          pad.stick_y = input.stick_y;
          pad.nav_up = pad_nav_up;
          pad.nav_down = pad_nav_down;
          pad.nav_left = pad_nav_left;
          pad.nav_right = pad_nav_right;
          if (!help_swallow_confirm_) {
            pad.confirm = input.confirm;
            pad.confirm_held = input.confirm_held;
          }
          if (!help_swallow_back_) {
            pad.back = input.back;
            pad.back_held = input.back_held;
          }
        }
      }
      if (!yt_session) {
        pad.l2_held = input.l2_held;
        pad.r2_held = input.r2_held;
        pad.seek_back_held = input.seek_back_held;
        pad.seek_forward_held = input.seek_forward_held;
      }
      const bool menu_was = menu.visible();
      const bool controls_was = menu.controls_visible();
      if (decoder.is_open()) {
        menu.set_thumb_cover(decoder.thumb_cover_sec());
        menu.set_thumb_bar_force(!decoder.thumbnails_finished());
      } else {
        menu.set_thumb_bar_force(false);
      }
      const auto menu_out = menu.update(pad, ui_dt);
      if (menu.visible() != menu_was || menu.controls_visible() != controls_was) hud_paint();
      auto apply_projection = [&](vrp::ProjectionMode proj, vrp::StereoLayout stereo, bool stereo_full) {
        opt_.projection = proj;
        opt_.stereo = stereo;
        opt_.stereo_full = stereo_full;
        flat_scale_index = vrp::find_flat_scale_preset(opt_.flat_fov_deg, opt_.screen_distance);
        scene.set_projection(opt_.projection, opt_.stereo, opt_.flat_fov_deg, opt_.screen_distance);
        menu.set_format(opt_.projection, opt_.stereo, opt_.stereo_full);
        if (decoder.is_open()) {
          const auto& info = decoder.info();
          scene.set_content_aspect(vrp::frame_display_aspect(static_cast<float>(info.width),
                                                             static_cast<float>(info.height), opt_.stereo,
                                                             opt_.stereo_full));
        }
      };
      auto restore_media_projection = [&] {
        if (!decoder.is_open()) return;
        const std::string& path = decoder.info().path;
        if (path.empty()) return;
        vrp::detect_format_from_filename(path, opt_.projection, opt_.stereo, opt_.stereo_full);
        apply_projection(opt_.projection, opt_.stereo, opt_.stereo_full);
      };

      auto prepare_live_flat = [&] {
        play_wanted = false;
        show_idle_screen = true;
        if (decoder.is_open()) decoder.pause();
        if (audio.is_open()) audio.pause();
        apply_projection(vrp::ProjectionMode::Flat, vrp::StereoLayout::Mono, true);
        menu.set_format(opt_.projection, opt_.stereo, opt_.stereo_full);
        fsr_need_process = false;
        fsr_src_y = fsr_src_uv = VK_NULL_HANDLE;
        fsr_src_samp = VK_NULL_HANDLE;
        fsr_src_w = fsr_src_h = 0;
        fsr_src_nv12 = 0;
        (void)fsr.poll();
        (void)texture.upload_idle_screen();
        if (texture.view()) scene.set_texture(texture.view(), texture.sampler());
        hud_paint();
      };

      if (menu_out.open_live && !menu_out.live_url.empty()) {
        const QString live_url = QString::fromStdString(menu_out.live_url);
        if (yt_session_ &&
            (yt_session_->active() || yt_session_->loading() ||
             yt_session_->state() != vrp::YouTubeSession::State::Idle)) {
          const bool reuse = vdisp_mgr_ && vdisp_mgr_->matches(virtualSpecForSpan(1));
          yt_session_->set_busy_status(reuse ? tr("切替中") : tr("Virtual 削除中"));
          show_idle_screen = false;
        }
        QMetaObject::invokeMethod(
            this, [this, live_url] { startLiveFromHmd(live_url); }, Qt::QueuedConnection);
        prepare_live_flat();
        if (yt_session_ && !yt_session_->busy_status().isEmpty()) show_idle_screen = false;
      }

      if (menu_out.open_desktop_app && !menu_out.desktop_app_exec.empty()) {
        const QString desk_exec = QString::fromStdString(menu_out.desktop_app_exec);
        QStringList desk_args;
        desk_args.reserve(static_cast<int>(menu_out.desktop_app_args.size()));
        for (const std::string& a : menu_out.desktop_app_args) {
          desk_args.push_back(QString::fromStdString(a));
        }
        const int desk_span = menu_out.desktop_monitor_span > 0 ? menu_out.desktop_monitor_span : 1;
        const QString desk_label = QString::fromStdString(menu_out.desktop_app_label);
        if (yt_session_ &&
            (yt_session_->active() || yt_session_->loading() ||
             yt_session_->state() != vrp::YouTubeSession::State::Idle)) {
          const bool reuse = vdisp_mgr_ && vdisp_mgr_->matches(virtualSpecForSpan(desk_span));
          yt_session_->set_busy_status(reuse ? tr("切替中") : tr("Virtual 削除中"));
          show_idle_screen = false;
        }
        QMetaObject::invokeMethod(
            this,
            [this, desk_exec, desk_args, desk_span, desk_label] {
              startLiveDesktopApp(desk_exec, desk_args, desk_span, desk_label);
            },
            Qt::QueuedConnection);
        prepare_live_flat();
        if (yt_session_ && !yt_session_->busy_status().isEmpty()) show_idle_screen = false;
      }

      if (menu_out.open_video) {
        const std::string vpath = menu_out.video_path;
        const bool session_busy =
            yt_live || yt_loading ||
            (yt_session_ &&
             (yt_session_->active() || yt_session_->loading() ||
              yt_session_->state() != vrp::YouTubeSession::State::Idle));
        const bool virtual_up =
            (yt_session_ && yt_session_->virtual_display_alive()) ||
            vrp::KdeVirtualDisplay::anyManagedOutputExists();
        if (session_busy || virtual_up) {
          // Never open_media while Flat DMA / Virtual still live — HMD freezes (audio may play).
          {
            std::lock_guard<std::mutex> lock(video_mu_);
            pending_video_ = vpath;
          }
          yt_await_video_after_stop_.store(true, std::memory_order_release);
          if (yt_session_) {
            yt_session_->set_busy_status(tr("Virtual 削除中"));
          }
          play_wanted = false;
          show_idle_screen = false;
          prepare_live_flat();
          if (session_busy) {
            yt_stop_requested_.store(true, std::memory_order_release);
          } else {
            QMetaObject::invokeMethod(
                this, [this] { finishQueuedVideoAfterVirtualGone(); }, Qt::QueuedConnection);
          }
          QMetaObject::invokeMethod(
              this,
              [this] {
                appendLog(tr("Video queued — waiting until Virtual is removed"));
              },
              Qt::QueuedConnection);
        } else if (!open_media(vpath)) {
          const QString err = QString::fromStdString(
              decoder.info().error.empty() ? "GPU busy or open failed" : decoder.info().error);
          QMetaObject::invokeMethod(
              this, [this, err] { appendLog(tr("HMD: cannot open video: %1").arg(err)); },
              Qt::QueuedConnection);
        } else {
          play_wanted = true;
          show_idle_screen = false;
          decoder.play();
        }
        hud_paint();
      } else if (menu_out.play_toggle) {
        // Do not also handle raw input.confirm this frame — open_video closes the menu while
        // confirm is still true and would immediately pause.
        if (decoder.is_playing() || decoder.is_priming()) {
          play_wanted = false;
          decoder.pause();
          audio.pause();
        } else {
          play_wanted = true;
          show_idle_screen = false;
          // Stop left us on Flat idle; put 180/360 back before the first frame.
          restore_media_projection();
          decoder.play();
        }
      }
      if (menu_out.apply_format) {
        apply_projection(menu_out.projection, menu_out.stereo, menu_out.stereo_full);
        QMetaObject::invokeMethod(
            this,
            [this] {
              appendLog(tr("HMD format: %1 / %2")
                            .arg(QString::fromUtf8(vrp::projection_name(opt_.projection)))
                            .arg(QString::fromUtf8(
                                vrp::stereo_detect_name(opt_.stereo, opt_.stereo_full))));
            },
            Qt::QueuedConnection);
        hud_paint();
      }
      if (menu_out.play) {
        play_wanted = true;
        show_idle_screen = false;
        restore_media_projection();
        decoder.play();
      }
      if (menu_out.pause) {
        play_wanted = false;
        decoder.pause();
        audio.pause();
      }
      if (menu_out.stop) {
        play_wanted = false;
        show_idle_screen = true;
        decoder.pause();
        decoder.seek_to(menu.place_seek_bar(0.0));
        // Must seek audio demuxer to 0 as well — pause alone leaves decode at the stop point,
        // so the next play() can briefly emit that PCM (and Pulse may still hold it).
        if (audio.is_open()) audio.stop();
        // Idle placeholder is a flat panel — leave 180/360 so the sky/seat stay readable.
        // Keep Near/Cinema/Far seat (fov/distance); restore the panel's physical 16:9 size
        // so a 2:1 VR frame does not leave a oversized idle screen.
        apply_projection(vrp::ProjectionMode::Flat, vrp::StereoLayout::Mono, true);
        scene.set_content_aspect(16.f / 9.f);
        // Stop ends VR viewing — reset stick look to front.
        look_yaw = 0.f;
        look_pitch = 0.f;
        scene.set_view_offset(0.f, 0.f);
        if (!scene.wait_previous_submit(0)) {
          while (!xr_stop_.load() && !scene.wait_previous_submit(50'000'000ull)) {
          }
        }
        pending_video_rebind = false;
        fsr_need_process = false;
        fsr_src_y = fsr_src_uv = VK_NULL_HANDLE;
        fsr_src_samp = VK_NULL_HANDLE;
        fsr_src_w = fsr_src_h = 0;
        fsr_src_nv12 = 0;
        // Drain an in-flight FSR pass without rebinding it — otherwise the post-end_frame
        // poll queues the last video frame over the idle slate on the next eye submit.
        (void)fsr.poll();
        (void)texture.upload_idle_screen();
        scene.set_texture(texture.view(), texture.sampler());
        thumb_latched = false;
        wait_stop_frame = false;
        stop_frame_shown = false;
        hud_paint();
      }
      if (menu_out.rate_changed) {
        decoder.set_rate(menu_out.rate);
        audio.set_rate(menu_out.rate);
        menu.set_rate(menu_out.rate);
        // Rate change is a pause plus the normal play start, so audio is armed
        // with the picture instead of catching up afterwards.
        if (play_wanted && (decoder.is_playing() || decoder.is_priming())) {
          decoder.pause();
          audio.pause();
          decoder.play();
        }
        hud_paint();
      }
      if (menu_out.fsr_changed) {
        fsr.set_mode(menu_out.fsr);
        menu.set_fsr(menu_out.fsr);
        force_fsr_refresh = true;
        preferred_fsr_ = menu_out.fsr;
        QMetaObject::invokeMethod(
            this,
            [this, mode = menu_out.fsr] {
              preferred_fsr_ = mode;
              saveAppConf();
              rebuildFsrMenu();
              appendLog(QString("FSR1: %1").arg(QString::fromUtf8(vrp::fsr_mode_name(mode))));
            },
            Qt::QueuedConnection);
        hud_paint();
      }
      if (menu_out.refresh_hz_restart) {
        preferred_hz_ = menu_out.preferred_hz;
        menu.set_preferred_hz(preferred_hz_);
        const int want = preferred_hz_;
        // Must not disconnect/join XR from this thread — hand off to UI thread.
        QMetaObject::invokeMethod(
            this,
            [this, want] {
              preferred_hz_ = want;
              setMonasphereConfKey(QStringLiteral("hz"), QString::number(want));
              rebuildHzMenu();
              applyPreferredRefreshRate(/*from_ui=*/true, /*force_monado_restart=*/true);
            },
            Qt::QueuedConnection);
        hud_paint();
      }
      if (std::fabs(menu_out.volume_delta) > 0.f) {
        audio.set_volume(audio.volume() + menu_out.volume_delta);
        menu.set_volume(audio.volume());
        hud_paint();
      }
      // The bar moves on its own. Playback pauses and a prebuilt thumb fills the screen.
      // On release, one seek and the still stays up. Playback starts 0.5s after the
      // stick or shoulder is released, from the frame at that time.
      const bool file_ui = menu.visible();
      const bool scrubbing = !file_ui && menu_out.seek_scrubbing;
      const bool seek_release = !file_ui && menu_out.seek;
      const bool seek_now = seek_release || (!file_ui && menu_out.seek_start);
      auto arm_stop_seek = [&](double sec) {
        audio.pause();
        decoder.pause();
        decoder.seek_to(sec);
        stop_seek_sent = true;
        wait_stop_frame = true;
        stop_frame_shown = false;
        stop_seq = decoder.frame_seq();
        stop_pts = -1.0;
        stop_upload_sent = false;
        seek_settle = true;
        seek_settle_from = now;
        hud_paint();
      };
      if (scrubbing) {
        if (!was_scrubbing) menu.clear_seek_bar_drawn();
        decoder.pause();
        audio.pause();
        decoder.set_seek_scrubbing(true);
        thumb_latched = true;
        seek_settle = false;
        wait_stop_frame = false;
        stop_frame_shown = false;
        stop_seek_sent = false;
        stop_pts = -1.0;
        stop_upload_sent = false;
        stop_seek_pending = false;
      } else if (seek_release) {
        stop_seek_pending = true;
        stop_seek_sec = menu_out.seek_sec;
      } else if (was_scrubbing && !wait_stop_frame && !seek_settle && !stop_seek_pending) {
        thumb_latched = false;
      }
      if (stop_seek_pending && !seek_paint_wait) {
        arm_stop_seek(stop_seek_sec);
        stop_seek_pending = false;
      }
      if (!file_ui && menu_out.seek_start) {
        arm_stop_seek(menu.place_seek_bar(0.0));
      }
      was_scrubbing = scrubbing;
      const bool seek_settling =
          seek_settle && std::chrono::duration<double>(now - seek_settle_from).count() < 0.5;
      const bool hold_play = scrubbing || seek_settling || wait_stop_frame || stop_seek_pending;
      // Never pause/park while live monitor owns OpenAL (Flat HRTF path).
      if (hold_play && !yt_live && !audio.live_monitor_active()) audio.pause();

      if (decoder.consume_av_resync()) {
        const bool resume = play_wanted && !hold_play;
        if (resume) decoder.play();
        else if ((!play_wanted || hold_play) && !yt_live && !audio.live_monitor_active())
          audio.park_at(decoder.position());
      }

      // Sole start: audible audio, then the first new frame. Seek-in-flight waits
      // so we arm at the landed PTS rather than the requested time.
      if (decoder.consume_playback_started() && play_wanted && !hold_play && !decoder.is_playing() &&
          !decoder.seek_in_flight()) {
        const double arm = stop_pts >= 0.0 ? stop_pts : decoder.position();
        if (audio.is_open()) audio.arm_at(arm);
        decoder.start_presentation();
        stop_pts = -1.0;
      }

      // EOF auto-loop: treat as playback end → face forward again for 180/360.
      if (decoder.consume_looped()) {
        look_yaw = 0.f;
        look_pitch = 0.f;
      }

      menu.set_playback(decoder.is_playing() || decoder.is_priming(), decoder.position());
      menu.set_volume(audio.volume());
      menu.set_preferred_hz(preferred_hz_);
      session_play_wanted_.store(play_wanted);
      if (decoder.is_open()) {
        std::lock_guard<std::mutex> lock(session_media_mu_);
        session_video_path_ = decoder.info().path;
        session_video_pos_ = decoder.position();
      }
      {
        const auto st_hz = xr.status();
        if (st_hz.display_hz > 0.0) menu.set_display_hz(static_cast<float>(st_hz.display_hz));
      }
      // Video end-of-stream loop → keep audio on the same timeline (avoids post-repeat silence).
      if (decoder.consume_looped() && audio.is_open() && decoder.seek_in_flight()) audio.mute_output();
      menu.note_seek_bar(!menu.visible() && !menu.controls_visible() &&
                         (scrubbing || seek_now || stop_seek_pending || menu.seek_gliding() || seek_paint_wait));
      static bool seek_overlay_was = false;
      const bool seek_overlay = menu.seek_bar_only();
      if (seek_overlay != seek_overlay_was) {
        hud_paint();
        seek_overlay_was = seek_overlay;
      }
      if (menu.visible() || menu.controls_visible() || seek_overlay) {
        apply_preview_result();
        // Decode a thumbnail only after the highlight has stayed put.
        const std::string preview_hi = menu.highlighted_video_path();
        const std::string row_hi = menu.highlighted_row_id();
        bool hud_dirty = false;
        if (row_hi != highlight_row) {
          highlight_row = row_hi;
          if (preview_hi != preview_wait_path) {
            preview_wait_path = preview_hi;
            preview_wait_since = now;
            if (preview_path_done != preview_hi) {
              preview_gen.fetch_add(1, std::memory_order_acq_rel);
              preview_path_done.clear();
              menu.clear_preview();
            }
          }
          hud_dirty = true;
        }
        // One seek-bar paint per UI update. The clock does not wait on that paint.
        if (ui_dt > 0.f && (menu.list_gliding() || menu.seek_gliding())) {
          if (menu.seek_gliding()) {
            const uint64_t g = menu.publish_seek_bar();
            if (g != 0) {
              seek_paint_wait = true;
              seek_paint_gen = g;
              seek_paint_pixels = 0;
            }
          }
          hud_dirty = true;
        }
        if (!preview_hi.empty() && now - preview_wait_since >= std::chrono::milliseconds(450)) {
          request_preview(preview_hi);
        }
        static auto last_hud_paint = std::chrono::steady_clock::time_point{};
        // DLNA spinner needs ~60 fps; idle browser can stay at 10 fps.
        const auto hud_interval =
            menu.network_busy() ? std::chrono::milliseconds(16) : std::chrono::milliseconds(100);
        if (hud_dirty || now - last_hud_paint > hud_interval) {
          last_hud_paint = now;
          hud_paint();
        }
      }

      if (audio_reopen_.exchange(false) && !opt_.mute) {
        std::string adev;
        {
          std::lock_guard<std::mutex> lock(audio_mu_);
          adev = audio_device_;
        }
        const std::string path = decoder.is_open() ? decoder.info().path : std::string{};
        audio.mute_output();
        if (!audio.ensure_output(adev)) {
          const QString err = QString::fromStdString(audio.last_error());
          QMetaObject::invokeMethod(
              this, [this, err] { appendLog(tr("Audio switch failed: %1").arg(err)); }, Qt::QueuedConnection);
        } else if (!path.empty() && audio.switch_file(path)) {
          if (play_wanted && !decoder.seek_in_flight()) {
            decoder.pause();
            audio.pause();
            decoder.play();
          } else {
            audio.park_at(decoder.position());
          }
          const QString dev = QString::fromStdString(audio.device_name());
          QMetaObject::invokeMethod(
              this, [this, dev] { (void)dev; }, Qt::QueuedConnection);
        } else {
          audio.clear_file();
          const QString dev = QString::fromStdString(audio.device_name());
          QMetaObject::invokeMethod(
              this, [this, dev] { (void)dev; }, Qt::QueuedConnection);
        }
      }

      // Status UI is sampled after end_frame while rendering (so 応答 ms is fresh).
      if (!xr.is_session_running()) {
        if (now - last_status > std::chrono::milliseconds(500)) {
          last_status = now;
          auto cur = xr.status();
          QString health = "Wait";
          QString detail = "Waiting for session";
          if (cur.session_state == "LOSS_PENDING" || cur.session_state == "EXITING") {
            health = "Error";
            detail = "Session lost";
            xr_stop_.store(true);
          }
          QMetaObject::invokeMethod(
              this,
              [this, cur, health, detail] {
                applyStatus(QString::fromStdString(cur.system_name),
                            QString::fromStdString(cur.session_state), health, detail,
                            cur.tracking_valid, cur.view_width, cur.view_height, cur.display_hz,
                            cur.app_frame_ms, QString::fromStdString(cur.sync_note),
                            cur.refresh_rate_ext, QString::fromStdString(cur.refresh_rates));
              },
              Qt::QueuedConnection);
        }
        if (xr_stop_.load()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        continue;
      }

      const auto frame_t0 = std::chrono::steady_clock::now();
      if (xr_stop_.load()) break;
      vrp::XrVulkanApp::FrameInfo fi;
      if (!xr.begin_frame(fi)) {
        if (xr_stop_.load()) break;
        continue;
      }
      if (xr_stop_.load()) {
        // Drop this frame and exit — request_exit already issued from UI.
        XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
        ei.displayTime = fi.display_time;
        ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        (void)xrEndFrame(xr.session(), &ei);
        break;
      }
      if (xr.maybe_init_tracking_origin()) {
        showControlsHelp();
        help_shown = true;
      }

      // While the bar is moving, show the prebuilt thumb at or behind the bar. After release, keep it
      // until the decoded frame at the stop time is bound, then start from that frame.
      if (wait_stop_frame && !scrubbing && stop_pts < 0.0 && decoder.frame_seq() != stop_seq) {
        vrp::VideoFrame landed;
        if (decoder.take_latest(landed)) {
          stop_pts = landed.pts_sec;
          if (landed.gpu || landed.rgba.empty()) {
            stop_upload_sent = true;
          } else {
            pending_upload = std::move(landed);
            stop_upload_sent = false;
          }
        }
      }
      const bool reveal_stop = wait_stop_frame && !scrubbing && stop_pts >= 0.0;
      const bool show_thumb =
          thumb_latched && (scrubbing || stop_seek_pending || (wait_stop_frame && !reveal_stop));
      const bool seek_visual = show_thumb && !reveal_stop;
      // Stage video binds for flush after the eye-submit fence inside render_frame.
      // Do not wait here — wait(0) almost always fails while the previous frame is in flight.
      // Idle translucent panel (startup / Stop) must not be overwritten by a paused seek frame.
#if defined(VRP_HAS_PIPEWIRE)
      // Tools → Set up must not paint live capture onto the HMD (leftover frames after stop).
      const bool yt_suppress = yt_hmd_suppress_live_.load(std::memory_order_acquire);
      if (!yt_suppress) yt_suppress_idle_uploaded = false;
      if (yt_suppress) {
        show_idle_screen = true;
        scene.set_content_cursor(0.5f, 0.5f, false);
        // Still exercise DMA-BUF → Vulkan import (NVIDIA experiment) without painting.
        // Do not reset the texture every frame — that aborted arming/import.
        if (pw_dma && yt_session_ && yt_cap && yt_cap->running() &&
            !yt_session_->dma_disabled() && !pw_dma_tex.import_abandoned()) {
          vrp::PipewireDmaFrame dma;
          if (yt_cap->take_latest_dma(dma, yt_session_->dma_seq())) {
            if (pw_dma_tex.try_import(dma)) {
              if (!yt_session_->dma_logged()) {
                yt_session_->dma_logged() = true;
                const QString note =
                    tr("display path: PipeWire DMA-BUF zero-copy %1x%2 modifier=0x%3 (setup/no paint)")
                        .arg(dma.width)
                        .arg(dma.height)
                        .arg(dma.modifier, 0, 16);
                QMetaObject::invokeMethod(
                    this, [this, note] { appendLog(note); }, Qt::QueuedConnection);
              }
            } else if (pw_dma_tex.import_abandoned()) {
              yt_session_->dma_disabled() = true;
              const QString err = QString::fromStdString(pw_dma_tex.last_error());
              const QString note =
                  tr("ScreenCast: DMA-BUF import failed (%1, modifier=0x%2) — CPU RGBA fallback")
                      .arg(err.isEmpty() ? tr("unknown") : err)
                      .arg(dma.modifier, 0, 16);
              QMetaObject::invokeMethod(
                  this, [this, note] { appendLog(note); }, Qt::QueuedConnection);
            }
          }
        }
        if (!seek_visual) {
          if (!yt_suppress_idle_uploaded) {
            (void)texture.upload_idle_screen();
            yt_suppress_idle_uploaded = true;
          }
          if (texture.view()) scene.set_texture(texture.view(), texture.sampler());
          scene.set_content_aspect(16.f / 9.f);
          fsr_need_process = false;
        }
      }
      // Spinner while loading / Virtual create-destroy. busy_status keeps the overlay
      // even if show_idle was set (live↔app switch waits on Virtual 削除中).
      else if (!seek_visual && !opt_.clear_only && yt_session_ &&
               ((yt_session_->show_busy_overlay() && !yt_session_->active()) ||
                (!show_idle_screen && yt_session_->active() && yt_cap && yt_cap->running()))) {
        if (yt_session_->loading() || yt_session_->active() || yt_session_->show_busy_overlay())
          show_idle_screen = false;
        bool yt_picture = false;
        auto set_yt_aspect = [&](int tw, int th) {
          int aw = tw, ah = th;
          if (yt_session_ && yt_session_->virtual_w() > 0 && yt_session_->virtual_h() > 0) {
            aw = yt_session_->virtual_w();
            ah = yt_session_->virtual_h();
          } else {
            virtualSizeForSpan(live_session_span_, &aw, &ah);
          }
          scene.set_content_aspect(static_cast<float>(std::max(1, aw)) /
                                   static_cast<float>(std::max(1, ah)));
        };
        // Desktop capture is sRGB/gamma; Flat shader expects linear unless rgba_gamma.
        auto bind_last_yt = [&] {
          if (pw_dma && !(yt_session_ && yt_session_->dma_disabled()) && pw_dma_tex.view()) {
            scene.set_texture_gamma(pw_dma_tex.view(), pw_dma_tex.sampler());
            set_yt_aspect(pw_dma_tex.width(), pw_dma_tex.height());
            fsr_need_process = false;
            return true;
          }
          if (texture.view() && texture.width() > 8 && texture.height() > 8) {
            scene.set_texture_gamma(texture.view(), texture.sampler());
            set_yt_aspect(texture.width(), texture.height());
            fsr_need_process = false;
            return true;
          }
          return false;
        };

        // Skip DMA only while tearing down (切替/削除/終了). 「起動中」must still import
        // so mark_frame_presented() can clear the spinner — otherwise it spins forever.
        const QString yt_busy = yt_session_->busy_status();
        const bool yt_busy_teardown =
            yt_busy.contains(QStringLiteral("切替")) || yt_busy.contains(QStringLiteral("削除")) ||
            yt_busy.contains(QStringLiteral("終了"));
        if (yt_live && yt_cap && yt_cap->running() && !yt_busy_teardown) {
          {
            const auto status = yt_cap->drain_status();
            for (const std::string& s : status) {
              const QString note = QString::fromStdString(s);
              QMetaObject::invokeMethod(
                  this, [this, note] { appendLog(note); }, Qt::QueuedConnection);
            }
          }
          bool imported_dma = false;
          if (pw_dma && !(yt_session_ && yt_session_->dma_disabled()) &&
              !pw_dma_tex.import_abandoned()) {
            vrp::PipewireDmaFrame dma;
            if (yt_cap->take_latest_dma(dma, yt_session_->dma_seq())) {
              if (pw_dma_tex.try_import(dma)) {
                imported_dma = true;
                if (yt_session_ && !yt_session_->dma_logged()) {
                  yt_session_->dma_logged() = true;
                  const QString note =
                      tr("display path: PipeWire DMA-BUF zero-copy %1x%2 modifier=0x%3")
                          .arg(dma.width)
                          .arg(dma.height)
                          .arg(dma.modifier, 0, 16);
                  QMetaObject::invokeMethod(
                      this, [this, note] { appendLog(note); }, Qt::QueuedConnection);
                }
              } else if (pw_dma_tex.import_abandoned()) {
                // Hard fail only — soft skips (arming / missing modifier) keep CPU path.
                if (yt_session_) yt_session_->dma_disabled() = true;
                const QString err = QString::fromStdString(pw_dma_tex.last_error());
                const QString note =
                    tr("ScreenCast: DMA-BUF import failed (%1, modifier=0x%2) — CPU RGBA fallback")
                        .arg(err.isEmpty() ? tr("unknown") : err)
                        .arg(dma.modifier, 0, 16);
                QMetaObject::invokeMethod(
                    this, [this, note] { appendLog(note); }, Qt::QueuedConnection);
              }
            }
          }
          const bool dma_live =
              imported_dma || (pw_dma && !(yt_session_ && yt_session_->dma_disabled()) && pw_dma_tex.has_valid_frame());
          bool got_cpu = false;
          if (!dma_live) {
            vrp::VideoFrame yf;
            if (yt_cap->take_latest(yf, yt_session_->frame_seq()) && !yf.rgba.empty()) {
              (void)texture.try_upload(yf);
              got_cpu = true;
            }
          }
          if (imported_dma && pw_dma_tex.poll_present() && pw_dma_tex.view()) {
            scene.set_texture_gamma(pw_dma_tex.view(), pw_dma_tex.sampler());
            set_yt_aspect(pw_dma_tex.width(), pw_dma_tex.height());
            fsr_need_process = false;
            yt_picture = true;
          } else if (dma_live && !(yt_session_ && yt_session_->dma_disabled()) && pw_dma_tex.view()) {
            scene.set_texture_gamma(pw_dma_tex.view(), pw_dma_tex.sampler());
            set_yt_aspect(pw_dma_tex.width(), pw_dma_tex.height());
            fsr_need_process = false;
            yt_picture = true;
          } else if (got_cpu) {
            if (texture.poll_present() && texture.view()) {
              scene.set_texture_gamma(texture.view(), texture.sampler());
              set_yt_aspect(texture.width(), texture.height());
              fsr_need_process = false;
              yt_picture = true;
            } else if (bind_last_yt()) {
              yt_picture = true;
            }
          } else if (!yt_loading && bind_last_yt()) {
            yt_picture = true;
          }
          if (yt_picture && yt_session_) yt_session_->mark_frame_presented();
          // Portal AvailableCursorModes=0 → Hidden stream; draw stick cursor on Flat.
          if (yt_picture && yt_session_ && !yt_session_->embedded_cursor() &&
              opt_.projection == vrp::ProjectionMode::Flat) {
            scene.set_content_cursor(yt_session_->x11().cursor_u(), yt_session_->x11().cursor_v(),
                                     true);
          } else {
            scene.set_content_cursor(0.5f, 0.5f, false);
          }
        } else {
          scene.set_content_cursor(0.5f, 0.5f, false);
        }

        if (yt_session_->show_busy_overlay() && !yt_picture) {
          yt_spin_phase += static_cast<float>(dt) * 9.5f;
          if (yt_spin_phase > 6.2831853f) yt_spin_phase = std::fmod(yt_spin_phase, 6.2831853f);
          const int sw = std::clamp(yt_session_->virtual_w() > 0 ? yt_session_->virtual_w()
                                                                : (live_virtual_w_ > 0 ? live_virtual_w_ : 1280),
                                    320, 1920);
          const int sh = std::clamp(yt_session_->virtual_h() > 0 ? yt_session_->virtual_h()
                                                                : (live_virtual_h_ > 0 ? live_virtual_h_ : 720),
                                    180, 1080);
          const QString status = yt_session_->busy_status();
          const QImage busy = vrp::paint_live_busy(sw, sh, yt_spin_phase, status);
          if (!busy.isNull()) {
            vrp::VideoFrame ph;
            ph.width = busy.width();
            ph.height = busy.height();
            ph.rgba.resize(static_cast<size_t>(ph.width) * static_cast<size_t>(ph.height) * 4u);
            for (int y = 0; y < busy.height(); ++y) {
              const uchar* src = busy.constScanLine(y);
              uint8_t* dst = ph.rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(ph.width) * 4u;
              std::memcpy(dst, src, static_cast<size_t>(ph.width) * 4u);
            }
            (void)texture.try_upload(ph);
          }
          if (texture.poll_present() && texture.view()) {
            scene.set_texture(texture.view(), texture.sampler());
            scene.set_content_aspect(static_cast<float>(sw) / static_cast<float>(std::max(1, sh)));
            fsr_need_process = false;
          }
        }
      } else
#endif
      if (!seek_visual && !opt_.clear_only && decoder.is_open() && !show_idle_screen &&
          !(yt_session_ && yt_session_->active())) {
        scene.set_content_cursor(0.5f, 0.5f, false);
        bool video_updated = false;
        int nv12_mode = 0;
        VkImageView src_y = VK_NULL_HANDLE;
        VkImageView src_uv = VK_NULL_HANDLE;
        VkSampler src_samp = VK_NULL_HANDLE;
        int src_w = 0;
        int src_h = 0;

        if (decoder.gpu_path_active()) {
          nv12_mode = cuda_tex.full_range() ? 2 : 1;
          src_y = cuda_tex.y_view();
          src_uv = cuda_tex.uv_view();
          src_samp = cuda_tex.sampler();
          src_w = decoder.display_width() > 0 ? decoder.display_width() : decoder.width();
          src_h = decoder.display_height() > 0 ? decoder.display_height() : decoder.height();
          if (cuda_tex.poll_present()) {
            pending_video_rebind = true;
            video_updated = true;
          }
          vrp::VideoFrame discard;
          (void)decoder.update(dt, discard);
        } else if (decoder.vaapi_path_active()) {
          nv12_mode = vaapi_tex.full_range() ? 2 : 1;
          src_y = vaapi_tex.y_view();
          src_uv = vaapi_tex.uv_view();
          src_samp = vaapi_tex.sampler();
          src_w = vaapi_tex.width();
          src_h = vaapi_tex.height();
          if (vaapi_tex.poll_present()) {
            pending_video_rebind = true;
            video_updated = true;
          }
          if (vaapi_hud_pending) {
            menu.set_output_note("0-copy NV12");
            vaapi_hud_pending = false;
          }
          vrp::VideoFrame discard;
          (void)decoder.update(dt, discard);
        } else {
          vrp::VideoFrame frame;
          if (decoder.update(dt, frame) && !frame.rgba.empty()) {
            pending_upload = std::move(frame);
          }
          if (!pending_upload.rgba.empty() && texture.try_upload(pending_upload)) {
            pending_upload = {};
            if (reveal_stop) stop_upload_sent = true;
          }
          if (texture.poll_present() && (!reveal_stop || stop_upload_sent)) {
            pending_video_rebind = true;
            video_updated = true;
          }
          src_y = texture.view();
          src_uv = texture.view();
          src_samp = texture.sampler();
          src_w = texture.width();
          src_h = texture.height();
        }

        if (force_fsr_refresh) pending_video_rebind = true;

        // XR critical path: only publish completed FSR / queue raw. Never submit FSR here —
        // compute + queue contention before end_frame causes head-turn jitter.
        if (fsr.enabled()) {
          if (fsr.poll() && fsr.output_view() && fsr.output_sampler()) {
            scene.queue_texture_gamma(fsr.output_view(), fsr.output_sampler());
            force_fsr_refresh = false;
          }
          if (pending_video_rebind && src_y && src_samp && src_w > 0 && src_h > 0) {
            fsr_src_y = src_y;
            fsr_src_uv = src_uv ? src_uv : src_y;
            fsr_src_samp = src_samp;
            fsr_src_w = src_w;
            fsr_src_h = src_h;
            fsr_src_nv12 = nv12_mode;
            fsr_need_process = true;
            // Bootstrap until the first FSR front buffer exists.
            if (!fsr.output_view()) {
              if (decoder.gpu_path_active() || decoder.vaapi_path_active()) {
                scene.queue_nv12_texture(src_y, src_uv ? src_uv : src_y, src_samp,
                                         decoder.gpu_path_active() ? cuda_tex.full_range()
                                                                   : vaapi_tex.full_range());
              } else {
                scene.queue_texture(src_y, src_samp);
              }
            }
            pending_video_rebind = false;
            force_fsr_refresh = false;
          }
        } else if (pending_video_rebind && src_y && src_samp && src_w > 0 && src_h > 0) {
          if (decoder.gpu_path_active()) {
            scene.queue_nv12_texture(src_y, src_uv, src_samp, cuda_tex.full_range());
          } else if (decoder.vaapi_path_active()) {
            scene.queue_nv12_texture(src_y, src_uv, src_samp, vaapi_tex.full_range());
          } else {
            scene.queue_texture(src_y, src_samp);
          }
          pending_video_rebind = false;
          force_fsr_refresh = false;
        }
        if (reveal_stop && video_updated) stop_frame_shown = true;
      }

      // The still stays up through the 0.5s settle. Play starts after that, at the landed PTS.
      if (stop_frame_shown && !seek_settling && !scrubbing && !decoder.seek_in_flight() &&
          !decoder.is_playing() && !decoder.is_priming()) {
        wait_stop_frame = false;
        thumb_latched = false;
        seek_settle = false;
        if (play_wanted) {
          decoder.play();
          if (decoder.consume_playback_started()) {
            const double arm = stop_pts >= 0.0 ? stop_pts : decoder.position();
            if (audio.is_open()) audio.arm_at(arm);
            decoder.start_presentation();
            stop_pts = -1.0;
          }
        }
        stop_frame_shown = false;
      }

      // HMD menu: cached atlas on the GPU, quads placed every paint.
      {
        static uint64_t seen_paint = 0;
        static uint64_t resident_atlas = 0;
        static uint64_t inflight_atlas = 0;
        static uint64_t applied_paint = 0;
        static int mesh_kind = 0;
        static bool atlas_resize = false;
        static std::vector<vrp::HudVertex> pending_verts;
        static uint64_t pending_atlas = 0;
        static int pending_kind = 0;
        static double pending_seek_sec = 0;
        vrp::VideoFrame hud_frame;
        uint64_t upload_gen = 0;
        bool have_atlas = false;
        {
          std::lock_guard<std::mutex> lock(hud_pixels_mu_);
          if (hud_pixels_gen_ != seen_paint) {
            pending_verts = hud_verts_;
            pending_atlas = hud_mesh_atlas_gen_;
            pending_kind = hud_pixels_kind_;
            pending_seek_sec = hud_painted_seek_sec_;
            seen_paint = hud_pixels_gen_;
          }
          if (hud_atlas_gen_ != 0 && hud_atlas_gen_ != resident_atlas && hud_atlas_gen_ != inflight_atlas &&
              hud_atlas_w_ > 0 && !hud_pixels_.empty()) {
            hud_frame.width = hud_atlas_w_;
            hud_frame.height = hud_atlas_h_;
            hud_frame.rgba = hud_pixels_;
            upload_gen = hud_atlas_gen_;
            have_atlas = true;
          }
        }
        const bool help_on = controls_help_.load();
        const bool toast_on = hudToastActive();
        const int want_kind = help_on ? 1
                                      : menu.visible() ? 2
                                      : menu.controls_visible() ? 3
                                      : menu.seek_bar_only() ? 4
                                      : toast_on                 ? 5
                                                                 : 0;
        if (hud_tex.poll_present() && inflight_atlas != 0) {
          resident_atlas = inflight_atlas;
          atlas_resize = false;
        }
        if (have_atlas && want_kind != 0) {
          if (hud_tex.width() > 0 &&
              (hud_tex.width() != hud_frame.width || hud_tex.height() != hud_frame.height)) {
            atlas_resize = true;
          }
          (void)scene.wait_previous_submit(5'000'000ull);
          if (hud_tex.try_upload(hud_frame)) inflight_atlas = upload_gen;
        }
        constexpr float kHudDistanceM = 1.5f;
        constexpr float kHudAspect = 1.05f;
        auto apply_layout = [&](int kind) {
          if (kind == 1 || kind == 2) {
            scene.set_hud_layout(0.70f, kHudDistanceM, kHudAspect);
          } else if (kind == 3 || kind == 4) {
            constexpr float kControlsHalfW = 0.72f;
            constexpr float kOldAspect = 720.f / 1280.f;
            const float y = kControlsHalfW * (kHudAspect - kOldAspect);
            scene.set_hud_layout(kControlsHalfW, kHudDistanceM, kHudAspect, y);
          } else if (kind == 5) {
            scene.set_hud_layout(0.55f, kHudDistanceM, kHudAspect);
          }
        };
        const bool atlas_ready =
            resident_atlas != 0 && pending_kind == want_kind && pending_atlas <= resident_atlas &&
            !pending_verts.empty();
        const bool keep_seek_bar = mesh_kind == 4 && (scrubbing || stop_seek_pending || menu.seek_bar_only());
        if ((want_kind == 0 || atlas_resize) && !keep_seek_bar) {
          scene.set_hud_mesh(nullptr, 0);
          scene.set_hud_texture(hud_tex.view(), hud_tex.sampler(), false);
          mesh_kind = 0;
          applied_paint = 0;
        } else if (atlas_ready) {
          if (applied_paint != seen_paint) {
            scene.set_hud_mesh(pending_verts.data(), static_cast<uint32_t>(pending_verts.size()));
            applied_paint = seen_paint;
          }
          apply_layout(want_kind);
          scene.set_hud_texture(hud_tex.view(), hud_tex.sampler(), true);
          mesh_kind = want_kind;
          hud_mesh_applied = applied_paint;
          // Kind 3 is the playback panel, which already draws the bar. Kind 4 is the
          // bar alone. Either one finishing is enough to release the stop-seek.
          const bool seek_hud = want_kind == 3 || want_kind == 4;
          if (seek_hud && seek_paint_wait && seek_paint_pixels == 0 &&
              hud_paint_done_.load(std::memory_order_acquire) >= seek_paint_gen) {
            std::lock_guard<std::mutex> lock(hud_pixels_mu_);
            seek_paint_pixels = hud_pixels_gen_;
          }
          if (want_kind == 4 && pending_kind == 4) menu.note_seek_bar_drawn(pending_seek_sec);
          if (seek_hud &&
              (!seek_paint_wait || (seek_paint_pixels != 0 && hud_mesh_applied >= seek_paint_pixels))) {
            seek_paint_wait = false;
          }
        } else if ((mesh_kind == want_kind || (want_kind == 4 && mesh_kind == 4)) && resident_atlas != 0) {
          apply_layout(want_kind);
          scene.set_hud_texture(hud_tex.view(), hud_tex.sampler(), true);
        } else {
          scene.set_hud_texture(hud_tex.view(), hud_tex.sampler(), false);
        }

        // Same sample as the bar that was just shown.
        if (show_thumb) {
          if (nvidia_bc7) {
            const int idx = bc7.index_for(menu.seek_bar_drawn() ? menu.seek_drawn_sec() : menu.seek_bar_sec());
            VkImageView thumb_view = bc7.view(idx);
            if (idx >= 0 && idx != thumb_idx && thumb_view) {
              scene.set_texture(thumb_view, bc7.sampler());
              thumb_idx = idx;
            }
          } else {
            vrp::VideoFrame thumb;
            if (decoder.copy_thumb(menu.seek_bar_drawn() ? menu.seek_drawn_sec() : menu.seek_bar_sec(), thumb_idx,
                                   thumb)) {
              pending_thumb = std::move(thumb);
              have_pending_thumb = true;
            }
            if (have_pending_thumb && seek_tex.try_upload(pending_thumb)) {
              have_pending_thumb = false;
              pending_thumb = {};
            }
            if (seek_tex.poll_present() && seek_tex.view()) {
              scene.set_texture(seek_tex.view(), seek_tex.sampler());
            }
          }
        }

        static uint64_t uploaded_thumb = 0;
        static bool thumb_shown = false;
        if (thumb_tex.poll_present()) thumb_shown = true;
        vrp::VideoFrame thumb_frame;
        bool thumb_upload = false;
        bool thumb_pixels = false;
        float thumb_aspect = 9.f / 16.f;
        vrp::ProjectionMode thumb_proj = vrp::ProjectionMode::Flat;
        vrp::StereoLayout thumb_stereo = vrp::StereoLayout::Mono;
        bool thumb_full = true;
        if (want_kind == 2) {
          const std::string hi = menu.highlighted_video_path();
          if (!hi.empty()) vrp::detect_format_from_filename(hi, thumb_proj, thumb_stereo, thumb_full);
        }
        {
          std::lock_guard<std::mutex> lock(hud_pixels_mu_);
          thumb_pixels = thumb_w_ > 0 && thumb_h_ > 0 && !thumb_pixels_.empty();
          if (thumb_pixels) {
            if (thumb_proj != vrp::ProjectionMode::Flat) {
              thumb_aspect = 1.f;
            } else {
              const float disp = vrp::frame_display_aspect(static_cast<float>(thumb_w_),
                                                           static_cast<float>(thumb_h_), thumb_stereo,
                                                           thumb_full);
              thumb_aspect = 1.f / disp;
            }
          }
          if (thumb_pixels && thumb_gen_ != uploaded_thumb) {
            thumb_frame.width = thumb_w_;
            thumb_frame.height = thumb_h_;
            thumb_frame.rgba = thumb_pixels_;
            uploaded_thumb = thumb_gen_;
            thumb_upload = true;
          }
        }
        if (thumb_upload && want_kind == 2) {
          (void)scene.wait_previous_submit(5'000'000ull);
          (void)thumb_tex.try_upload(thumb_frame);
        }
        if (want_kind == 2 && thumb_pixels && thumb_shown && thumb_tex.view()) {
          scene.set_thumb_format(thumb_proj, thumb_stereo);
          scene.set_thumb_pose(0.46f, thumb_aspect, 0.66f, 0.12f, kHudDistanceM - 0.18f);
          scene.set_thumb_texture(thumb_tex.view(), thumb_tex.sampler(), true);
        } else {
          scene.set_thumb_texture(thumb_tex.view(), thumb_tex.sampler(), false);
          if (!thumb_pixels || want_kind != 2) thumb_shown = false;
        }
      }

      if (input.view_reset) {
        look_yaw = 0.f;
        look_pitch = 0.f;
      }
      float look_x = 0.f;
      float look_y = 0.f;
      // R-stick look: 180° / 360° VR video only (not Flat, not live).
      const bool vr_look =
          decoder.is_open() && !show_idle_screen &&
          (opt_.projection == vrp::ProjectionMode::Deg180 ||
           opt_.projection == vrp::ProjectionMode::Deg360) &&
          !(yt_session_ && (yt_session_->active() || yt_session_->loading()));
      if (vr_look) {
        auto look_axis = [](float v) {
          constexpr float kDead = 0.20f;
          const float mag = std::fabs(v);
          if (mag <= kDead) return 0.f;
          const float t = std::clamp((mag - kDead) / (1.f - kDead), 0.f, 1.f);
          return std::copysign(t, v);
        };
        look_x = look_axis(input.stick_rx);
        look_y = look_axis(input.stick_ry);
        constexpr float kLookRad = 120.f * 3.14159265f / 180.f;
        const float look_dt = static_cast<float>(dt);
        look_yaw -= look_x * kLookRad * look_dt;
        look_pitch += look_y * kLookRad * look_dt;
        constexpr float kPitchLimit = 80.f * 3.14159265f / 180.f;
        look_pitch = std::clamp(look_pitch, -kPitchLimit, kPitchLimit);
        constexpr float kPi = 3.14159265f;
        if (look_yaw > kPi) look_yaw -= 2.f * kPi;
        else if (look_yaw < -kPi) look_yaw += 2.f * kPi;
      } else if (opt_.projection == vrp::ProjectionMode::Flat || show_idle_screen) {
        // Flat / idle: keep view centered (do not leave a leftover VR look offset).
        look_yaw = 0.f;
        look_pitch = 0.f;
      }
      scene.set_view_offset(look_yaw, look_pitch);
      scene.set_comfort_grid(look_x != 0.f || look_y != 0.f);

      // Flat / live: world-locked speakers + HMD listener (same OpenAL Soft HRTF).
      if (audio.spatial_active() || audio.live_monitor_active()) {
        const auto& q = fi.views[0].pose.orientation;
        const float lx = -2.f * (q.x * q.z + q.w * q.y);
        const float ly = -2.f * (q.y * q.z - q.w * q.x);
        const float lz = -(1.f - 2.f * (q.x * q.x + q.y * q.y));
        const float hmd_yaw = std::atan2(lx, -lz);
        const float hmd_pitch = std::atan2(ly, std::sqrt(lx * lx + lz * lz));
        // Stick look rotates content opposite the OpenAL yaw/pitch sense used for HMD.
        const float lis_yaw = hmd_yaw - look_yaw;
        const float lis_pitch = hmd_pitch - look_pitch;
        audio.set_listener_orientation(lis_yaw, lis_pitch);
        if (audio.live_monitor_active()) {
          static double last_lis_log = 0.0;
          const double tnow =
              std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
                  .count();
          if (tnow - last_lis_log >= 2.0) {
            last_lis_log = tnow;
            const QString note =
                tr("live audio: HRTF listener yaw=%1° pitch=%2° (head-tracked)")
                    .arg(lis_yaw * 180.f / 3.14159265f, 0, 'f', 1)
                    .arg(lis_pitch * 180.f / 3.14159265f, 0, 'f', 1);
            QMetaObject::invokeMethod(
                this, [this, note] { appendLog(note); }, Qt::QueuedConnection);
          }
        }
      }

      scene.render_frame(xr, fi, proj_views, layer);
      std::vector<XrCompositionLayerBaseHeader*> layers = {
          reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer)};
      xr.end_frame(fi, layers);

      // FSR after end_frame so predictedDisplayTime submit is not delayed by upscale.
      // Skip idle slate and live RGBA — a late poll must not replace those panels.
      if (fsr.enabled() && !show_idle_screen && !(yt_session_ && yt_session_->active()) &&
          !(yt_session_ && yt_session_->loading())) {
        if (fsr.poll() && fsr.output_view() && fsr.output_sampler()) {
          scene.queue_texture_gamma(fsr.output_view(), fsr.output_sampler());
        }
        if (fsr_need_process && !fsr.busy() && fsr_src_y && fsr_src_samp && fsr_src_w > 0 &&
            fsr_src_h > 0) {
          (void)fsr.process(fsr_src_y, fsr_src_uv ? fsr_src_uv : fsr_src_y, fsr_src_samp, fsr_src_w,
                            fsr_src_h, fsr_src_nv12);
          fsr_need_process = false;
          if (fsr.poll() && fsr.output_view() && fsr.output_sampler()) {
            scene.queue_texture_gamma(fsr.output_view(), fsr.output_sampler());
          }
        }
      } else if (fsr.enabled() &&
                 (show_idle_screen || (yt_session_ && (yt_session_->active() || yt_session_->loading())))) {
        (void)fsr.poll();
        fsr_need_process = false;
      }

      const double frame_ms =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frame_t0).count();
      xr.note_frame_ms(frame_ms);

      if (now - last_status > std::chrono::milliseconds(500)) {
        last_status = now;
        auto cur = xr.status();
        if (!logged_session_refresh_hz_ && cur.display_hz > 0.0) {
          logged_session_refresh_hz_ = true;
          const double hz = cur.display_hz;
          const int want = effectivePreferredHz();
          QMetaObject::invokeMethod(
              this,
              [this, hz, want] {
                if (want > 0 && std::fabs(hz - static_cast<double>(want)) > 5.0) {
                  appendLog(tr("Want %1 Hz, measured %2 Hz")
                                .arg(want)
                                .arg(hz, 0, 'f', 1));
                  if (!monado_hz_mismatch_retried_) {
                    monado_hz_mismatch_retried_ = true;
                    const int idx = findModeIndexForHz(want);
                    if (idx >= 0) {
                      monado_desired_mode_ = idx;
                      appendLog(tr("Hz mismatch — restart Monado (mode %1 ≈ %2 Hz)")
                                    .arg(idx)
                                    .arg(monado_mode_hz_.value(idx), 0, 'f', 1));
                      applyPreferredRefreshRate(/*from_ui=*/false, /*force_monado_restart=*/true);
                    }
                  }
                }
              },
              Qt::QueuedConnection);
        }
        const bool healthy = cur.session_running && (cur.focused || cur.session_state == "VISIBLE" ||
                                                     cur.session_state == "SYNCHRONIZED" ||
                                                     cur.session_state == "FOCUSED");
        QString health = "Wait";
        QString detail = "Waiting for session";
        if (cur.session_state == "LOSS_PENDING" || cur.session_state == "EXITING") {
          health = "Error";
          detail = "Session lost";
          xr_stop_.store(true);
          // Virtual delete can kill Monado mid-switch — never open portal on a dead HMD.
          QMetaObject::invokeMethod(
              this,
              [this] {
                if (yt_pending_kind_ == YtPendingKind::None) return;
                appendLog(tr("Live: queued switch cancelled — XR session lost"));
                yt_pending_kind_ = YtPendingKind::None;
                yt_pending_url_.clear();
                yt_pending_exec_.clear();
                yt_pending_args_.clear();
                yt_pending_label_.clear();
                yt_pending_span_ = 1;
                if (yt_session_) yt_session_->clear_busy_status();
              },
              Qt::QueuedConnection);
        } else if (healthy) {
          health = cur.tracking_valid ? "OK" : "Wait";
          detail = cur.tracking_valid ? "Drawing" : "Pose not ready";
        }
        if (decoder.is_open() && gpu_nv12) {
          static auto last_vid_dbg = std::chrono::steady_clock::time_point{};
          if (now - last_vid_dbg > std::chrono::seconds(2)) {
            last_vid_dbg = now;
            const QString vid =
                QString("NV12 copy ok=%1 fail=%2 flips=%3 valid=%4 %5x%6 gpu=%7 err=%8")
                    .arg(cuda_tex.copy_ok())
                    .arg(cuda_tex.copy_fail())
                    .arg(cuda_tex.present_flips())
                    .arg(cuda_tex.has_valid_frame() ? 1 : 0)
                    .arg(cuda_tex.width())
                    .arg(cuda_tex.height())
                    .arg(decoder.gpu_path_active() ? 1 : 0)
                    .arg(QString::fromStdString(cuda_tex.last_error()));
            QMetaObject::invokeMethod(
                this, [this, vid] { appendLog(tr("Video DBG: %1").arg(vid)); }, Qt::QueuedConnection);
          }
        }
        // Use this frame's measured ms (not a stale pre-render sample).
        const double shown_ms = frame_ms;
        QMetaObject::invokeMethod(
            this,
            [this, cur, health, detail, shown_ms] {
              applyStatus(QString::fromStdString(cur.system_name),
                          QString::fromStdString(cur.session_state), health, detail, cur.tracking_valid,
                          cur.view_width, cur.view_height, cur.display_hz, shown_ms,
                          QString::fromStdString(cur.sync_note), cur.refresh_rate_ext,
                          QString::fromStdString(cur.refresh_rates));
            },
            Qt::QueuedConnection);
      }
    }

    decoder.stop_thumbnail_build();
    join_preview_worker();
    decoder.halt_decode();
    audio.close();
    VRP_LOG("teardown: halt decode done");
    cuda_tex.prepare_shutdown();
    cuda_tex.drain_copies();
    // Bound GPU waits — after physical unplug vkQueueWaitIdle can hang forever and
    // leave the Vulkan instance alive if the UI thread detaches us.
    (void)scene.wait_previous_submit(100'000'000ull);
    if (!xr_stop_.load()) {
      std::lock_guard<std::mutex> qlock(xr.queue_mutex());
      (void)vkQueueWaitIdle(xr.queue());
    } else {
      VRP_LOG("teardown: skip vkQueueWaitIdle (stop/unplug)");
    }
    scene.set_texture(texture.view(), texture.sampler());
    scene.set_hud_texture(VK_NULL_HANDLE, VK_NULL_HANDLE, false);
    scene.set_thumb_texture(VK_NULL_HANDLE, VK_NULL_HANDLE, false);
    VRP_LOG("teardown: reset NV12 slots (before decoder.close)");
    cuda_tex.reset_slots(decoder.cuda_context());
    decoder.close();
    bc7.shutdown();
    VRP_LOG("teardown: decoder closed");
    fsr.shutdown();
    cuda_tex.shutdown();
    vaapi_tex.shutdown();
#if defined(VRP_HAS_PIPEWIRE)
    pw_dma_tex.shutdown();
#endif
    thumb_tex.shutdown();
    hud_tex.shutdown();
    seek_tex.shutdown();
    texture.shutdown();
    scene.shutdown();
    xr.shutdown();
    VRP_LOG("teardown: complete (Vulkan instance destroyed)");
  } catch (const std::exception& e) {
    const QString msg = QString::fromUtf8(e.what());
    if (!shutting_down_.load()) {
      QMetaObject::invokeMethod(
          this,
          [this, msg] {
            if (shutting_down_.load()) return;
            appendLog(UiTr(msg));
            monado_auto_connect_in_progress_ = false;
            syncDisconnectedUi(msg);
            setHealth("Error", msg);
            if (!user_suppressed_auto_connect_) {
              monado_auto_connect_cooldown_ms_ = QDateTime::currentMSecsSinceEpoch() + 1000;
              QTimer::singleShot(500, this, [this] { maybeAutoConnectHeadset(); });
            }
          },
          Qt::QueuedConnection);
    }
  }
  xr_running_.store(false);
  if (!shutting_down_.load()) {
    QMetaObject::invokeMethod(
        this,
        [this] {
          if (shutting_down_.load()) return;
          // Manual disconnect already synced UI; only handle unexpected session end.
          if (user_suppressed_auto_connect_) {
            if (!connect_ui_enabled_) {
              syncDisconnectedUi(tr("Session ended"));
            }
            return;
          }
          if (!connect_ui_enabled_) {
            monado_auto_connect_in_progress_ = false;
            syncDisconnectedUi(tr("Waiting for HMD"));
            QTimer::singleShot(500, this, [this] { maybeAutoConnectHeadset(); });
          }
        },
        Qt::QueuedConnection);
  }
}
