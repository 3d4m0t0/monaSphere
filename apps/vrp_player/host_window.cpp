#include "host_window.hpp"
#include "hud_paint.hpp"
#include "i18n.hpp"

#include "audio/audio_player.hpp"
#include "common.hpp"
#include "input/gamepad.hpp"
#include "scene/fsr1_upscaler.hpp"
#include "scene/scene_renderer.hpp"
#include "ui/vr_menu.hpp"
#include "video/cuda_nv12_texture.hpp"
#include "video/vaapi_nv12_texture.hpp"
#include "video/video_decoder.hpp"
#include "xr/xr_vulkan_app.hpp"

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
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QWindow>
#include <QtGui/qpa/qplatformwindow_p.h>

#include <dlfcn.h>
#include <QCloseEvent>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QVBoxLayout>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMap>
#include <QRegularExpression>

#include <chrono>
#include <cmath>
#include <cstdio>
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
      beginThumbnailBuild(opt_.video_path);
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

  // Start Monado only when a single USB HMD is present (probe sees the real device).
  if (!isMonadoIpcLive()) {
    setHealth("Wait", "Starting Monado…");
    if (monado_hz_require_owned_ || (preferred_hz_ > 0 && monado_desired_mode_ >= 0)) {
      startOwnedMonado(/*allow_external_adopt=*/false);
    } else {
      startMonado();
    }
    return;
  }

  // Prefer owned process when DESIRED_MODE must apply — don't silently adopt systemd.
  if (monado_hz_require_owned_ &&
      (!monado_ || monado_->state() == QProcess::NotRunning || monado_external_)) {
    appendLog(tr("Auto-connect: replace external Monado for Hz"));
    restartOwnedMonadoForHz();
    return;
  }

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

void HostWindow::requestHudPaint() {
  hud_paint_req_.fetch_add(1);
  QMetaObject::invokeMethod(this, [this] { paintHudNow(); }, Qt::QueuedConnection);
}

