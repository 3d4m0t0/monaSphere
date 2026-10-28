#include "youtube_session.hpp"

#include "audio/pulse_monitor.hpp"
#include "capture/kwin_window_placer.hpp"
#include "common.hpp"

#include <QCoreApplication>
#include <QCursor>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIODevice>
#include <QScreen>
#include <QThread>
#include <QWidget>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <thread>
#include <unistd.h>

namespace vrp {

YouTubeSession::YouTubeSession(QObject* parent) : QObject(parent) {
  desk_ = detect_desktop_env();
  chrome_ = new ChromiumYtLauncher(this);
  desktop_app_ = new QProcess(this);
  desktop_app_->setProcessChannelMode(QProcess::MergedChannels);
  connect(desktop_app_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
          &YouTubeSession::onDesktopAppFinished);
  portal_ = new ScreencastPortal(this);
  kde_eis_ = new KdeEisPointer();
  portal_eis_ = new EisPointer();
#if defined(VRP_HAS_PIPEWIRE)
  capture_ = std::make_unique<PipewireCapture>();
#endif
  place_timer_.setSingleShot(true);
  connect(&place_timer_, &QTimer::timeout, this, &YouTubeSession::pollPlace);
  desk_maintain_timer_.setSingleShot(false);
  desk_maintain_timer_.setInterval(400);
  // Timer kept but unused (sticky KWin arm replaces maintain poll).
  connect(&desk_maintain_timer_, &QTimer::timeout, this, &YouTubeSession::maintainDesktopAppWindows);

  connect(portal_, &ScreencastPortal::started, this, &YouTubeSession::onPortalStarted);
  connect(portal_, &ScreencastPortal::failed, this, &YouTubeSession::onPortalFailed);
  connect(portal_, &ScreencastPortal::stopped, this, [this] {
    // Start Response≠0 closes the portal session while we may still retry restore.
    // Only treat Closed as fatal once we were actually streaming.
    if (state_ == State::Portal || loading_.load(std::memory_order_acquire)) return;
    if (state_ == State::Streaming || active_.load(std::memory_order_acquire)) {
      emit logMessage(tr("ScreenCast stopped on host — ending live capture"));
      emit streamLost();
    }
  });
  connect(portal_, &ScreencastPortal::status, this, [this](const QString& m) { emit logMessage(m); });
}

YouTubeSession::~YouTubeSession() {
  // Do not touch Host-owned VirtualDisplayManager — it may already be mid-teardown.
  vdisp_mgr_ = nullptr;
  shutdown();
  delete kde_eis_;
  kde_eis_ = nullptr;
  delete portal_eis_;
  portal_eis_ = nullptr;
}

EisPointer* YouTubeSession::activeEis() {
  if (portal_eis_ && portal_eis_->ready()) return portal_eis_;
  if (kde_eis_ && kde_eis_->ready()) return &kde_eis_->core();
  return nullptr;
}

void YouTubeSession::rememberSeatCursor() {
  if (have_saved_cursor_) return;  // keep first snapshot (before Virtual warp)
  double kx = 0, ky = 0;
  QString err;
  if (desk_ == DesktopEnv::Kde && KwinWindowPlacer::query_cursor_pos(&kx, &ky, &err)) {
    saved_cursor_x_ = kx;
    saved_cursor_y_ = ky;
    have_saved_cursor_ = true;
    VRP_LOG("live: saved KWin seat cursor at %.0f,%.0f", saved_cursor_x_, saved_cursor_y_);
    return;
  }
  const QPoint p = QCursor::pos();
  saved_cursor_x_ = p.x();
  saved_cursor_y_ = p.y();
  have_saved_cursor_ = true;
  VRP_LOG("live: saved QCursor at %.0f,%.0f (%s)", saved_cursor_x_, saved_cursor_y_,
          err.isEmpty() ? "no KWin" : err.toUtf8().constData());
}

void YouTubeSession::restoreSeatCursor() {
  if (!have_saved_cursor_) return;
  have_saved_cursor_ = false;

  double x = saved_cursor_x_;
  double y = saved_cursor_y_;
  // Never leave the seat on the live MONITOR — QCursor often lies on Wayland
  // and can look like Virtual-local coords.
  const QRect virt(place_x_, place_y_, std::max(1, place_w_), std::max(1, place_h_));
  auto on_virt = [&](double px, double py) {
    return virt.adjusted(-16, -16, 16, 16).contains(QPoint(static_cast<int>(px + 0.5),
                                                           static_cast<int>(py + 0.5)));
  };
  if (on_virt(x, y)) {
    if (QScreen* primary = QGuiApplication::primaryScreen()) {
      const QPoint c = primary->availableGeometry().center();
      x = c.x();
      y = c.y();
    } else {
      x = 200;
      y = 200;
    }
  }
  // Prefer primary output center if still somehow on Virtual (mis-resolved geo).
  if (on_virt(x, y)) {
    x = 200;
    y = 200;
  }

  if (EisPointer* eis = activeEis()) {
    // Temporarily ignore Virtual-only region_local: send desktop-global.
    eis->warp_desktop(x, y);
    emit logMessage(tr("Pointer: restored seat cursor to %1,%2 (was %3,%4)")
                        .arg(static_cast<int>(x))
                        .arg(static_cast<int>(y))
                        .arg(static_cast<int>(saved_cursor_x_))
                        .arg(static_cast<int>(saved_cursor_y_)));
    return;
  }
  QCursor::setPos(static_cast<int>(x + 0.5), static_cast<int>(y + 0.5));
}

void YouTubeSession::stopEis() {
  restoreSeatCursor();
  x11_.set_eis_inject(nullptr);
  if (kde_eis_) kde_eis_->stop();
  if (portal_eis_) portal_eis_->stop();
}

void YouTubeSession::setState(State s) {
  if (state_ == s) return;
  state_ = s;
  emit stateChanged(s);
}

void YouTubeSession::fail(const QString& err) {
  place_timer_.stop();
  ++portal_gen_;
  KwinWindowPlacer::disarm_place_on_open();
  KwinWindowPlacer::disarm_portal_dialog_on_primary();
  cleanupStreaming();
  // Leave Virtual up — Host destroy() when leaving live; next ensure() may reuse.
  loading_.store(false, std::memory_order_release);
  active_.store(false, std::memory_order_release);
  audio_wanted_.store(false, std::memory_order_release);
  clear_busy_status();
  setState(State::Failed);
  emit failed(err);
  emit logMessage(err);
  setState(State::Idle);
}

bool YouTubeSession::virtual_display_alive() const {
  return vdisp_mgr_ && vdisp_mgr_->alive();
}

void YouTubeSession::stopDesktopAppProcess() {
  if (!desktop_app_) return;
  if (desktop_app_->state() == QProcess::NotRunning) return;
  desktop_app_stopping_ = true;
  desktop_app_->terminate();
  if (!desktop_app_->waitForFinished(2000)) {
    desktop_app_->kill();
    desktop_app_->waitForFinished(1000);
  }
  desktop_app_stopping_ = false;
}

void YouTubeSession::set_busy_status(const QString& status) {
  std::lock_guard<std::mutex> lock(busy_mu_);
  busy_status_ = status;
}

void YouTubeSession::clear_busy_status() {
  std::lock_guard<std::mutex> lock(busy_mu_);
  busy_status_.clear();
}

QString YouTubeSession::busy_status() const {
  std::lock_guard<std::mutex> lock(busy_mu_);
  return busy_status_;
}

void YouTubeSession::onDesktopAppFinished(int exit_code, QProcess::ExitStatus status) {
  // Intentional stop() clears the flag after waitForFinished; the finished signal may
  // arrive on the next event-loop turn — ignore exit during/after our own teardown.
  if (desktop_app_stopping_) return;
  if (state_ == State::Idle || state_ == State::Stopping) return;
  const QString label = desktop_label_.isEmpty() ? tr("アプリ") : desktop_label_;
  // Keep 「終了中」 until Host XR uploads idle and calls stop() — avoids leftover spinner.
  set_busy_status(tr("%1 終了中").arg(label));
  loading_.store(false, std::memory_order_release);
  emit logMessage(tr("Desktop app exited (%1, code %2) — ending live capture")
                      .arg(status == QProcess::CrashExit ? tr("crash") : tr("normal"))
                      .arg(exit_code));
  emit desktopAppExited();
}

void YouTubeSession::stopDesktopAppMaintain() {
  desk_maintain_timer_.stop();
  if (desktop_rule_installed_) {
    KwinWindowPlacer::remove_desktop_window_rule();
    desktop_rule_installed_ = false;
  }
  desktop_wmclass_.clear();
}

void YouTubeSession::startDesktopAppMaintain() {
  // No Qt-side polling. Sticky KWin arm (windowAdded) + Window Rule cover
  // RetroArch content recreates without loadScript/unload spam (plasmashell freeze).
  if (!desktopAppMode()) return;
  emit logMessage(tr("Desktop app: sticky place + Window Rule on %1 (no maintain poll)")
                      .arg(output_name_));
}

void YouTubeSession::installDesktopAppSessionRule() {
  if (!desktopAppMode() || kwin_window_id_.isEmpty()) return;
  QString rc, rn, err;
  if (!KwinWindowPlacer::query_window_wmclass(kwin_window_id_, &rc, &rn, &err)) {
    emit logMessage(tr("Desktop app: wmclass query failed (%1) — sticky arm only")
                        .arg(err.isEmpty() ? tr("unknown") : err));
    return;
  }
  // KWin complete Exact: "resourceName resourceClass" (e.g. "retroarch retroarch").
  const QString name = rn.isEmpty() ? rc : rn;
  const QString cls = rc.isEmpty() ? rn : rc;
  if (name.isEmpty() && cls.isEmpty()) return;
  const QString complete =
      (name.isEmpty() || cls.isEmpty() || name.compare(cls, Qt::CaseInsensitive) == 0)
          ? (cls.isEmpty() ? name : cls)
          : (name + QLatin1Char(' ') + cls);
  if (desktop_rule_installed_ &&
      desktop_wmclass_.compare(complete, Qt::CaseInsensitive) == 0) {
    return;
  }
  desktop_wmclass_ = complete;
  if (vdisp_mgr_) (void)vdisp_mgr_->refresh_identity();
  QString rule_err;
  const int scr = vdisp_mgr_ ? vdisp_mgr_->screen_index() : -1;
  const bool ok = (scr >= 0)
                      ? KwinWindowPlacer::install_desktop_window_rule(complete, scr, &rule_err)
                      : KwinWindowPlacer::install_desktop_window_rule(complete, output_name_,
                                                                      &rule_err);
  if (ok) {
    desktop_rule_installed_ = true;
    emit logMessage(tr("Desktop app: Window Rule after map complete=%1 → %2 (#%3) Exact")
                        .arg(complete, output_name_)
                        .arg(scr));
  } else {
    emit logMessage(tr("Desktop app: Window Rule install failed (%1)")
                        .arg(rule_err.isEmpty() ? tr("unknown") : rule_err));
  }
}

void YouTubeSession::maintainDesktopAppWindows() {
  // Intentionally empty — former 400ms one-shot KWin scripts froze Plasma.
}

void YouTubeSession::cleanupStreaming() {
  stopDesktopAppMaintain();
#if defined(VRP_HAS_PIPEWIRE)
  if (capture_) capture_->stop();
#endif
  stopEis();
  if (portal_) portal_->stop();
  KwinWindowPlacer::disarm_place_on_open();
  KwinWindowPlacer::disarm_portal_dialog_on_primary();
  {
    QString err;
    bool closed = false;
    if (desk_ == DesktopEnv::Kde) {
      if (!kwin_window_id_.isEmpty()) {
        closed = KwinWindowPlacer::close_window_by_id(kwin_window_id_, &err);
      }
      if (!desktopAppMode()) {
        if (!closed) closed = KwinWindowPlacer::close_youtube_window(&err);
        // Place never got an id (class often google-chrome under default-profile
        // singleton) — close windows that appeared after our pre-launch snapshot.
        if (!closed && kwin_window_id_.isEmpty()) {
          closed = KwinWindowPlacer::close_chrome_except(chrome_ids_before_, &err);
        }
      }
    }
    // After compositor close the X11 xid is often already invalid. Safe close
    // traps BadWindow; never leave a stale id for a later inject/close.
    if (!closed && x11_.have_window()) closed = x11_.close_window();
    x11_.clear();
    emit logMessage(closed ? tr("Live window closed")
                           : tr("Live window close: %1")
                                 .arg(err.isEmpty() ? tr("not found") : err));
  }
  kwin_window_id_.clear();
  chrome_ids_before_.clear();
  if (desktopAppMode()) {
    stopDesktopAppProcess();
  } else if (chrome_) {
    chrome_->stop();
  }
#if defined(VRP_HAS_PULSE)
  // Always scrub monaSphereYt — even when Flat live audio was never started
  // (older builds set PULSE_SINK unconditionally and left the null sink around).
  std::thread([] { (void)PulseMonitor::restore_browser_routing(); }).detach();
#endif
}

void YouTubeSession::mark_frame_presented() {
  if (loading_.load(std::memory_order_acquire)) {
    loading_.store(false, std::memory_order_release);
  }
  clear_busy_status();
}

void YouTubeSession::start(const Config& cfg) {
#if !defined(VRP_HAS_PIPEWIRE)
  fail(tr("Live capture requires PipeWire"));
  return;
#else
  if (state_ != State::Idle && state_ != State::Failed) {
    stop();
  }
  cfg_ = cfg;
  desktop_label_ = cfg.desktop_label;
  // Token comes from VirtualDisplayManager after ensure(); cfg.restore_token is fallback.
  restore_token_ = cfg.restore_token;
  if (!cfg.virtual_name.isEmpty()) {
    output_name_ = QStringLiteral("Virtual-") + cfg.virtual_name;
  } else if (!cfg.output_hint.isEmpty()) {
    output_name_ = cfg.output_hint;
  } else {
    output_name_ = KdeVirtualDisplay::outputNameForSpan(1);
  }
  portal_restore_retries_ = 0;
  portal_retry_used_ = false;
  place_tries_ = 0;
  kwin_window_id_.clear();
  frame_seq_ = 0;
  dma_seq_ = 0;
  dma_logged_ = false;
  dma_disabled_ = false;
  audio_wanted_.store(false, std::memory_order_release);
  active_.store(false, std::memory_order_release);
  loading_.store(true, std::memory_order_release);
  {
    const QString label = desktop_label_.isEmpty()
                              ? (desktopAppMode() ? tr("アプリ") : tr("Live"))
                              : desktop_label_;
    set_busy_status(tr("%1 起動中").arg(label));
  }

  if (capture_) capture_->stop();
  have_saved_cursor_ = false;  // new session — do not restore mid-restart
  stopEis();
  if (portal_) portal_->stop();
  if (chrome_) chrome_->stop();
  stopDesktopAppProcess();
  stopDesktopAppMaintain();
  desktop_rule_installed_ = false;
  desktop_wmclass_.clear();

  desk_ = detect_desktop_env();
  emit logMessage(tr("Live desktop: %1")
                      .arg(QString::fromLatin1(desktop_env_name(desk_))));

  if (cfg_.virtual_display) {
    if (desk_ == DesktopEnv::Gnome) {
      fail(tr("Managed virtual display (live_virtual=1) is KDE-only. "
              "On GNOME use a spare MONITOR or set live_capture=window / live_virtual=0."));
      return;
    }
    if (!vdisp_mgr_) {
      fail(tr("Live setup aborted (no VirtualDisplayManager)"));
      return;
    }
    VirtualDisplayManager::Spec spec;
    spec.name = cfg_.virtual_name.isEmpty() ? KdeVirtualDisplay::nameForSpan(1) : cfg_.virtual_name;
    spec.width = cfg_.virtual_w;
    spec.height = cfg_.virtual_h;
    spec.scale = (cfg_.virtual_scale >= 1.f && cfg_.virtual_scale <= 3.f) ? cfg_.virtual_scale : 2.f;
    spec.span = VirtualDisplayManager::spanFromName(spec.name);
    const auto vr = vdisp_mgr_->ensure(spec);
    if (!vr.ok) {
      fail(tr("Live setup aborted (virtual display): %1")
               .arg(vr.error.isEmpty() ? tr("unknown") : vr.error));
      return;
    }
    // Manager is source of truth for Virtual name + KWin screen index.
    if (!vr.output_name.isEmpty()) output_name_ = vr.output_name;
    else if (!vdisp_mgr_->output_name().isEmpty()) output_name_ = vdisp_mgr_->output_name();
    if (vr.screen_index < 0) {
      emit logMessage(tr("Virtual: warning — KWin screen index unknown; place may miss Virtual"));
    }
    // Host sets cfg.restore_token from manager (empty for interactive setup).
    restore_token_ = cfg_.restore_token;
    if (restore_token_.isEmpty() && !vr.restore_token.isEmpty()) {
      emit logMessage(tr("Virtual: x%1 has token but session requested interactive (setup)")
                          .arg(vr.span));
    } else if (!restore_token_.isEmpty()) {
      emit logMessage(tr("ScreenCast: using x%1 restore token (%2 chars)")
                          .arg(vr.span)
                          .arg(restore_token_.size()));
    }
    setState(State::VDispReady);
    {
      const QString label = desktop_label_.isEmpty()
                                ? (desktopAppMode() ? tr("アプリ") : tr("Live"))
                                : desktop_label_;
      set_busy_status(tr("%1 起動中").arg(label));
    }
  }

  QRect geo;
  QString matched;
  if (!resolveOutputRect(&geo, &matched) || geo.isEmpty()) {
    fail(tr("No live MONITOR found (output=%1)")
             .arg(output_name_.isEmpty() ? tr("(empty)") : output_name_));
    return;
  }
  if (!matched.isEmpty()) output_name_ = matched;
  // Keep manager identity in sync with kscreen-matched name (index refresh).
  if (vdisp_mgr_ && cfg_.virtual_display) {
    (void)vdisp_mgr_->refresh_identity();
    if (!vdisp_mgr_->output_name().isEmpty()) output_name_ = vdisp_mgr_->output_name();
  }

  place_x_ = geo.x();
  place_y_ = geo.y();
  place_w_ = cfg_.virtual_w > 0 ? cfg_.virtual_w : (geo.width() > 0 ? geo.width() : 1280);
  place_h_ = cfg_.virtual_h > 0 ? cfg_.virtual_h : (geo.height() > 0 ? geo.height() : 720);

  // Flat live audio deferred: scrub any orphan monaSphereYt null sink so Plasma
  // does not keep an “unknown microphone” output and mute the real HDMI sink.
  if (!cfg_.enable_audio) {
#if defined(VRP_HAS_PULSE)
    std::thread([] { (void)PulseMonitor::restore_browser_routing(); }).detach();
#endif
  }
  // XTest must use Xrandr/X11 geometry (not kscreen logical) when scale≠1.
  if (!x11_.bind_output_by_name(output_name_.toStdString(), place_w_, place_h_)) {
    x11_.set_screen_rect(place_x_, place_y_, place_w_, place_h_);
  }

  if (desktopAppMode()) {
    if (desk_ != DesktopEnv::Kde) {
      fail(tr("Desktop apps ([アプリ]) require KDE Plasma for window placement"));
      return;
    }
    emit logMessage(tr("Desktop app → %1 (%2×%3): %4")
                        .arg(output_name_)
                        .arg(place_w_)
                        .arg(place_h_)
                        .arg(cfg_.exec));
    beginPlaceDesktopApp();
    return;
  }

  chrome_->set_url(cfg_.url);
  // Wayland Chrome follows KWin output scale (live_virtual_scale) — do NOT also
  // push virtual_scale into --force-device-scale-factor (that became ~400% at scale=2).
  // X11 ozone cannot see output scale; only then mirror virtual_scale when
  // live_chrome_scale is left at 1.
  const bool ozone_x11 = !qEnvironmentVariableIsEmpty("MONASPHERE_YT_X11");
  float chrome_dsf = cfg_.chrome_scale;
  if (ozone_x11 && cfg_.virtual_display && cfg_.virtual_scale > 1.01f &&
      std::abs(cfg_.chrome_scale - 1.f) < 0.05f) {
    chrome_dsf = cfg_.virtual_scale;
  }
  chrome_->set_device_scale_factor(chrome_dsf);
  // Omit --force-device-scale-factor on Wayland so Chromium uses the output scale.
  const bool follow_out = !ozone_x11 && std::abs(cfg_.chrome_scale - 1.f) < 0.05f;
  chrome_->set_follow_output_scale(follow_out);
  chrome_->set_use_default_profile(cfg_.use_default_chrome_profile);
  chrome_->set_route_audio_to_yt_sink(cfg_.enable_audio);
  // Wayland --window-size is in DIPs (logical). Physical 3840 at scale 2 → 1920.
  int chrome_w = place_w_;
  int chrome_h = place_h_;
  if (follow_out && cfg_.virtual_scale > 1.01f) {
    chrome_w = std::max(320, static_cast<int>(std::lround(
                                  static_cast<double>(place_w_) / static_cast<double>(cfg_.virtual_scale))));
    chrome_h = std::max(240, static_cast<int>(std::lround(
                                  static_cast<double>(place_h_) / static_cast<double>(cfg_.virtual_scale))));
  }
  chrome_->set_window_size(chrome_w, chrome_h);
  chrome_->clear_window_position();
  if (follow_out) {
    emit logMessage(tr("Chrome Wayland: follow output scale %1% (window %2×%3 DIP, mode %4×%5)")
                        .arg(QString::number(static_cast<double>(cfg_.virtual_scale) * 100.0, 'f', 0))
                        .arg(chrome_w)
                        .arg(chrome_h)
                        .arg(place_w_)
                        .arg(place_h_));
  } else {
    emit logMessage(tr("Chrome force-device-scale-factor=%1 (virtual=%2 chrome_scale=%3 ozone=x11)")
                        .arg(QString::number(static_cast<double>(chrome_dsf), 'f', 2))
                        .arg(QString::number(static_cast<double>(cfg_.virtual_scale), 'f', 2))
                        .arg(QString::number(static_cast<double>(cfg_.chrome_scale), 'f', 2)));
  }
  // Borderless fullscreen on the target MONITOR (KWin also sets fullScreen after place).
  chrome_->set_start_fullscreen(true);
  chrome_->set_start_minimized(cfg_.virtual_display && desk_ == DesktopEnv::Kde);
  if (desk_ == DesktopEnv::Gnome) {
    chrome_->set_window_position(place_x_, place_y_);
    emit logMessage(tr("Live browser → %1 (%2×%3) @%4,%5 (GNOME / window-position)")
                        .arg(output_name_)
                        .arg(place_w_)
                        .arg(place_h_)
                        .arg(place_x_)
                        .arg(place_y_));
    beginPlaceGnome();
  } else {
    emit logMessage(tr("Live browser → %1 (%2×%3) via KWin")
                        .arg(output_name_)
                        .arg(place_w_)
                        .arg(place_h_));
    beginPlace();
  }
#endif
}

void YouTubeSession::stop_capture_from_xr() {
  // XR already reset DMA textures. Stop PipeWire before FDs/buffers are revoked underfoot.
  place_timer_.stop();
  ++portal_gen_;
  loading_.store(false, std::memory_order_release);
  active_.store(false, std::memory_order_release);
  audio_wanted_.store(false, std::memory_order_release);
#if defined(VRP_HAS_PIPEWIRE)
  if (capture_) capture_->stop();
#endif
}

void YouTubeSession::stop() {
  if (state_ == State::Stopping) return;
  const bool capture_alive =
#if defined(VRP_HAS_PIPEWIRE)
      (capture_ && capture_->running());
#else
      false;
#endif
  const bool chrome_alive = chrome_ && chrome_->running();
  const bool desk_alive =
      desktop_app_ && desktop_app_->state() != QProcess::NotRunning;
  if (state_ == State::Idle && !loading_.load() && !active_.load() && !capture_alive &&
      !chrome_alive && !desk_alive && kwin_window_id_.isEmpty()) {
    return;
  }
  place_timer_.stop();
  ++portal_gen_;
  setState(State::Stopping);
  loading_.store(false, std::memory_order_release);
  active_.store(false, std::memory_order_release);
  audio_wanted_.store(false, std::memory_order_release);
  // Capture + app only. Virtual stays up; VirtualDisplayManager::ensure/destroy owns it.
  emit logMessage(tr("Live stop: capture + app (Virtual left to manager)"));
  set_busy_status(tr("切替中"));
  cleanupStreaming();
  clear_busy_status();
  setState(State::Idle);
  emit stopped();
  emit logMessage(tr("Live capture stopped"));
}

void YouTubeSession::shutdown() {
  stop();
  // Virtual lifetime is Host/VirtualDisplayManager — never destroy from here
  // (child delete order + double shutdown caused QProcess UAF / SEGV).
  vdisp_mgr_ = nullptr;
}

bool YouTubeSession::resolveOutputRect(QRect* geo, QString* matched) {
  if (!geo) return false;
  if (KdeVirtualDisplay::queryOutputRect(output_name_, geo, matched) && !geo->isEmpty()) {
    return true;
  }
  const QList<QScreen*> screens = QGuiApplication::screens();
  for (QScreen* s : screens) {
    if (!s) continue;
    const QString blob =
        (s->name() + QLatin1Char(' ') + s->manufacturer() + QLatin1Char(' ') + s->model())
            .toLower();
    if (!output_name_.isEmpty() && blob.contains(output_name_.toLower())) {
      *geo = s->geometry();
      if (matched) *matched = s->name();
      return !geo->isEmpty();
    }
  }
  return false;
}

void YouTubeSession::beginPlace() {
  setState(State::Placing);
  chrome_ids_before_ = KwinWindowPlacer::list_candidate_ids();
  emit logMessage(tr("Chrome: %1 existing window(s) before launch").arg(chrome_ids_before_.size()));

  QString arm_err;
  const int chrome_scr = vdisp_mgr_ ? vdisp_mgr_->screen_index() : -1;
  if (!KwinWindowPlacer::arm_place_on_open(output_name_, chrome_ids_before_, &arm_err,
                                           chrome_scr)) {
    fail(tr("KWin place arm failed: %1 — abort (will not leave Chrome on primary)")
             .arg(arm_err.isEmpty() ? tr("(no detail)") : arm_err));
    return;
  }
  emit logMessage(tr("KWin: armed — next Chrome window → %1 (#%2)")
                      .arg(output_name_)
                      .arg(chrome_scr));

  if (QWidget* w = qobject_cast<QWidget*>(QObject::parent())) {
    w->activateWindow();
  }

  if (!chrome_->start()) {
    KwinWindowPlacer::disarm_place_on_open();
    fail(tr("Could not start Chromium/Chrome for live capture"));
    return;
  }
  place_tries_ = 0;
  place_timer_.start(400);
}

void YouTubeSession::beginPlaceGnome() {
  setState(State::Placing);
  chrome_ids_before_.clear();
  kwin_window_id_.clear();
  if (QWidget* w = qobject_cast<QWidget*>(QObject::parent())) {
    w->activateWindow();
  }
  if (!chrome_->start()) {
    fail(tr("Could not start Chromium/Chrome for live capture"));
    return;
  }
  place_tries_ = 0;
  place_timer_.start(400);
}

void YouTubeSession::beginPlaceDesktopApp() {
  setState(State::Placing);
  // KWin Wayland: Window Rules installed before the first map often do not apply
  // (startup/apply-on-open bug). Script-place first; install Rule only after the
  // window exists, with Exact + wmclasscomplete on the real class string.
  chrome_ids_before_ = KwinWindowPlacer::list_all_window_ids();
  emit logMessage(tr("Desktop app: %1 existing window(s) before launch")
                      .arg(chrome_ids_before_.size()));

  desktop_want_names_.clear();
  desktop_rule_installed_ = false;
  desktop_wmclass_.clear();
  {
    const QFileInfo fi(cfg_.exec);
    const QString base = fi.completeBaseName().toLower();
    if (!base.isEmpty()) desktop_want_names_ << base;
    if (base.contains(QLatin1String("retroarch")) ||
        cfg_.exec.contains(QLatin1String("retroarch"), Qt::CaseInsensitive)) {
      desktop_want_names_ << QStringLiteral("retroarch") << QStringLiteral("libretro")
                          << QStringLiteral("com.libretro.retroarch");
    }
    desktop_want_names_.removeDuplicates();
  }

  QString arm_err;
  // Pids unknown until start — name hints + exclude list (like Chrome class match).
  if (!KwinWindowPlacer::arm_place_new_window(output_name_, chrome_ids_before_, {},
                                              desktop_want_names_, &arm_err)) {
    fail(tr("KWin place arm failed: %1 — abort")
             .arg(arm_err.isEmpty() ? tr("(no detail)") : arm_err));
    return;
  }
  emit logMessage(tr("KWin: armed — next desktop window → %1 (names %2)")
                      .arg(output_name_, desktop_want_names_.join(QLatin1Char(','))));

  if (QWidget* w = qobject_cast<QWidget*>(QObject::parent())) {
    w->activateWindow();
  }

  {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (!cfg_.pad_hook_lib.isEmpty() && !cfg_.pad_hook_sock.isEmpty()) {
      const QString prev = env.value(QStringLiteral("LD_PRELOAD"));
      const QString preload =
          prev.isEmpty() ? cfg_.pad_hook_lib : (cfg_.pad_hook_lib + QLatin1Char(':') + prev);
      env.insert(QStringLiteral("LD_PRELOAD"), preload);
      env.insert(QStringLiteral("MONASPHERE_PAD_SOCK"), cfg_.pad_hook_sock);
      env.insert(QStringLiteral("MONASPHERE_PAD_HOOK"), cfg_.pad_hook_lib);
      emit logMessage(tr("Desktop app: LD_PRELOAD pad hook (%1)").arg(cfg_.pad_hook_lib));
    } else if (!cfg_.sdl_joystick_device.isEmpty()) {
      env.insert(QStringLiteral("SDL_JOYSTICK_HIDAPI"), QStringLiteral("0"));
      env.insert(QStringLiteral("SDL_JOYSTICK_DEVICE"), cfg_.sdl_joystick_device);
      emit logMessage(tr("Desktop app: SDL_JOYSTICK_DEVICE=%1 (legacy uinput)")
                          .arg(cfg_.sdl_joystick_device));
    }
    desktop_app_->setProcessEnvironment(env);
  }
  QStringList args = cfg_.exec_args;
  if (cfg_.exec.contains(QLatin1String("retroarch"), Qt::CaseInsensitive)) {
    const QString ra_cfg =
        QDir::temp().filePath(QStringLiteral("monasphere_ra_pad.cfg"));
    {
      QFile f(ra_cfg);
      if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        f.write("input_joypad_driver = \"sdl2\"\n");
        f.close();
      }
    }
    args.append(QStringLiteral("--appendconfig"));
    args.append(ra_cfg);
    emit logMessage(tr("Desktop app: RetroArch appendconfig → sdl2 joypad"));
  }
  desktop_app_->setProgram(cfg_.exec);
  desktop_app_->setArguments(args);
  emit logMessage(tr("Desktop app exec: %1 %2")
                      .arg(cfg_.exec, args.join(QLatin1Char(' '))));
  desktop_app_->start();
  if (!desktop_app_->waitForStarted(5000)) {
    KwinWindowPlacer::disarm_place_on_open();
    fail(tr("Could not start desktop app: %1 (%2)")
             .arg(cfg_.exec, desktop_app_->errorString()));
    return;
  }
  emit logMessage(tr("Desktop app started (pid %1)")
                      .arg(desktop_app_->processId()));