void HostWindow::paintHudNow() {
  if (shutting_down_.load()) return;
  vrp::VrMenu* menu = nullptr;
  {
    std::lock_guard<std::mutex> lock(menu_mu_);
    menu = vr_menu_;
  }
  if (!menu) return;
  const auto snap = menu->snapshot();
  if (!snap.visible && !snap.controls_visible) {
    std::lock_guard<std::mutex> lock(hud_pixels_mu_);
    hud_pixels_.clear();
    hud_pixels_gen_ = snap.gen;
    hud_paint_done_.store(snap.gen);
    return;
  }
  QImage img = vrp::paint_vr_menu(snap, hud_w_, hud_h_);
  if (img.format() != QImage::Format_RGBA8888) img = img.convertToFormat(QImage::Format_RGBA8888);
  {
    std::lock_guard<std::mutex> lock(hud_pixels_mu_);
    hud_pixels_.resize(static_cast<size_t>(hud_w_ * hud_h_ * 4));
    for (int y = 0; y < hud_h_; ++y) {
      std::memcpy(hud_pixels_.data() + static_cast<size_t>(y * hud_w_ * 4), img.constScanLine(y),
                  static_cast<size_t>(hud_w_ * 4));
    }
    hud_pixels_gen_ = snap.gen;
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
}

void HostWindow::stopSystemdMonadoForOwnedStart() {
  // Socket activation will respawn monado-service without our DESIRED_MODE unless stopped.
  QProcess ctl;
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

/** DE terminal/fixed family + smallest-readable size (never fall back to proportional). */
QFont makeLogTerminalFont() {
  QFont term = QFontDatabase::systemFont(QFontDatabase::FixedFont);
  term.setStyleHint(QFont::TypeWriter);
  term.setFixedPitch(true);

  // When kdeglobals `fixed=` is unset, FixedFont can resolve to the proportional General font.
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

  const QFont small = QFontDatabase::systemFont(QFontDatabase::SmallestReadableFont);
  if (small.pointSizeF() > 0.0) term.setPointSizeF(small.pointSizeF());
  else if (small.pixelSize() > 0) term.setPixelSize(small.pixelSize());
  // Re-assert after size change (some platforms re-resolve family).
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

  // Status bar: HMD name only (表示 → HMD情報).
  hmd_label_ = new QLabel(tr("HMD: none"));
  statusBar()->addWidget(hmd_label_, 1);

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

  QSettings s(QStringLiteral("monaSphere"), QStringLiteral("monaSphere"));
  const bool show_hmd = s.value(QStringLiteral("ui/showHmdInfo"), true).toBool();
  if (act_show_hmd_info_) act_show_hmd_info_->setChecked(show_hmd);
  applyHmdInfoVisibility(show_hmd);

  setSessionChrome(false);
  refreshAudioDevices();
  rebuildConnectDeviceMenu();
  QTimer* hmd_menu_timer = new QTimer(this);
  connect(hmd_menu_timer, &QTimer::timeout, this, [this] {
    if (!xr_running_.load()) rebuildConnectDeviceMenu();
  });
  hmd_menu_timer->start(2000);

  restoreWindowGeometry();
  loadAppConf();
  syncRuntimeRadios();
  rebuildFsrMenu();
}

void HostWindow::saveAppConf() const {
  const QString dir = QDir::homePath() + QStringLiteral("/.config/monasphere");
  QDir().mkpath(dir);
  QFile f(dir + QStringLiteral("/conf"));
  QString last_dir;
  if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
    while (!f.atEnd()) {
      const QString line = QString::fromUtf8(f.readLine()).trimmed();
      if (line.startsWith(QStringLiteral("last_dir="))) last_dir = line.mid(9);
    }
    f.close();
  }
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return;
  QTextStream out(&f);
  if (!last_dir.isEmpty()) out << "last_dir=" << last_dir << '\n';
  out << "fsr=" << static_cast<int>(preferred_fsr_) << '\n';
  out << "runtime=" << (usingWivrn() ? "wivrn" : "monado") << '\n';
}

void HostWindow::loadAppConf() {
  QFile f(QDir::homePath() + QStringLiteral("/.config/monasphere/conf"));
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;
  while (!f.atEnd()) {
    const QString line = QString::fromUtf8(f.readLine()).trimmed();
    if (line.startsWith(QStringLiteral("fsr="))) {
      bool ok = false;
      const int v = line.mid(4).toInt(&ok);
      if (ok && v >= 0 && v <= 3) preferred_fsr_ = static_cast<vrp::FsrMode>(v);
    } else if (line.startsWith(QStringLiteral("runtime="))) {
      runtime_kind_.store(line.mid(8) == QLatin1String("wivrn") ? 1 : 0);
    }
  }
}

void HostWindow::restoreWindowGeometry() {
  QSettings s(QStringLiteral("monaSphere"), QStringLiteral("monaSphere"));
  const QByteArray geo = s.value(QStringLiteral("ui/geometry")).toByteArray();
  if (!geo.isEmpty() && restoreGeometry(geo)) return;
  resize(640, 480);
}

void HostWindow::saveWindowGeometry() const {
  QSettings s(QStringLiteral("monaSphere"), QStringLiteral("monaSphere"));
  s.setValue(QStringLiteral("ui/geometry"), saveGeometry());
}

void HostWindow::applyHmdInfoVisibility(bool visible) {
  if (statusBar()) statusBar()->setVisible(visible);
}

void HostWindow::onShowHmdInfoToggled(bool checked) {
  QSettings s(QStringLiteral("monaSphere"), QStringLiteral("monaSphere"));
  s.setValue(QStringLiteral("ui/showHmdInfo"), checked);
  applyHmdInfoVisibility(checked);
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

  auto* view = menuBar()->addMenu(tr("View"));
  act_show_hmd_info_ = view->addAction(tr("HMD info"));
  act_show_hmd_info_->setCheckable(true);
  act_show_hmd_info_->setChecked(true);
  connect(act_show_hmd_info_, &QAction::toggled, this, &HostWindow::onShowHmdInfoToggled);

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
      {vrp::ControllerProfile::Gamepad, QT_TR_NOOP("Gamepad")},
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
  hand_menu_->setEnabled(opt_.controller_profile != vrp::ControllerProfile::Gamepad);

  auto* help = menuBar()->addMenu(tr("Help"));
  help->addAction(tr("About"), this, &HostWindow::showAboutDialog);
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
                           .arg(QStringLiteral("1.0.0"));
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
      {0, QT_TR_NOOP("Auto")},
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
  monado_hz_restarted_ = false;
  monado_hz_mismatch_retried_ = false;
  {
    std::lock_guard<std::mutex> lock(menu_mu_);
    if (vr_menu_) vr_menu_->set_preferred_hz(hz);
  }
  requestHudPaint();
  appendLog(tr("Refresh: %1").arg(hz <= 0 ? tr("Auto") : tr("%1 Hz").arg(hz)));
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
  if (hand_menu_) {
    hand_menu_->setEnabled(!session_active &&
                           opt_.controller_profile != vrp::ControllerProfile::Gamepad);
  }
}

void HostWindow::onControllerProfileAction(QAction* action) {
  if (!action) return;
  opt_.controller_profile = static_cast<vrp::ControllerProfile>(action->data().toInt());
  const bool gp = opt_.controller_profile == vrp::ControllerProfile::Gamepad;
  if (hand_menu_) hand_menu_->setEnabled(!gp && connect_ui_enabled_);
  appendLog(tr("Pad: %1 (applies on reconnect)").arg(QString::fromUtf8(vrp::controller_profile_name(opt_.controller_profile))));
  if (gp) {
    const auto names = vrp::GamepadInput::list_names();
    if (names.empty()) {
      appendLog(tr("No gamepad — plug in, then connect HMD"));
    } else {
      appendLog(tr("Found: %1").arg(QString::fromStdString(names.front())));
    }
  }
}

void HostWindow::onHandAction(QAction* action) {
  if (!action) return;
  opt_.controller_hand = static_cast<vrp::ControllerHand>(action->data().toInt());
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
  // Only auto-pick HMD HDMI while connecting/connected — not after disconnect.
  const bool pick_hmd =
      prefer_hmd || (xr_running_.load() && !last_xr_system_.isEmpty() && !audio_user_override_.load());
  if (pick_hmd && !audio_user_override_.load()) {
    const std::string pick =
        vrp::AudioPlayer::prefer_hmd_device(devices, last_xr_system_.toStdString());
    if (!pick.empty()) {
      idx = audio_device_ids_.indexOf(QString::fromStdString(pick));
      if (idx >= 0) {
        audio_user_override_.store(false);
        appendLog(tr("Audio → HMD: %1").arg(audio_device_labels_[idx]));
      }
    }
  }

  // After disconnect: prefer a non-HMD sink so PSVR2 HDMI does not stick.
  if (idx < 0 && !pick_hmd) {
    for (const auto& d : devices) {
      if (d.kind == vrp::AudioRouteKind::HmdHdmi) continue;
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
        if (QString::fromStdString(d.name) == prev && d.kind == vrp::AudioRouteKind::HmdHdmi) {
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

void HostWindow::beginThumbnailBuild(const std::string& path) {
  if (path.empty()) return;
  thumb_cancel_.store(true);
  if (thumb_thread_.joinable()) thumb_thread_.detach();
  thumb_cancel_.store(false);


  thumb_thread_ = std::thread([this, path] {
    vrp::ThumbnailStrip strip = vrp::build_thumbnail_strip(path);
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
    if (!shutting_down_.load()) startOwnedWivrn(/*allow_external_adopt=*/true);
    return;
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
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
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
  QSettings s(QStringLiteral("monaSphere"), QStringLiteral("monaSphere"));
  const QString raw = s.value(QStringLiteral("monado/mode_table")).toString();
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
  QSettings s(QStringLiteral("monaSphere"), QStringLiteral("monaSphere"));
  s.setValue(QStringLiteral("monado/mode_table"), parts.join(';'));
}

bool HostWindow::ensureMonadoModeTable() {
  if (!monado_mode_hz_.isEmpty()) return true;
  loadMonadoModeTable();
  if (!monado_mode_hz_.isEmpty()) return true;

  // Probe HMD VkDisplay modes before Monado start (PRINT_MODES only appears after compositor init).
  QProcess probe;
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
      appendLog(tr("Refresh: %1").arg(preferred_hz_ <= 0 ? tr("Auto") : tr("%1 Hz").arg(preferred_hz_)));
    }
    std::lock_guard<std::mutex> lock(xr_app_mu_);
    if (xr_app_ && xr_app_->has_display_refresh_rate_ext()) {
      const float pref = preferred_hz_ <= 0 ? 0.f : static_cast<float>(preferred_hz_);
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

  const QString want_label =
      preferred_hz_ <= 0 ? tr("Auto") : tr("%1 Hz").arg(preferred_hz_);
  if (from_ui) {
    appendLog(tr("Refresh: %1").arg(want_label));
  }

  // Live OpenXR request (desktop combo / soft path). HMD [再起動] always restarts Monado.
  if (!force_monado_restart) {
    std::lock_guard<std::mutex> lock(xr_app_mu_);
    if (xr_app_) {
      const float pref = preferred_hz_ <= 0 ? 0.f : static_cast<float>(preferred_hz_);
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
          const int want = preferred_hz_;
          QTimer::singleShot(1200, this, [this, want] {
            if (shutting_down_.load() || want <= 0 || preferred_hz_ != want) return;
            double measured = 0.0;
            {
              std::lock_guard<std::mutex> lock(xr_app_mu_);
              if (xr_app_) measured = xr_app_->status().display_hz;
            }
            if (measured > 0.0 && std::fabs(measured - static_cast<double>(want)) > 5.0) {
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
  if (preferred_hz_ > 0) {
    ensureMonadoModeTable();
    const int idx = findModeIndexForHz(preferred_hz_);
    if (idx >= 0) {
      monado_desired_mode_ = idx;
      appendLog(QString("XRT_COMPOSITOR_DESIRED_MODE=%1 (≈%2 Hz)")
                    .arg(idx)
                    .arg(monado_mode_hz_.value(idx), 0, 'f', 1));
    } else {
      monado_desired_mode_ = -1;
      appendLog(tr("No mode for %1 Hz — rescan after restart").arg(preferred_hz_));
      monado_mode_hz_.clear();
      monado_mode_pixels_.clear();
      monado_hz_restarted_ = false;
    }
  } else {
    monado_desired_mode_ = -1;
    monado_hz_restarted_ = true;  // auto: no post-start mode chase
  }
  if (preferred_hz_ > 0 && monado_desired_mode_ >= 0) {
    monado_hz_restarted_ = true;  // already decided; avoid double-restart on PRINT_MODES
  }
  monado_hz_mismatch_retried_ = false;

  // End XR session first (do not stop Monado here — we restart it with new env next).
  if (xr_running_.load() || xr_thread_.joinable()) {
    user_suppressed_auto_connect_ = false;  // reconnect after Monado comes back
    if (!shutdownXrSession(8000, /*stop_monado=*/false)) {
      appendLog(tr("XR detach timeout — restart continues"));
      if (xr_thread_.joinable()) xr_thread_.detach();
      xr_running_.store(false);
    }
    syncDisconnectedUi(tr("Hz change — restarting"));
  }

  if (monado_external_ && preferred_hz_ <= 0) {
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
  if (preferred_hz_ <= 0) return;
  if (monado_mode_hz_.isEmpty()) return;
  const int idx = findModeIndexForHz(preferred_hz_);
  if (idx < 0) {
    appendLog(tr("No display mode near %1 Hz")
                  .arg(preferred_hz_));
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
  // When DESIRED_MODE must stick, never attach to a pre-existing service.
  if (monado_hz_require_owned_) allow_external_adopt = false;
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
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
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
    xr.set_preferred_display_refresh_hz(preferred_hz_ <= 0 ? 0.f : static_cast<float>(preferred_hz_));
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

    vrp::Fsr1Upscaler fsr;
    fsr.init(xr.physical_device(), xr.device(), xr.queue(), xr.queue_family(), &xr.queue_mutex(),
             shader_dir_.toStdString());
    fsr.set_mode(preferred_fsr_);

    vrp::VideoDecoder decoder;
    vrp::AudioPlayer audio;
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
    vrp::VideoFrame pending_upload;
    texture.init(xr.physical_device(), xr.device(), xr.queue(), xr.queue_family(), &xr.queue_mutex());
    const bool gpu_nv12 =
        cuda_tex.init(xr.physical_device(), xr.device(), xr.queue(), xr.queue_family(), &xr.queue_mutex(),
                      xr.external_memory_fd());
    const bool vaapi_nv12 =
        vaapi_tex.init(xr.physical_device(), xr.device(), xr.queue(), xr.queue_family(), &xr.queue_mutex(),
                       xr.dma_buf_import());
    decoder.set_cuda_texture(gpu_nv12 ? &cuda_tex : nullptr);
    decoder.set_vaapi_texture(vaapi_nv12 ? &vaapi_tex : nullptr);
    QMetaObject::invokeMethod(
        this,
        [this, gpu_nv12, vaapi_nv12] {
          if (gpu_nv12) appendLog(tr("Video: GPU NV12"));
          else if (vaapi_nv12) appendLog(tr("Video: VA-API dma-buf"));
          else appendLog(tr("Video: CPU RGBA"));
        },
        Qt::QueuedConnection);
    scene.set_texture(texture.view(), texture.sampler());

    vrp::VideoTexture hud_tex;
    hud_tex.init(xr.physical_device(), xr.device(), xr.queue(), xr.queue_family(), &xr.queue_mutex(),
                 /*nearest_filter=*/false);
    vrp::VrMenu menu;
    {
      std::filesystem::path start;
      if (!opt_.video_path.empty()) {
        start = std::filesystem::path(opt_.video_path).parent_path();
      } else {
        const char* home = std::getenv("HOME");
        const std::filesystem::path conf =
            std::filesystem::path(home ? home : "") / ".config" / "monasphere" / "conf";
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
      menu.set_format(opt_.projection, opt_.stereo);
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
    requestHudPaint();

    vrp::GamepadInput gamepad;
    const bool use_gamepad = opt_.controller_profile == vrp::ControllerProfile::Gamepad;
    std::string last_gamepad_name;
    if (use_gamepad) {
      if (gamepad.ensure_open()) {
        QMetaObject::invokeMethod(
            this, [this] { appendLog(tr("Input: gamepad (A OK, B back, Start menu)")); },
            Qt::QueuedConnection);
      } else {
        QMetaObject::invokeMethod(
            this,
            [this] {
              appendLog(tr("Gamepad open failed. Recenter only"));
            },
            Qt::QueuedConnection);
      }
    }

    std::atomic<bool> preview_busy{false};
    std::string preview_path_done;
    auto request_preview = [&](const std::string& path) {
      if (path.empty()) {
        if (!preview_path_done.empty() || preview_busy.load()) {
          preview_path_done.clear();
          menu.clear_preview();
          requestHudPaint();
        }
        return;
      }
      if (path == preview_path_done || preview_busy.load()) return;
      preview_busy.store(true);
      std::thread([&, path] {
        std::vector<uint8_t> rgba;
        const bool ok = vrp::extract_video_preview(path, 288, 162, rgba);
        if (ok && !xr_stop_.load() && !shutting_down_.load()) {
          // Drop if selection moved away while decoding the thumb.
          if (menu.highlighted_video_path() == path) {
            menu.set_preview(std::move(rgba), 288, 162);
            preview_path_done = path;
            requestHudPaint();
          }
        }
        preview_busy.store(false);
      }).detach();
    };

    bool vaapi_hud_pending = true;
    auto open_media = [&](const std::string& path) {
      vaapi_hud_pending = true;
      // Keep miniaudio alive and muted so silence keeps flushing HMD/Pulse during the switch.
      // Closing the device mid-switch is what left the previous file's audio in the sink.
      if (audio.is_open()) audio.mute_output();
      if (decoder.is_open()) decoder.pause();

      // Stop video decode but keep NVDEC CUDA context alive until after slot teardown.
      // Do NOT audio.close() here — device stays up pumping silence.
      decoder.halt_decode();
      decoder.stop_thumbnail_build();
      pending_upload = {};
      cuda_tex.drain_copies();

      // Bound GPU wait — full vkQueueWaitIdle stalls the HMD on high-bitrate switches.
      (void)scene.wait_previous_submit(50'000'000ull);  // 50ms
      // Unbind NV12 descriptors first, then free Vulkan/CUDA memory under the import context.
      scene.set_texture(texture.view(), texture.sampler());
      cuda_tex.reset_slots(decoder.cuda_context());
      vaapi_tex.reset();
      decoder.close();

      const bool ok = decoder.open(path);
      const vrp::VideoInfo info = decoder.info();
      QMetaObject::invokeMethod(
          this, [this, info] { reportVideoOpen(info); }, Qt::QueuedConnection);
      if (ok) {
        vrp::detect_format_from_filename(path, opt_.projection, opt_.stereo);
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
        menu.set_format(opt_.projection, opt_.stereo);
        audio.set_volume(menu.volume());
        decoder.set_rate(1.f);
        audio.set_rate(1.f);
        menu.set_rate(1.f);
        {
          float aspect = (info.height > 0) ? static_cast<float>(info.width) / static_cast<float>(info.height)
                                           : (16.f / 9.f);
          if (opt_.stereo == vrp::StereoLayout::Sbs) aspect *= 0.5f;
          else if (opt_.stereo == vrp::StereoLayout::OverUnder) aspect *= 2.f;
          scene.set_content_aspect(aspect);
        }
        QMetaObject::invokeMethod(
            this,
            [this] {
              appendLog(tr("Format guess: %1 / %2")
                            .arg(QString::fromUtf8(vrp::projection_name(opt_.projection)))
                            .arg(QString::fromUtf8(vrp::stereo_name(opt_.stereo))));
            },
            Qt::QueuedConnection);
        requestHudPaint();
        // Host-side strip only — do not block XR on high-bitrate thumbnail decode.
        QMetaObject::invokeMethod(
            this, [this, path] { beginThumbnailBuild(path); }, Qt::QueuedConnection);
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
      return ok;
    };

    if (!opt_.video_path.empty() && !opt_.clear_only) {
      if (!open_media(opt_.video_path)) {
        QMetaObject::invokeMethod(
            this,
            [this] { appendLog(tr("Startup video failed. Pick another file")); },
            Qt::QueuedConnection);
      } else {
        decoder.play();
        // Audio starts when consume_playback_started fires (prefetch full).
      }
    }

    auto last = std::chrono::steady_clock::now();
    std::vector<XrCompositionLayerProjectionView> proj_views;
    XrCompositionLayerProjection layer{};
    auto last_status = std::chrono::steady_clock::now();
    bool force_fsr_refresh = true;

    while (!xr_stop_.load() && xr.pump_events()) {
      auto now = std::chrono::steady_clock::now();
      double dt = std::chrono::duration<double>(now - last).count();
      last = now;
      if (dt > 0.1) dt = 0.1;

      const int cmd = cmd_.exchange(0);
      if (cmd == 1) {
        // Keep A/V in lockstep — independent toggles desync after audio reopen.
        const bool active = decoder.is_playing() || decoder.is_priming();
        if (!active) {
          audio.seek_to(decoder.position());
          decoder.play();
        } else {
          decoder.pause();
          audio.pause();
        }
      }
      if (cmd == 2) {
        decoder.seek_relative(-10.0);
        if (audio.is_open()) audio.begin_seek_prefill(decoder.position());
      }
      if (cmd == 3) {
        decoder.seek_relative(10.0);
        if (audio.is_open()) audio.begin_seek_prefill(decoder.position());
      }
      if (cmd == 4) {
        xr.recenter();
      }

      const int p = proj_req_.exchange(-1);
      const int s = stereo_req_.exchange(-1);
      if (p >= 0 || s >= 0) {
        if (p >= 0) opt_.projection = static_cast<vrp::ProjectionMode>(p);
        if (s >= 0) opt_.stereo = static_cast<vrp::StereoLayout>(s);
        scene.set_projection(opt_.projection, opt_.stereo, opt_.flat_fov_deg, opt_.screen_distance);
      }

      const int fsr_pick = fsr_req_.exchange(-1);
      if (fsr_pick >= 0) {
        const auto mode = static_cast<vrp::FsrMode>(fsr_pick);
        fsr.set_mode(mode);
        menu.set_fsr(mode);
        force_fsr_refresh = true;
        requestHudPaint();
      }

      if (video_pending_.exchange(false)) {
        std::string path;
        {
          std::lock_guard<std::mutex> lock(video_mu_);
          path = pending_video_;
        }
        if (!path.empty()) {
          if (!open_media(path)) {
            QMetaObject::invokeMethod(
                this,
                [this] { appendLog(tr("Cannot play video this session")); },
                Qt::QueuedConnection);
          } else {
            decoder.play();
          }
        }
      }

      auto input = xr.poll_actions();
      if (use_gamepad) {
        const auto gp = gamepad.poll();
        if (gp.present) {
          last_gamepad_name = gp.name;
          input.stick_x = gp.stick_x;
          input.stick_y = gp.stick_y;
          input.confirm = input.confirm || gp.confirm;
          input.confirm_held = input.confirm_held || gp.confirm_held;
          input.back = input.back || gp.back;
          input.back_held = input.back_held || gp.back_held;
          input.menu_toggle = input.menu_toggle || gp.menu_toggle;
          input.seek_back = input.seek_back || gp.seek_back;
          input.seek_forward = input.seek_forward || gp.seek_forward;
          input.seek_scrubbing = input.seek_scrubbing || gp.seek_scrubbing;
          input.recenter = input.recenter || gp.recenter;
        } else {
          last_gamepad_name.clear();
        }
      }
      decoder.set_seek_scrubbing(input.seek_scrubbing);

      vrp::VrMenu::PadInput pad;
      pad.stick_x = input.stick_x;
      pad.stick_y = input.stick_y;
      pad.confirm = input.confirm;
      pad.confirm_held = input.confirm_held;
      pad.back = input.back;
      pad.back_held = input.back_held;
      pad.menu_toggle = input.menu_toggle;
      const bool menu_was = menu.visible();
      const bool controls_was = menu.controls_visible();
      const auto menu_out = menu.update(pad, static_cast<float>(dt));
      if (menu_out.seek_scrubbing) decoder.set_seek_scrubbing(true);
      if (menu.visible() != menu_was || menu.controls_visible() != controls_was) requestHudPaint();
      if (menu_out.open_video) {
        if (!open_media(menu_out.video_path)) {
          QMetaObject::invokeMethod(
              this, [this] { appendLog(tr("HMD: cannot open video")); }, Qt::QueuedConnection);
        } else {
          decoder.play();
        }
        requestHudPaint();
      } else if (menu_out.play_toggle) {
        // Do not also handle raw input.confirm this frame — open_video closes the menu while
        // confirm is still true and would immediately pause.
        if (decoder.is_playing() || decoder.is_priming()) {
          decoder.pause();
          audio.pause();
        } else {
          // Align audio demuxer before priming; device already runs silence while muted.
          if (audio.is_open()) audio.seek_to(decoder.position());
          decoder.play();
        }
      }
      if (menu_out.apply_format) {
        opt_.projection = menu_out.projection;
        opt_.stereo = menu_out.stereo;
        scene.set_projection(opt_.projection, opt_.stereo, opt_.flat_fov_deg, opt_.screen_distance);
        menu.set_format(opt_.projection, opt_.stereo);
        if (decoder.is_open()) {
          const auto& info = decoder.info();
          float aspect = (info.height > 0) ? static_cast<float>(info.width) / static_cast<float>(info.height)
                                           : (16.f / 9.f);
          if (opt_.stereo == vrp::StereoLayout::Sbs) aspect *= 0.5f;
          else if (opt_.stereo == vrp::StereoLayout::OverUnder) aspect *= 2.f;
          scene.set_content_aspect(aspect);
        }
        QMetaObject::invokeMethod(
            this,
            [this] {
              appendLog(tr("HMD format: %1 / %2")
                            .arg(QString::fromUtf8(vrp::projection_name(opt_.projection)))
                            .arg(QString::fromUtf8(vrp::stereo_name(opt_.stereo))));
            },
            Qt::QueuedConnection);
        requestHudPaint();
      }
      if (menu_out.play) {
        audio.seek_to(decoder.position());
        decoder.play();
      }
      if (menu_out.pause) {
        decoder.pause();
        audio.pause();
      }
      if (menu_out.stop) {
        decoder.pause();
        decoder.seek_to(0.0);
        // Must seek audio demuxer to 0 as well — pause alone leaves decode at the stop point,
        // so the next play() can briefly emit that PCM (and Pulse may still hold it).
        if (audio.is_open()) audio.stop();
      }
      if (menu_out.rate_changed) {
        decoder.set_rate(menu_out.rate);
        audio.set_rate(menu_out.rate);
        menu.set_rate(menu_out.rate);
        // Resync audio demuxer to video timeline — rate alone leaves decode ahead of playhead.
        if (audio.is_open()) {
          const bool keep = decoder.is_playing() || decoder.is_priming();
          audio.seek_to(decoder.position());
          if (keep) audio.play();
        }
        requestHudPaint();
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
        requestHudPaint();
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
              rebuildHzMenu();
              applyPreferredRefreshRate(/*from_ui=*/true, /*force_monado_restart=*/true);
            },
            Qt::QueuedConnection);
        requestHudPaint();
      }
      if (std::fabs(menu_out.volume_delta) > 0.f) {
        audio.set_volume(audio.volume() + menu_out.volume_delta);
        menu.set_volume(audio.volume());
        requestHudPaint();
      }
      // LB / RB / [<][>] seek — mute + start audio demux prefill while video seeks.
      if (input.seek_back || menu_out.seek_back) {
        decoder.seek_relative(-10.0);
        if (audio.is_open()) audio.begin_seek_prefill(decoder.position());
      }
      if (input.seek_forward || menu_out.seek_forward) {
        decoder.seek_relative(10.0);
        if (audio.is_open()) audio.begin_seek_prefill(decoder.position());
      }
      if (menu_out.seek_start) {
        decoder.seek_to(0.0);
        if (audio.is_open()) audio.begin_seek_prefill(decoder.position());
      }

      // Video caught up after seek → align audio to the decoded frame PTS, then unmute.
      if (decoder.consume_av_resync() && audio.is_open()) {
        audio.enable_after_seek(decoder.position());
      }

      // Prefetch filled after play() → start audio with video.
      if (decoder.consume_playback_started() && audio.is_open()) {
        audio.seek_to(decoder.position());
        audio.play();
      }

      if (input.recenter) {
        xr.recenter();
      }

      menu.set_playback(decoder.is_playing() || decoder.is_priming(), decoder.position());
      menu.set_volume(audio.volume());
      menu.set_preferred_hz(preferred_hz_);
      {
        const auto st_hz = xr.status();
        if (st_hz.display_hz > 0.0) menu.set_display_hz(static_cast<float>(st_hz.display_hz));
      }
      // Video end-of-stream loop → keep audio on the same timeline (avoids post-repeat silence).
      if (decoder.consume_looped() && audio.is_open() && decoder.is_playing()) {
        audio.seek_to(0.0);
        if (!audio.is_playing()) audio.play();
      }
      if (menu.visible() || menu.controls_visible()) {
        request_preview(menu.highlighted_video_path());
        static auto last_hud_paint = std::chrono::steady_clock::time_point{};
        if (now - last_hud_paint > std::chrono::milliseconds(100)) {
          last_hud_paint = now;
          requestHudPaint();
        }
      }

      if (audio_reopen_.exchange(false) && !opt_.mute) {
        const bool keep_playing = decoder.is_playing() || decoder.is_priming();
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
          audio.seek_to(decoder.position());
          // Only start audio once video is actually playing (not still priming).
          if (keep_playing && decoder.is_playing()) audio.play();
          else audio.pause();
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
      xr.maybe_init_tracking_origin();

      // Only wait when rebinding — avoid stalling every XR frame.
      if (!opt_.clear_only && decoder.is_open()) {
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
            (void)scene.wait_previous_submit(10'000'000ull);
            video_updated = true;
          }
          if ((video_updated || force_fsr_refresh) && !fsr.enabled() && src_y && src_samp) {
            scene.set_nv12_texture(src_y, src_uv, src_samp, cuda_tex.full_range());
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
            (void)scene.wait_previous_submit(10'000'000ull);
            video_updated = true;
          }
          if ((video_updated || force_fsr_refresh) && !fsr.enabled() && src_y && src_samp) {
            scene.set_nv12_texture(src_y, src_uv, src_samp, vaapi_tex.full_range());
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
          }
          if (texture.poll_present()) {
            video_updated = true;
          }
          src_y = texture.view();
          src_uv = texture.view();
          src_samp = texture.sampler();
          src_w = texture.width();
          src_h = texture.height();
          if ((video_updated || force_fsr_refresh) && !fsr.enabled() && src_y && src_samp) {
            scene.set_texture(src_y, src_samp);
          }
        }

        if (fsr.enabled() && (video_updated || force_fsr_refresh) && src_y && src_samp && src_w > 0 &&
            src_h > 0) {
          if (fsr.process(src_y, src_uv, src_samp, src_w, src_h, nv12_mode)) {
            (void)scene.wait_previous_submit(10'000'000ull);
            scene.set_texture_gamma(fsr.output_view(), fsr.output_sampler());
          }
        }
        force_fsr_refresh = false;
      }

      // HMD menu panel texture
      {
        static uint64_t uploaded_gen = 0;
        vrp::VideoFrame hud_frame;
        bool have = false;
        {
          std::lock_guard<std::mutex> lock(hud_pixels_mu_);
          if (hud_pixels_gen_ != uploaded_gen && !hud_pixels_.empty()) {
            hud_frame.width = hud_w_;
            hud_frame.height = hud_h_;
            hud_frame.rgba = hud_pixels_;
            uploaded_gen = hud_pixels_gen_;
            have = true;
          } else if (hud_pixels_.empty() && !menu.visible() && !menu.controls_visible()) {
            scene.set_hud_texture(VK_NULL_HANDLE, VK_NULL_HANDLE, false);
          }
        }
        if (have) {
          (void)scene.wait_previous_submit(5'000'000ull);
          if (hud_tex.try_upload(hud_frame)) {
            /* wait for present */
          }
        }
        const bool hud_on = menu.visible() || menu.controls_visible();
        // HMD UI distance is fixed; only panel size/aspect differ by mode.
        constexpr float kHudDistanceM = 1.5f;
        if (menu.visible()) {
          // Narrower + taller panel for file manager.
          scene.set_hud_layout(0.48f, kHudDistanceM, 1.05f);
        } else if (menu.controls_visible()) {
          scene.set_hud_layout(0.72f, kHudDistanceM, 720.f / 1280.f);
        }
        if (hud_tex.poll_present()) {
          scene.set_hud_texture(hud_tex.view(), hud_tex.sampler(), hud_on);
        } else {
          scene.set_hud_texture(hud_tex.view(), hud_tex.sampler(), hud_on && hud_tex.width() > 0);
        }
      }

      scene.render_frame(xr, fi, proj_views, layer);
      std::vector<XrCompositionLayerBaseHeader*> layers = {
          reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer)};
      xr.end_frame(fi, layers);
      const double frame_ms =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frame_t0).count();
      xr.note_frame_ms(frame_ms);

      if (now - last_status > std::chrono::milliseconds(500)) {
        last_status = now;
        auto cur = xr.status();
        if (!logged_session_refresh_hz_ && cur.display_hz > 0.0) {
          logged_session_refresh_hz_ = true;
          const double hz = cur.display_hz;
          const int want = preferred_hz_;
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
    VRP_LOG("teardown: reset NV12 slots (before decoder.close)");
    cuda_tex.reset_slots(decoder.cuda_context());
    decoder.close();
    VRP_LOG("teardown: decoder closed");
    fsr.shutdown();
    cuda_tex.shutdown();
    vaapi_tex.shutdown();
    hud_tex.shutdown();
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