  place_tries_ = 0;
  place_timer_.start(200);
}

void YouTubeSession::pollPlaceDesktopApp() {
  if (state_ != State::Placing) return;

  QString wid;
  QString err;
  if (KwinWindowPlacer::poll_armed_place(&wid, &err) && !wid.isEmpty()) {
    if (kwin_window_id_.isEmpty()) {
      emit logMessage(tr("KWin: desktop app on %1 (%2…)").arg(output_name_, wid.left(8)));
    }
    kwin_window_id_ = wid;
  }

  if (!kwin_window_id_.isEmpty()) {
    // One Chrome-style shove, then done (rule already Force-screens).
    if (place_tries_ == 0) {
      const qint64 root_pid = desktop_app_ ? desktop_app_->processId() : 0;
      const QList<qint64> pids = KwinWindowPlacer::pid_tree(root_pid);
      bool moved_ok = false;
      QString move_wid;
      QString move_err;
      (void)KwinWindowPlacer::move_new_window_to_output(output_name_, chrome_ids_before_, pids,
                                                        desktop_want_names_, &move_wid, &move_err,
                                                        &moved_ok);
      if (moved_ok && !move_wid.isEmpty()) kwin_window_id_ = move_wid;
    }
    ++place_tries_;
    if (place_tries_ >= 2) {
      // Rule after map only — Exact + wmclasscomplete with the real class string.
      installDesktopAppSessionRule();
      KwinWindowPlacer::disarm_place_on_open(/*keep_fullscreen_rule=*/true);
      emit logMessage(tr("KWin: placed desktop app on %1 (%2…)")
                          .arg(output_name_, kwin_window_id_.left(8)));
      onPlaced();
      startDesktopAppMaintain();
      return;
    }
    place_timer_.start(200);
    return;
  }

  ++place_tries_;
  if (desktop_app_ && desktop_app_->state() == QProcess::NotRunning) {
    KwinWindowPlacer::disarm_place_on_open();
    fail(tr("Desktop app exited before window placement"));
    return;
  }

  if (place_tries_ > 0 && (place_tries_ % 5) == 0) {
    const qint64 root_pid = desktop_app_ ? desktop_app_->processId() : 0;
    const QList<qint64> pids = KwinWindowPlacer::pid_tree(root_pid);
    bool moved_ok = false;
    QString move_wid;
    QString move_err;
    if (KwinWindowPlacer::move_new_window_to_output(output_name_, chrome_ids_before_, pids,
                                                    desktop_want_names_, &move_wid, &move_err,
                                                    &moved_ok) &&
        moved_ok && !move_wid.isEmpty()) {
      kwin_window_id_ = move_wid;
      place_tries_ = 0;
      emit logMessage(
          tr("KWin: desktop app on %1 (%2…)").arg(output_name_, move_wid.left(8)));
      place_timer_.start(200);
      return;
    }
  }

  if (place_tries_ < 40) {
    if (place_tries_ == 1 || place_tries_ % 10 == 0) {
      emit logMessage(tr("KWin: waiting for desktop app on %1…").arg(output_name_));
    }
    place_timer_.start(200);
    return;
  }

  KwinWindowPlacer::disarm_place_on_open();
  fail(tr("KWin place failed — desktop app was not moved to %1 (ScreenCast not started)")
           .arg(output_name_));
}

void YouTubeSession::pollPlace() {
  if (state_ != State::Placing) return;
  if (desktopAppMode()) {
    pollPlaceDesktopApp();
    return;
  }
  if (desk_ == DesktopEnv::Gnome) {
    pollPlaceGnome();
    return;
  }

  (void)x11_.prepare_window(place_w_, place_h_, /*hide_host=*/false, /*move=*/false);
  if (!x11_.bind_output_by_name(output_name_.toStdString(), place_w_, place_h_)) {
    x11_.set_screen_rect(place_x_, place_y_, place_w_, place_h_);
  }

  QString wid;
  QString err;
  if (KwinWindowPlacer::poll_armed_place(&wid, &err)) {
    kwin_window_id_ = wid;
    KwinWindowPlacer::disarm_place_on_open(/*keep_fullscreen_rule=*/true);
    emit logMessage(tr("KWin: placed on %1 (%2…)").arg(output_name_, wid.left(8)));
    onPlaced();
    return;
  }

  ++place_tries_;
  if (place_tries_ >= 2 && place_tries_ <= 24) {
    bool moved_ok = false;
    QString move_err;
    QString move_wid;
    if (KwinWindowPlacer::move_youtube_to_output(output_name_, chrome_ids_before_, &move_wid,
                                                 &move_err, &moved_ok) &&
        moved_ok) {
      kwin_window_id_ = move_wid;
      KwinWindowPlacer::disarm_place_on_open(/*keep_fullscreen_rule=*/true);
      emit logMessage(tr("KWin: moved onto %1 (%2…)").arg(output_name_, move_wid.left(8)));
      onPlaced();
      return;
    }
  }

  if (place_tries_ < 30) {
    if (place_tries_ == 1 || place_tries_ % 5 == 0) {
      emit logMessage(tr("KWin: waiting for live browser on %1…").arg(output_name_));
    }
    place_timer_.start(350);
    return;
  }

  KwinWindowPlacer::disarm_place_on_open();
  fail(tr("KWin place failed — browser was not moved to %1 (ScreenCast not started)")
           .arg(output_name_));
}

void YouTubeSession::pollPlaceGnome() {
  if (state_ != State::Placing) return;

  (void)x11_.prepare_window(place_w_, place_h_, /*hide_host=*/false, /*move=*/true);
  if (!x11_.bind_output_by_name(output_name_.toStdString(), place_w_, place_h_)) {
    x11_.set_screen_rect(place_x_, place_y_, place_w_, place_h_);
  }

  if (x11_.have_window()) {
    emit logMessage(tr("GNOME: Chrome window bound on %1").arg(output_name_));
    onPlaced();
    return;
  }

  ++place_tries_;
  if (place_tries_ < 30) {
    if (place_tries_ == 1 || place_tries_ % 5 == 0) {
      emit logMessage(tr("GNOME: waiting for live browser…"));
    }
    place_timer_.start(350);
    return;
  }
  fail(tr("GNOME place failed — browser window not found (ScreenCast not started)"));
}

void YouTubeSession::onPlaced() {
  setState(State::Placed);
  // Cursor clamp = configured virtual-display resolution. XTest coords = Xrandr
  // (XWayland physical). kscreen logical pos/size diverge when primary scale≠1 —
  // that was the hard movement-range limit (e.g. half the Virtual MONITOR).
  QRect geo;
  QString matched;
  if (resolveOutputRect(&geo, &matched) && !geo.isEmpty()) {
    place_x_ = geo.x();
    place_y_ = geo.y();
    if (!matched.isEmpty()) output_name_ = matched;
  }
  place_w_ = cfg_.virtual_w > 0 ? cfg_.virtual_w : (geo.width() > 0 ? geo.width() : 1280);
  place_h_ = cfg_.virtual_h > 0 ? cfg_.virtual_h : (geo.height() > 0 ? geo.height() : 720);
  (void)x11_.prepare_window(place_w_, place_h_, /*hide_host=*/false, /*move=*/false);
  if (x11_.have_window()) (void)x11_.refresh_geometry();
  // Keep place_x_/y_ as kscreen *logical* origin for EIS. Xrandr physical
  // (often 2× under primary scale≠1) is only for XTest fallback inside x11_.
  if (!x11_.bind_output_by_name(output_name_.toStdString(), place_w_, place_h_)) {
    x11_.set_screen_rect(place_x_, place_y_, place_w_, place_h_);
    emit logMessage(tr("Pointer logical %1×%2 @%3,%4 (no Xrandr match)")
                        .arg(place_w_)
                        .arg(place_h_)
                        .arg(place_x_)
                        .arg(place_y_));
  } else {
    emit logMessage(tr("Pointer clamp %1×%2 @ logical %3,%4 (X11/XTest origin %5,%6)")
                        .arg(place_w_)
                        .arg(place_h_)
                        .arg(place_x_)
                        .arg(place_y_)
                        .arg(x11_.place_x())
                        .arg(x11_.place_y()));
  }
  x11_.set_cursor(place_w_ * 0.5f, place_h_ * 0.5f);
  if (desk_ == DesktopEnv::Kde) {
    QString act_err;
    if (KwinWindowPlacer::activate_youtube_window(kwin_window_id_, &act_err)) {
      emit logMessage(tr("KWin: activated live browser for pointer input"));
    }
  }

  // Virtual is already up before place (manager ensure). Restore only needs a short
  // beat so KWin publishes the output to the portal — 2.5s was the bulk of "slow RA".
  const int settle_ms = restore_token_.isEmpty() ? 300 : 500;
  QTimer::singleShot(settle_ms, this, [this] {
    if (state_ != State::Placed) return;
    beginPortal(false);
  });
}

void YouTubeSession::beginPortal(bool after_reject) {
  if (state_ != State::Placed && state_ != State::Portal) return;
  setState(State::Portal);

  // Snapshot host seat before ScreenCast / Virtual pointer steal it.
  rememberSeatCursor();

  const bool interactive = restore_token_.isEmpty();
  if (interactive) {
    if (QWidget* host = qobject_cast<QWidget*>(QObject::parent())) {
      if (QScreen* primary = QGuiApplication::primaryScreen()) {
        const QRect ag = primary->availableGeometry();
        QScreen* on = QGuiApplication::screenAt(host->frameGeometry().center());
        if (on != primary) {
          host->move(ag.x() + std::max(40, (ag.width() - host->width()) / 4),
                     ag.y() + std::max(40, (ag.height() - host->height()) / 4));
        }
      }
      if (host->isMinimized()) host->showNormal();
      host->show();
      host->raise();
      host->activateWindow();
      if (QWindow* wh = host->windowHandle()) wh->requestActivate();
    }
    if (desk_ == DesktopEnv::Kde) {
      QString arm_err;
      if (KwinWindowPlacer::arm_portal_dialog_on_primary(&arm_err)) {
        emit logMessage(tr("ScreenCast: portal dialog armed → primary"));
      }
    }
    emit toast(tr("Host: share ONLY “%1” (not whole desktop)").arg(output_name_));
    emit logMessage(tr("ScreenCast: pick MONITOR “%1” only — do not share the full desktop / both screens")
                        .arg(output_name_));
  } else {
    emit logMessage(tr("ScreenCast: restoring previous share…"));
  }

  auto launch = [this, interactive, after_reject] {
    if (state_ != State::Portal || !loading_.load()) return;
    QString parent;
    if (QWidget* host = qobject_cast<QWidget*>(QObject::parent())) {
      // Ensure native window exists before reading winId.
      host->winId();
      const QString plat = QGuiApplication::platformName();
      if (plat == QLatin1String("xcb")) {
        parent = QStringLiteral("x11:") + QString::number(static_cast<qulonglong>(host->winId()));
      } else if (plat.contains(QLatin1String("wayland"))) {
        parent = QStringLiteral("wayland:") + QString::number(static_cast<qulonglong>(host->winId()));
      } else {
        emit logMessage(tr("ScreenCast: parent window unavailable on platform “%1” "
                           "(dialog may appear without parenting)")
                            .arg(plat));
      }
    }
    const auto mode = (desk_ == DesktopEnv::Gnome)
                          ? ScreencastPortal::SessionMode::RemoteDesktopWithVideo
                          : ScreencastPortal::SessionMode::VideoOnly;
    portal_->start(parent, restore_token_, /*persist_mode*/ 2,
                   ScreencastPortal::SourceKind::Monitor, mode);
    armPortalWatchdog(!restore_token_.isEmpty() && !after_reject);
  };

  if (interactive) {
    QTimer::singleShot(400, this, launch);
  } else {
    launch();
  }
}

void YouTubeSession::armPortalWatchdog(bool had_token) {
  const int gen = ++portal_gen_;
  // Interactive: picker should appear within a few seconds after Start.
  // Keep shorter than before so a missed Request Response cannot leave Chrome up.
  const int wait_ms = had_token ? 12000 : 25000;
  QTimer::singleShot(wait_ms, this, [this, gen, had_token] {
    if (gen != portal_gen_) return;
    if (active_.load() || state_ != State::Portal) return;
    if (!portal_ || !portal_->busy()) {
      if (loading_.load() && !active_.load()) {
        fail(tr("ScreenCast: no stream after wait"));
      }
      return;
    }
    if (had_token && !portal_retry_used_) {
      portal_->stop();
      if (portal_restore_retries_ < 2) {
        ++portal_restore_retries_;
        emit logMessage(tr("ScreenCast restore hung — silent retry %1/2 (token kept)")
                            .arg(portal_restore_retries_));
        QTimer::singleShot(2000, this, [this] {
          if (!loading_.load() || active_.load()) return;
          setState(State::Placed);
          beginPortal(false);
        });
        return;
      }
      portal_retry_used_ = true;
      emit logMessage(tr("ScreenCast restore hung — opening host picker (conf token kept)"));
      restore_token_.clear();
      emit toast(tr("Host: pick Virtual-monaSphere"));
      QTimer::singleShot(400, this, [this] {
        if (!loading_.load() || active_.load()) return;
        setState(State::Placed);
        beginPortal(true);
      });
      return;
    }
    portal_->stop();
    fail(tr("ScreenCast timed out — approve on the host desktop"));
  });
}

void YouTubeSession::onPortalStarted(const ScreencastPortal::Result& r) {
  if (state_ != State::Portal) {
    if (r.pw_fd >= 0) ::close(r.pw_fd);
    if (r.eis_fd >= 0) ::close(r.eis_fd);
    return;
  }
  KwinWindowPlacer::disarm_portal_dialog_on_primary();
  ++portal_gen_;

  // Stream must be the chosen MONITOR alone. A dual-desktop stream (e.g. 3840×1080)
  // leaves the cursor stuck on the host half.
  const int vw = cfg_.virtual_w > 0 ? cfg_.virtual_w : place_w_;
  const int vh = cfg_.virtual_h > 0 ? cfg_.virtual_h : place_h_;
  if (r.width > 0 && r.height > 0 && vw > 0 && vh > 0 &&
      (r.width > vw + 64 || r.height > vh + 64)) {
    if (r.pw_fd >= 0) ::close(r.pw_fd);
    if (r.eis_fd >= 0) ::close(r.eis_fd);
    if (portal_) portal_->stop();
    restore_token_.clear();
    if (vdisp_mgr_) vdisp_mgr_->clearToken(vdisp_mgr_->active_span());
    else emit restoreTokenChanged(QString());
    fail(tr("ScreenCast is %1×%2 but MONITOR “%3” is %4×%5. "
            "On the host picker choose ONLY that MONITOR — not the whole desktop / both screens. "
            "Then Tools→Set up again.")
             .arg(r.width)
             .arg(r.height)
             .arg(output_name_)
             .arg(vw)
             .arg(vh));
    return;
  }

  if (!r.restore_token.isEmpty()) {
    restore_token_ = r.restore_token;
    if (vdisp_mgr_) {
      const int span = vdisp_mgr_->active_span() > 0
                           ? vdisp_mgr_->active_span()
                           : VirtualDisplayManager::spanFromName(output_name_);
      vdisp_mgr_->setToken(span, restore_token_);
    } else {
      emit restoreTokenChanged(restore_token_);
    }
  } else {
    emit logMessage(tr("Live ScreenCast: portal returned no restore_token "
                       "(enable Remember on the share dialog, then Set up again)"));
  }

#if defined(VRP_HAS_PIPEWIRE)
  if (r.pw_fd < 0) {
    if (r.eis_fd >= 0) ::close(r.eis_fd);
    fail(tr("ScreenCast: no PipeWire fd"));
    return;
  }
  if (!capture_) capture_ = std::make_unique<PipewireCapture>();
  // Prefer configured Virtual-* pixels so KWin does not resize the stream down
  // to a hardcoded 1080p default during PipeWire format negotiation.
  const int prefer_w = cfg_.virtual_w > 0 ? cfg_.virtual_w : r.width;
  const int prefer_h = cfg_.virtual_h > 0 ? cfg_.virtual_h : r.height;
  if (!capture_->start(r.pw_fd, r.node_id, r.pipewire_serial, cfg_.drm_modifiers, prefer_w,
                       prefer_h)) {
    const QString err = QString::fromStdString(capture_->last_error());
    ::close(r.pw_fd);
    if (r.eis_fd >= 0) ::close(r.eis_fd);
    fail(tr("PipeWire capture failed: %1").arg(err));
    return;
  }
  active_.store(true, std::memory_order_release);
  // Keep loading until first XR frame (mark_frame_presented).
  if (cfg_.enable_audio) {
    audio_wanted_.store(true, std::memory_order_release);
  }
  setState(State::Streaming);

  x11_.set_eis_origin(place_x_, place_y_);
  // Drop any prior EIS without restoring (fresh start — cursor still on host).
  have_saved_cursor_ = false;
  x11_.set_eis_inject(nullptr);
  if (kde_eis_) kde_eis_->stop();
  if (portal_eis_) portal_eis_->stop();

  bool eis_ok = false;
  if (desk_ == DesktopEnv::Gnome) {
    if (r.eis_fd < 0) {
      emit logMessage(tr("Pointer: portal ConnectToEIS returned no fd — XTest fallback"));
    } else if (!portal_eis_) {
      ::close(r.eis_fd);
      emit logMessage(tr("Pointer: portal EIS null — XTest fallback"));
    } else if (portal_eis_->start_from_fd(r.eis_fd, r.mapping_id.toStdString())) {
      // start_from_fd owns eis_fd (success or failure).
      x11_.set_eis_inject(portal_eis_);
      eis_ok = true;
      emit logMessage(tr("Pointer: portal EIS (%1) mapping_id=%2 region %3×%4; cursor %5×%6")
                          .arg(portal_eis_->region_local() ? tr("region-local")
                                                          : tr("desktop-global"))
                          .arg(r.mapping_id.isEmpty() ? tr("(none)") : r.mapping_id)
                          .arg(portal_eis_->region_w())
                          .arg(portal_eis_->region_h())
                          .arg(place_w_)
                          .arg(place_h_));
      if (portal_eis_->region_local() && portal_eis_->region_w() > 0 &&
          (static_cast<int>(portal_eis_->region_w()) != place_w_ ||
           static_cast<int>(portal_eis_->region_h()) != place_h_)) {
        VRP_LOG("live: EIS logical %ux%u vs virtual %dx%d — scaling cursor into region",
                portal_eis_->region_w(), portal_eis_->region_h(), place_w_, place_h_);
      }
    } else {
      emit logMessage(tr("Pointer: portal EIS failed (%1) — XTest fallback")
                          .arg(QString::fromStdString(portal_eis_->last_error())));
    }
  } else {
    if (r.eis_fd >= 0) ::close(r.eis_fd);
    if (kde_eis_ && kde_eis_->start(output_name_.toStdString())) {
      x11_.set_eis_inject(&kde_eis_->core());
      eis_ok = true;
      const auto& eis = kde_eis_->core();
      emit logMessage(tr("Pointer: KWin EIS region for “%1” (local=%2, region %3×%4 @%5,%6; "
                         "cursor space %7×%8)")
                          .arg(output_name_)
                          .arg(eis.region_local() ? tr("yes") : tr("no"))
                          .arg(eis.region_w())
                          .arg(eis.region_h())
                          .arg(eis.region_x())
                          .arg(eis.region_y())
                          .arg(place_w_)
                          .arg(place_h_));
      if (eis.region_local() && eis.region_w() > 0 && eis.region_h() > 0 &&
          (static_cast<int>(eis.region_w()) != place_w_ ||
           static_cast<int>(eis.region_h()) != place_h_)) {
        VRP_LOG("live: EIS logical %ux%u vs virtual %dx%d — scaling cursor into region",
                eis.region_w(), eis.region_h(), place_w_, place_h_);
      }
    } else {
      emit logMessage(tr("Pointer: KWin EIS unavailable (%1) — XTest fallback")
                          .arg(kde_eis_ ? QString::fromStdString(kde_eis_->last_error())
                                       : tr("null")));
    }
  }

  // Keep beginPortal snapshot; only fill in if that failed.
  if (eis_ok) rememberSeatCursor();

  // Re-focus after ScreenCast steals attention from the Chrome window.
  QTimer::singleShot(400, this, [this] {
    if (!active_.load()) return;
    if (desk_ == DesktopEnv::Kde) {
      QString err;
      (void)KwinWindowPlacer::activate_youtube_window(kwin_window_id_, &err);
    }
    (void)x11_.ensure_inject_target();
    x11_.set_cursor(place_w_ * 0.5f, place_h_ * 0.5f);
  });
  emit streamingStarted();
  emit logMessage(tr("Live source active (node %1)").arg(r.node_id));
#else
  if (r.eis_fd >= 0) ::close(r.eis_fd);
  (void)r;
  fail(tr("Built without PipeWire"));
#endif
}

void YouTubeSession::onPortalFailed(const QString& e) {
  if (state_ != State::Portal) return;
  // Spurious overlapping start() — in-flight portal must keep running.
  if (e.contains(QStringLiteral("already starting"), Qt::CaseInsensitive) ||
      e.contains(QStringLiteral("already in progress"), Qt::CaseInsensitive)) {
    VRP_LOG("live: ignoring overlapping ScreenCast start");
    return;
  }
  KwinWindowPlacer::disarm_portal_dialog_on_primary();
  // Cancel restore/hung watchdogs before scheduling another Start.
  ++portal_gen_;

  const bool restore_rejected =
      e.contains(QStringLiteral("restore rejected"), Qt::CaseInsensitive) ||
      (portal_ && portal_->used_restore_token() &&
       e.contains(QStringLiteral("cancelled"), Qt::CaseInsensitive));

  if (restore_rejected && !restore_token_.isEmpty() && loading_.load()) {
    // Portal session is dead after Response≠0 — drop it before retry.
    if (portal_) portal_->stop();
    if (portal_restore_retries_ < 3 && !portal_retry_used_) {
      ++portal_restore_retries_;
      emit logMessage(tr("ScreenCast restore rejected — silent retry %1/3 (token kept)")
                          .arg(portal_restore_retries_));
      // Virtual recreate + 4K scale: portal needs a longer settle than 1080p.
      const int delay_ms = 4500 + portal_restore_retries_ * 1500;
      QTimer::singleShot(delay_ms, this, [this] {
        if (!loading_.load() || active_.load()) return;
        if (portal_ && portal_->busy()) {
          emit logMessage(tr("ScreenCast: waiting for previous Start to finish…"));
          QTimer::singleShot(2000, this, [this] {
            if (!loading_.load() || active_.load()) return;
            if (portal_ && portal_->busy()) portal_->stop();
            setState(State::Placed);
            beginPortal(false);
          });
          return;
        }
        setState(State::Placed);
        beginPortal(false);
      });
      return;
    }
    if (!portal_retry_used_) {
      portal_retry_used_ = true;
      // Memory only — keep conf token until a successful Start replaces it.
      emit logMessage(tr("ScreenCast restore still rejected — opening host picker "
                         "(conf token kept until a new one is saved). "
                         "If Virtual size/scale changed, re-run Tools → Set up live capture."));
      restore_token_.clear();
      emit toast(tr("Host: pick Virtual-monaSphere"));
      QTimer::singleShot(1200, this, [this] {
        if (!loading_.load() || active_.load()) return;
        if (portal_ && portal_->busy()) portal_->stop();
        setState(State::Placed);
        beginPortal(true);
      });
      return;
    }
  }

  fail(tr("ScreenCast failed: %1").arg(e));
}

}  // namespace vrp
