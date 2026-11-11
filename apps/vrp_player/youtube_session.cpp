#include "youtube_session.hpp"

#include "audio/pulse_monitor.hpp"
#include "capture/kwin_window_placer.hpp"
#include "common.hpp"

#include "capture/gnome_portal_dialog.hpp"
#include "capture/kde_virtual_display.hpp"

#include <QCoreApplication>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIODevice>
#include <QMetaObject>
#include <QScreen>
#include <QThread>
#include <QTimer>
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
  meta_paint_timer_.setSingleShot(false);
  // Stage damage only — never EIS warp here. Periodic set_cursor stole the seat
  // pointer onto Meta coords (mouse stuck; unlike KDE which only moves via XR
  // stick → x11_.move_cursor). Frame uniqueness comes from Embedded cursor +
  // the same stick/D-pad path as KDE.
  meta_paint_timer_.setInterval(200);
  connect(&meta_paint_timer_, &QTimer::timeout, this, [this] {
    if (desk_ != DesktopEnv::Gnome || !cfg_.virtual_display) return;
    if (state_ != State::Streaming && !active_.load(std::memory_order_acquire)) return;
    const bool kicked = meta_paint_kick_.exchange(false, std::memory_order_acq_rel);
    (void)x11_.force_output_damage(/*quiet=*/!kicked);
    if (kicked) {
      VRP_DISPLAY_LOG("live-meta: paint kick (damage only, no cursor warp)");
    }
  });

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
  // MutterVirtualScreencast lives on VirtualDisplayManager — wired in set_virtual_manager.
}

void YouTubeSession::set_virtual_manager(VirtualDisplayManager* mgr) {
  vdisp_mgr_ = mgr;
  if (!mgr || !mgr->mutter()) return;
  connect(mgr->mutter(), &MutterVirtualScreencast::failed, this, &YouTubeSession::onPortalFailed,
          Qt::UniqueConnection);
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
  busy_percent_.store(-1, std::memory_order_release);
}

QString YouTubeSession::busy_status() const {
  std::lock_guard<std::mutex> lock(busy_mu_);
  return busy_status_;
}

void YouTubeSession::set_busy_progress(int percent) {
  busy_percent_.store(percent, std::memory_order_release);
}

int YouTubeSession::busy_progress() const {
  return busy_percent_.load(std::memory_order_acquire);
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
    emit logMessage(tr("Desktop app: Window Rule after map complete=%1 -> %2 (#%3) Exact")
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

void YouTubeSession::kick_meta_paint() {
  meta_paint_kick_.store(true, std::memory_order_release);
  // XR thread cannot touch X11; queue damage only (no EIS warp — KDE parity).
  QMetaObject::invokeMethod(
      this,
      [this] {
        if (!meta_paint_kick_.exchange(false, std::memory_order_acq_rel)) return;
        if (desk_ != DesktopEnv::Gnome || !cfg_.virtual_display) return;
        if (state_ != State::Streaming && !active_.load(std::memory_order_acquire)) return;
        (void)x11_.force_output_damage(/*quiet=*/false);
        VRP_DISPLAY_LOG("live-meta: paint kick immediate (damage only)");
      },
      Qt::QueuedConnection);
}

void YouTubeSession::cleanupStreaming() {
  stopDesktopAppMaintain();
  meta_paint_timer_.stop();
  ++gnome_evacuate_gen_;  // cancel pending Chrome launch / Meta paint timers
  switch_hold_content_ = false;
  pending_switch_reveal_ = false;
  KwinWindowPlacer::disarm_place_on_open();
  KwinWindowPlacer::disarm_portal_dialog_on_primary();

  // GNOME RecordVirtual: Meta-* must not be torn down while ANY monasphere
  // client still references it (Chrome Wayland/X11, Meta paint window, seat).
  // Order: clients → cursor off Meta → settle → PW consumer → Session.Stop.
  // VRP_TEARDOWN_LOG writes ~/.config/monasphere/live-teardown.log + /tmp/…
  // so breadcrumbs survive a shell logout mid-teardown.
  const bool gnome_meta = (desk_ == DesktopEnv::Gnome && cfg_.virtual_display);
  if (gnome_meta) {
    VRP_TEARDOWN_LOG(
        "GNOME teardown BEGIN — mutter=%d chrome_running=%d pid=%lld x11_win=%d eis=%d",
        using_mutter_virtual_ ? 1 : 0, (chrome_ && chrome_->running()) ? 1 : 0,
        static_cast<long long>(chrome_ ? chrome_->process_id() : 0), x11_.have_window() ? 1 : 0,
        (portal_eis_ && portal_eis_->ready()) ? 1 : 0);
    emit logMessage(tr("GNOME: closing Live clients before Meta teardown"));
  }

  // 1) Move X11 Chrome off Meta (Wayland has no xid — killed in step 3).
  if (gnome_meta && x11_.have_window()) {
    if (QScreen* primary = QGuiApplication::primaryScreen()) {
      const QRect g = primary->geometry();
      x11_.set_screen_rect(g.x(), g.y(), std::max(320, g.width()), std::max(240, g.height()));
      (void)x11_.request_fullscreen(false);
      (void)x11_.prepare_window(std::max(320, g.width() / 2), std::max(240, g.height() / 2),
                                /*hide_host=*/false, /*move=*/true);
      VRP_TEARDOWN_LOG("GNOME teardown: moved X11 live window off Meta → primary");
      emit logMessage(tr("GNOME: moved live window off virtual before teardown"));
    }
  }

  // 2) Ask the window to close (WM_DELETE) where we have a handle.
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
    // Meta paint nudge window sits on Meta-* — destroy before Session.Stop.
    x11_.destroy_damage_window();
    x11_.clear();
    if (gnome_meta) {
      VRP_TEARDOWN_LOG("GNOME teardown: window close done closed=%d damage cleared", closed ? 1 : 0);
    }
    emit logMessage(closed ? tr("Live window closed")
                           : tr("Live window close: %1")
                                 .arg(err.isEmpty() ? tr("not found") : err));
  }
  kwin_window_id_.clear();
  chrome_ids_before_.clear();

  // 3) Kill browser / desktop-app processes (isolated profile + zygotes).
  if (desktopAppMode()) {
    stopDesktopAppProcess();
  } else if (chrome_) {
    chrome_->stop();
  }
  if (gnome_meta) {
    // Belt-and-suspenders: even if launcher thought profile was shared / already
    // dead, scrub monasphere chromium-yt / firefox-yt off Meta-*.
    ChromiumYtLauncher::killIsolatedLiveBrowsers(cfg_.browser);
    VRP_TEARDOWN_LOG("GNOME teardown: isolated Live browsers stopped (Meta clients clear)");
  }

  // 4) Seat + portal dialogs off Meta, then settle so Mutter drops surfaces.
  if (gnome_meta) {
    (void)evacuateGnomePortalDialogsOnce();
    if (QScreen* primary = QGuiApplication::primaryScreen()) {
      const QRect g = primary->geometry();
      const QPoint center(g.x() + g.width() / 2, g.y() + g.height() / 2);
      QCursor::setPos(primary, center);
      if (portal_eis_ && portal_eis_->ready()) {
        portal_eis_->set_buttons_enabled(false);
        portal_eis_->warp_desktop(static_cast<double>(center.x()),
                                  static_cast<double>(center.y()));
      }
    }
    VRP_TEARDOWN_LOG("GNOME teardown: seat off Meta — settle 1500ms before Session.Stop");
    QEventLoop loop;
    QTimer::singleShot(1500, &loop, &QEventLoop::quit);
    loop.exec();
  }

  // 5) Drop EIS, then PW consumer, then RecordVirtual / portal Session.Stop
  //    (this is what removes Meta-*).
  VRP_TEARDOWN_LOG("GNOME teardown: stopEis…");
  stopEis();
#if defined(VRP_HAS_PIPEWIRE)
  VRP_TEARDOWN_LOG("GNOME teardown: PipeWire capture stop…");
  if (capture_) capture_->stop();
  VRP_TEARDOWN_LOG("GNOME teardown: PipeWire capture stopped");
#endif
  if (portal_) {
    VRP_TEARDOWN_LOG("GNOME teardown: portal Session.Stop…");
    portal_->stop();
  }
  if (vdisp_mgr_ && vdisp_mgr_->gnome_meta_alive()) {
    VRP_TEARDOWN_LOG("GNOME teardown: Mutter RecordVirtual/RD Session.Stop (Meta dies here)…");
    vdisp_mgr_->destroy();
    VRP_TEARDOWN_LOG("GNOME teardown: Mutter Session.Stop returned");
  }
  using_mutter_virtual_ = false;
  gnome_live_ready_ = false;
  if (gnome_meta) {
    VRP_TEARDOWN_LOG("GNOME teardown: Meta gone — post-settle 600ms");
    QEventLoop loop;
    QTimer::singleShot(600, &loop, &QEventLoop::quit);
    loop.exec();
    VRP_TEARDOWN_LOG("GNOME teardown END (clients cleared before Meta)");
  }
#if defined(VRP_HAS_PULSE)
  // Always scrub monaSphereYt — even when Flat live audio was never started
  // (older builds set PULSE_SINK unconditionally and left the null sink around).
  // Synchronous: stopped() unmutes only after this returns, so the leftover
  // buffer is dropped while the output is still muted.
  (void)PulseMonitor::restore_browser_routing();
#endif
}

void YouTubeSession::mark_frame_presented() {
  // GNOME Meta: first PW frames are wallpaper — keep spinner until Chrome + EIS ready.
  if (using_mutter_virtual_ && !gnome_live_ready_) return;
  // Live↔Live: ignore frames until the new browser is placed.
  if (switch_hold_content_) {
    if (!pending_switch_reveal_) return;
    switch_hold_content_ = false;
    pending_switch_reveal_ = false;
  }
  if (loading_.load(std::memory_order_acquire)) {
    loading_.store(false, std::memory_order_release);
  }
  clear_busy_status();
}

void YouTubeSession::closeLiveBrowserWindow() {
  place_timer_.stop();
  KwinWindowPlacer::disarm_place_on_open();
  QString err;
  bool closed = false;
  if (desk_ == DesktopEnv::Kde) {
    if (!kwin_window_id_.isEmpty()) {
      closed = KwinWindowPlacer::close_window_by_id(kwin_window_id_, &err);
    }
    if (!closed) closed = KwinWindowPlacer::close_youtube_window(&err);
    if (!closed && kwin_window_id_.isEmpty()) {
      closed = KwinWindowPlacer::close_chrome_except(chrome_ids_before_, &err);
    }
  }
  if (!closed && x11_.have_window()) closed = x11_.close_window();
  // clear() drops the EIS inject pointer — keep ScreenCast EIS across Chrome relaunch.
  x11_.clear();
  if (EisPointer* eis = activeEis()) x11_.set_eis_inject(eis);
  kwin_window_id_.clear();
  chrome_ids_before_.clear();
  if (chrome_) chrome_->stop();
  emit logMessage(closed ? tr("Live: browser window closed for switch")
                         : tr("Live: browser close for switch: %1")
                               .arg(err.isEmpty() ? tr("not found") : err));
}

bool YouTubeSession::switch_live_url(const QString& url, const QString& label) {
  const QString next = url.trimmed();
  if (next.isEmpty()) return false;
  if (desktopAppMode()) return false;
  if (!active_.load(std::memory_order_acquire)) return false;
  if (state_ != State::Streaming && state_ != State::Placed) return false;
#if defined(VRP_HAS_PIPEWIRE)
  if (!capture_ || !capture_->running()) return false;
#else
  return false;
#endif

  emit logMessage(tr("Live: switch URL (keep Virtual/ScreenCast) → %1").arg(next));
  cfg_.url = next;
  if (!label.trimmed().isEmpty()) cfg_.launch_title = label.trimmed();
  switch_hold_content_ = true;
  pending_switch_reveal_ = false;
  if (using_mutter_virtual_) gnome_live_ready_ = false;
  loading_.store(true, std::memory_order_release);
  // Same spinner text as first-launch after Virtual is up (title only, no 「起動中」).
  set_busy_status(!cfg_.launch_title.isEmpty() ? cfg_.launch_title : tr("Live"));

  closeLiveBrowserWindow();
  configureLiveBrowserLaunch();
  if (desk_ == DesktopEnv::Gnome && cfg_.virtual_display) {
    beginPlaceGnome();
  } else {
    beginPlace();
  }
  return true;
}

void YouTubeSession::mark_gnome_live_ready() {
  gnome_live_ready_ = true;
  mark_frame_presented();
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

  // Clients first — never Session.Stop / PW-stop Meta while Chrome still maps
  // there (GNOME shell assert → logout). Same order as cleanupStreaming.
  have_saved_cursor_ = false;  // new session — do not restore mid-restart
  stopDesktopAppMaintain();
  ++gnome_evacuate_gen_;
  if (chrome_) chrome_->stop();
  stopDesktopAppProcess();
  x11_.destroy_damage_window();
  x11_.clear();
  if (desk_ == DesktopEnv::Gnome || using_mutter_virtual_) {
    ChromiumYtLauncher::killIsolatedLiveBrowsers(cfg_.browser);
    if (QScreen* primary = QGuiApplication::primaryScreen()) {
      const QPoint c = primary->geometry().center();
      QCursor::setPos(primary, c);
    }
    QEventLoop loop;
    QTimer::singleShot(800, &loop, &QEventLoop::quit);
    loop.exec();
  }
  stopEis();
#if defined(VRP_HAS_PIPEWIRE)
  if (capture_) capture_->stop();
#endif
  if (portal_) portal_->stop();
  // Meta Session.Stop via manager (same destroy path Host uses after stop).
  if (vdisp_mgr_ && vdisp_mgr_->gnome_meta_alive()) {
    vdisp_mgr_->destroy();
  }
  using_mutter_virtual_ = false;
  gnome_live_ready_ = false;
  if (gnome_eis_fd_ >= 0) {
    ::close(gnome_eis_fd_);
    gnome_eis_fd_ = -1;
  }
  gnome_node_id_ = 0;
  gnome_mapping_id_.clear();
  desktop_rule_installed_ = false;
  desktop_wmclass_.clear();

  desk_ = detect_desktop_env();
  emit logMessage(tr("Live desktop: %1")
                      .arg(QString::fromLatin1(desktop_env_name(desk_))));

  gnome_defer_browser_ = false;
  gnome_capture_only_ = false;
  gnome_live_ready_ = false;
  ++gnome_evacuate_gen_;
  if (cfg_.virtual_display) {
    if (desk_ == DesktopEnv::Gnome) {
      // GNOME Meta-* via VirtualDisplayManager (same ensure → title spinner as KDE).
      if (!vdisp_mgr_) {
        fail(tr("Live setup aborted (no VirtualDisplayManager)"));
        return;
      }
      if (!GnomeVirtualDisplay::managedVirtualAvailable()) {
        fail(tr("GNOME managed Virtual needs portal VIRTUAL or Mutter.ScreenCast "
                "(install xdg-desktop-portal-gnome). Or set live_virtual=0 + live_output=."));
        return;
      }
      VirtualDisplayManager::Spec spec;
      spec.name = QStringLiteral("Meta");
      spec.width = cfg_.virtual_w > 0 ? cfg_.virtual_w : 1280;
      spec.height = cfg_.virtual_h > 0 ? cfg_.virtual_h : 720;
      spec.scale =
          (cfg_.virtual_scale >= 1.f && cfg_.virtual_scale <= 3.f) ? cfg_.virtual_scale : 2.f;
      spec.span = 1;
      const auto vr = vdisp_mgr_->ensureGnomeMeta(spec);
      if (!vr.ok) {
        fail(tr("Live setup aborted (Meta Virtual): %1")
                 .arg(vr.error.isEmpty() ? tr("unknown") : vr.error));
        return;
      }
      using_mutter_virtual_ = true;
      output_name_ =
          cfg_.output_hint.isEmpty() ? vr.output_name : cfg_.output_hint;
      if (output_name_.isEmpty()) output_name_ = GnomeVirtualDisplay::outputNameHint();
      restore_token_.clear();  // Mutter path has no ScreenCast restore token
      place_w_ = spec.width;
      place_h_ = spec.height;
      place_x_ = 0;
      place_y_ = 0;
      gnome_node_id_ = vr.node_id;
      gnome_eis_fd_ = vr.eis_fd;
      gnome_mapping_id_ = vr.mapping_id;
      setState(State::VDispReady);
      set_busy_progress(-1);
      const QString title = !cfg_.launch_title.isEmpty()
                                ? cfg_.launch_title
                                : (desktopAppMode()
                                       ? (desktop_label_.isEmpty() ? tr("アプリ") : desktop_label_)
                                       : QStringLiteral("YouTube"));
      set_busy_status(title);
      gnome_defer_browser_ = true;
      gnome_capture_only_ = false;
    } else {
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
      if (!vr.output_name.isEmpty()) output_name_ = vr.output_name;
      else if (!vdisp_mgr_->output_name().isEmpty()) output_name_ = vdisp_mgr_->output_name();
      if (vr.screen_index < 0) {
        emit logMessage(tr("Virtual: warning — KWin screen index unknown; place may miss Virtual"));
      }
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
      set_busy_progress(-1);
      const QString title = !cfg_.launch_title.isEmpty()
                                ? cfg_.launch_title
                                : (desktopAppMode()
                                       ? (desktop_label_.isEmpty() ? tr("アプリ") : desktop_label_)
                                       : QStringLiteral("YouTube"));
      set_busy_status(title);
    }
  } else if (!cfg_.output_hint.isEmpty()) {
    output_name_ = cfg_.output_hint;
  }

  QRect geo;
  QString matched;
  if (!gnome_defer_browser_) {
    if (!resolveOutputRect(&geo, &matched) || geo.isEmpty()) {
      fail(tr("No live MONITOR found (output=%1)")
               .arg(output_name_.isEmpty() ? tr("(empty)") : output_name_));
      return;
    }
    if (!matched.isEmpty()) output_name_ = matched;
    place_x_ = geo.x();
    place_y_ = geo.y();
    place_w_ = cfg_.virtual_w > 0 ? cfg_.virtual_w : (geo.width() > 0 ? geo.width() : 1280);
    place_h_ = cfg_.virtual_h > 0 ? cfg_.virtual_h : (geo.height() > 0 ? geo.height() : 720);
  }

  if (!cfg_.enable_audio) {
#if defined(VRP_HAS_PULSE)
    // Scrub orphan monaSphereYt so Plasma does not keep a filter sink around.
    std::thread([] { (void)PulseMonitor::restore_browser_routing(); }).detach();
#endif
  } else {
#if defined(VRP_HAS_PULSE)
    // Sink before guest so PULSE_SINK / PIPEWIRE_NODE can bind at process start.
    const std::string mon = PulseMonitor::ensure_yt_null_sink();
    if (mon.empty()) {
      emit logMessage(tr("Live audio: could not create monaSphereYt — system sink only"));
    } else {
      emit logMessage(tr("Live audio: monaSphereYt ready before guest (%1)")
                          .arg(QString::fromStdString(mon)));
    }
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
    emit logMessage(tr("Desktop app -> %1 (%2×%3): %4")
                        .arg(output_name_)
                        .arg(place_w_)
                        .arg(place_h_)
                        .arg(cfg_.exec));
    beginPlaceDesktopApp();
    return;
  }

  if (gnome_defer_browser_) {
    // Portal VIRTUAL creates the MONITOR; browser starts in onPortalStarted.
    emit logMessage(tr("GNOME Virtual: ScreenCast VIRTUAL first (browser after monitor exists)"));
    setState(State::Placed);
    beginPortal(false);
    return;
  }

  chrome_->set_force_x11(desk_ == DesktopEnv::Gnome);
  configureLiveBrowserLaunch();
  if (desk_ == DesktopEnv::Gnome) {
    chrome_->set_window_position(place_x_, place_y_);
    emit logMessage(tr("Live browser -> %1 (%2×%3) @%4,%5 (GNOME / X11 place)")
                        .arg(output_name_)
                        .arg(place_w_)
                        .arg(place_h_)
                        .arg(place_x_)
                        .arg(place_y_));
    beginPlaceGnome();
  } else {
    emit logMessage(tr("Live browser -> %1 (%2×%3) via KWin")
                        .arg(output_name_)
                        .arg(place_w_)
                        .arg(place_h_));
    beginPlace();
  }
#endif
}

void YouTubeSession::configureLiveBrowserLaunch() {
  chrome_->set_url(cfg_.url);
  // Wayland Chrome follows output scale — do NOT also push virtual_scale into
  // --force-device-scale-factor (that became ~400% at scale=2). X11 ozone mirrors
  // virtual_scale when live_chrome_scale is left at 1.
  // Callers set force_x11 for GNOME X11 place; platform-Meta uses Wayland (force_x11=0).
  const bool ozone_x11 =
      chrome_->force_x11() || !qEnvironmentVariableIsEmpty("MONASPHERE_YT_X11");
  float chrome_dsf = cfg_.chrome_scale;
  if (ozone_x11 && cfg_.virtual_display && cfg_.virtual_scale > 1.01f &&
      std::abs(cfg_.chrome_scale - 1.f) < 0.05f) {
    chrome_dsf = cfg_.virtual_scale;
  }
  chrome_->set_browser(cfg_.browser);
  chrome_->set_device_scale_factor(chrome_dsf);
  const bool follow_out = !ozone_x11 && std::abs(cfg_.chrome_scale - 1.f) < 0.05f;
  chrome_->set_follow_output_scale(follow_out);
  // GNOME: shared/default Chrome is a Wayland singleton — it ignores ozone=x11,
  // --window-position, and --class, so the Live window always lands on the host.
  // Isolated profile = own process we can place on Meta-*.
  const bool gnome_isolated =
      desk_ == DesktopEnv::Gnome && cfg_.virtual_display && cfg_.browser != LiveBrowser::Firefox;
  chrome_->set_use_default_profile(gnome_isolated ? false : cfg_.use_default_chrome_profile);
  if (gnome_isolated && cfg_.use_default_chrome_profile) {
    emit logMessage(tr("GNOME Live: isolated Chrome profile (shared Chrome cannot place on Virtual)"));
  }
  chrome_->set_route_audio_to_yt_sink(cfg_.enable_audio);
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
  chrome_->set_start_fullscreen(cfg_.browser != LiveBrowser::Firefox);
  chrome_->set_start_minimized(cfg_.virtual_display && desk_ == DesktopEnv::Kde &&
                               cfg_.browser != LiveBrowser::Firefox);
  if (cfg_.browser == LiveBrowser::Firefox) {
    emit logMessage(tr("Firefox Live: --new-instance + XWayland; maximize (no compositor FS)"));
  }
}

void YouTubeSession::stop_capture_from_xr() {
  // XR already reset DMA textures. Stop PipeWire before FDs/buffers are revoked underfoot.
  place_timer_.stop();
  ++portal_gen_;
  loading_.store(false, std::memory_order_release);
  active_.store(false, std::memory_order_release);
  audio_wanted_.store(false, std::memory_order_release);
#if defined(VRP_HAS_PIPEWIRE)
  // GNOME RecordVirtual: ending the PW stream removes Meta-* immediately.
  // If Chrome is still on that monitor, Mutter can assert → session logout.
  // Defer capture_->stop() to cleanupStreaming (after window evacuate/close).
  if (desk_ != DesktopEnv::Gnome) {
    if (capture_) capture_->stop();
  }
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

bool YouTubeSession::resolveGnomeVirtualRect(int prefer_w, int prefer_h, QRect* geo,
                                             QString* matched) {
  if (!geo) return false;
  // Portal EIS binds the RecordVirtual stream region (logical desktop coords).
  // Prefer this — DisplayConfig QDBus demarshal is unsafe across Mutter versions.
  if (portal_eis_ && portal_eis_->ready() && portal_eis_->region_w() > 0 &&
      portal_eis_->region_h() > 0) {
    *geo = QRect(static_cast<int>(portal_eis_->region_x()),
                 static_cast<int>(portal_eis_->region_y()),
                 static_cast<int>(portal_eis_->region_w()),
                 static_cast<int>(portal_eis_->region_h()));
    if (matched) {
      *matched = output_name_.startsWith(QLatin1String("Meta-"))
                     ? output_name_
                     : QStringLiteral("Meta-");
    }
    return true;
  }
  if (GnomeVirtualDisplay::queryVirtualRect(geo, matched) && !geo->isEmpty()) {
    return true;
  }
  if (resolveOutputRect(geo, matched) && !geo->isEmpty()) {
    QScreen* primary = QGuiApplication::primaryScreen();
    const bool looks_virtual =
        matched && (matched->startsWith(QLatin1String("Meta-")) ||
                    matched->contains(QLatin1String("virtual"), Qt::CaseInsensitive));
    if (!(primary && *geo == primary->geometry() && !looks_virtual)) {
      return true;
    }
  }
  QScreen* primary = QGuiApplication::primaryScreen();
  QScreen* best = nullptr;
  int best_score = -1;
  for (QScreen* s : QGuiApplication::screens()) {
    if (!s || s == primary) continue;
    const QRect g = s->geometry();
    if (g.isEmpty()) continue;
    int score = 1;
    const QString n = s->name().toLower();
    if (n.startsWith(QLatin1String("meta-")) || n.contains(QLatin1String("virtual")) ||
        n.contains(QLatin1String("headless"))) {
      score += 20;
    }
    if (prefer_w > 0) {
      const int logic_w =
          cfg_.virtual_scale > 1.01f
              ? static_cast<int>(std::lround(static_cast<double>(prefer_w) /
                                             static_cast<double>(cfg_.virtual_scale)))
              : prefer_w;
      if (std::abs(g.width() - prefer_w) <= 96 || std::abs(g.width() - logic_w) <= 96) score += 8;
    }
    if (prefer_h > 0) {
      const int logic_h =
          cfg_.virtual_scale > 1.01f
              ? static_cast<int>(std::lround(static_cast<double>(prefer_h) /
                                             static_cast<double>(cfg_.virtual_scale)))
              : prefer_h;
      if (std::abs(g.height() - prefer_h) <= 96 || std::abs(g.height() - logic_h) <= 96) {
        score += 8;
      }
    }
    if (score > best_score) {
      best_score = score;
      best = s;
    }
  }
  if (!best) return false;
  *geo = best->geometry();
  if (matched) *matched = best->name();
  return !geo->isEmpty();
}

void YouTubeSession::beginPlace() {
  setState(State::Placing);
  chrome_ids_before_ = KwinWindowPlacer::list_candidate_ids();
  emit logMessage(tr("Chrome: %1 existing window(s) before launch").arg(chrome_ids_before_.size()));

  QString arm_err;
  const int chrome_scr = vdisp_mgr_ ? vdisp_mgr_->screen_index() : -1;
  // Firefox: do not Force compositor fullscreen — Wayland 200% + browser FS +
  // YouTube video FS double-scales the player (Mozilla 1869724 / 1898311).
  const bool force_fs = cfg_.browser != LiveBrowser::Firefox;
  if (!KwinWindowPlacer::arm_place_on_open(output_name_, chrome_ids_before_, &arm_err,
                                           chrome_scr, force_fs)) {
    fail(tr("KWin place arm failed: %1 — abort (will not leave browser on primary)")
             .arg(arm_err.isEmpty() ? tr("(no detail)") : arm_err));
    return;
  }
  emit logMessage(tr("KWin: armed — next browser window -> %1 (#%2, fs=%3)")
                      .arg(output_name_)
                      .arg(chrome_scr)
                      .arg(force_fs ? 1 : 0));

  if (QWidget* w = qobject_cast<QWidget*>(QObject::parent())) {
    w->activateWindow();
  }

  if (!chrome_->start()) {
    KwinWindowPlacer::disarm_place_on_open();
    fail(tr("Could not start Chromium/Chrome for live capture"));
    return;
  }
  place_tries_ = 0;
  // Faster first poll than legacy 400ms — KWin arm is already sticky; this only
  // backs the X11/xid path. GNOME Meta path is quicker partly because it skips
  // wait_ready/kscreen + this place-before-portal sequencing.
  place_timer_.start(200);
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
  emit logMessage(tr("KWin: armed — next desktop window -> %1 (names %2)")
                      .arg(output_name_, desktop_want_names_.join(QLatin1Char(','))));

  if (QWidget* w = qobject_cast<QWidget*>(QObject::parent())) {
    w->activateWindow();
  }

  {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (cfg_.enable_audio) {
      env.insert(QStringLiteral("PULSE_SINK"), QStringLiteral("monaSphereYt"));
      env.insert(QStringLiteral("PIPEWIRE_NODE"), QStringLiteral("monaSphereYt"));
    }
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
    emit logMessage(tr("Desktop app: RetroArch appendconfig -> sdl2 joypad"));
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
    // Window seen → one shove + post-map Rule → portal (no fixed extra ticks).
    {
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
    installDesktopAppSessionRule();
    KwinWindowPlacer::disarm_place_on_open(/*keep_fullscreen_rule=*/true);
    emit logMessage(tr("KWin: placed desktop app on %1 (%2…)")
                        .arg(output_name_, kwin_window_id_.left(8)));
    onPlaced();
    startDesktopAppMaintain();
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
  // GNOME deferred browser: capture is already Streaming while place still runs.
  if (desk_ == DesktopEnv::Gnome && !desktopAppMode()) {
    pollPlaceGnome();
    return;
  }
  if (state_ != State::Placing) return;
  if (desktopAppMode()) {
    pollPlaceDesktopApp();
    return;
  }

  // X11 tree walk + Xrandr: first tick and every 3rd — KWin script is the fast path.
  const bool x11_tick = (place_tries_ == 0 || (place_tries_ % 3) == 0);
  if (x11_tick) {
    (void)x11_.prepare_window(place_w_, place_h_, /*hide_host=*/false, /*move=*/false);
    if (!x11_.bind_output_by_name(output_name_.toStdString(), place_w_, place_h_)) {
      x11_.set_screen_rect(place_x_, place_y_, place_w_, place_h_);
    }
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
  // GNOME managed Virtual: onPortalStarted may already be Streaming while place
  // still runs (browser was deferred until after portal VIRTUAL).
  if (state_ != State::Placing && state_ != State::Streaming) return;

  QRect geo;
  QString matched;
  if (resolveGnomeVirtualRect(place_w_, place_h_, &geo, &matched) && !geo.isEmpty()) {
    place_x_ = geo.x();
    place_y_ = geo.y();
    if (geo.width() > 0) place_win_w_ = geo.width();
    if (geo.height() > 0) place_win_h_ = geo.height();
    if (!matched.isEmpty()) output_name_ = matched;
  }
  const bool x11_tick = (place_tries_ == 0 || (place_tries_ % 3) == 0);
  if (x11_tick) {
    // X11/XWayland uses Xrandr *physical* origin (e.g. 3840,0 @ scale 2).
    // EIS logical (1920,0) must NOT overwrite that — that parked Chrome on the host.
    const bool bound = x11_.bind_output_by_name("Meta", place_w_, place_h_) ||
                       x11_.bind_output_by_name(output_name_.toStdString(), place_w_, place_h_);
    if (!bound) {
      x11_.set_screen_rect(place_x_, place_y_, place_w_, place_h_);
    }
    const int move_w = bound && x11_.xtest_w() > 0
                           ? x11_.xtest_w()
                           : (place_win_w_ > 0 ? place_win_w_ : place_w_);
    const int move_h = bound && x11_.xtest_h() > 0
                           ? x11_.xtest_h()
                           : (place_win_h_ > 0 ? place_win_h_ : place_h_);
    if (chrome_) x11_.set_prefer_pid(static_cast<long>(chrome_->process_id()));
    (void)x11_.prepare_window(move_w, move_h, /*hide_host=*/false, /*move=*/true);
  }

  if (x11_.have_window()) {
    int rx = 0;
    int ry = 0;
    const bool have_origin = x11_.root_origin(&rx, &ry);
    const int want_x = x11_.place_x();
    const int want_y = x11_.place_y();
    // Still on primary / unknown origin while Meta is to the right — keep pushing.
    const bool off_meta =
        !have_origin || (want_x > 64 && rx < want_x - 64) || (want_y > 64 && ry < want_y - 64);
    if (off_meta && place_tries_ < 50) {
      if (place_tries_ == 0 || place_tries_ % 5 == 0) {
        VRP_LOG("GNOME: Chrome still @%d,%d — moving to Meta X11 @%d,%d (try %d)", rx, ry, want_x,
                want_y, place_tries_);
        emit logMessage(tr("GNOME: Chrome still @%1,%2 — moving to Meta X11 @%3,%4")
                            .arg(rx)
                            .arg(ry)
                            .arg(want_x)
                            .arg(want_y));
      }
      // Force another move tick even between x11_tick slots.
      (void)x11_.prepare_window(x11_.xtest_w() > 0 ? x11_.xtest_w() : place_w_,
                                x11_.xtest_h() > 0 ? x11_.xtest_h() : place_h_,
                                /*hide_host=*/false, /*move=*/true);
      ++place_tries_;
      place_timer_.start(300);
      return;
    }
    VRP_LOG("GNOME: Chrome window bound on %s X11@%d,%d (xid root %d,%d)",
            output_name_.toUtf8().constData(), want_x, want_y, rx, ry);
    emit logMessage(tr("GNOME: Chrome window bound on %1 X11@%2,%3 (xid root %4,%5)")
                        .arg(output_name_)
                        .arg(want_x)
                        .arg(want_y)
                        .arg(rx)
                        .arg(ry));
    // Shell.Eval is often blocked — pin FS to Meta via _NET_WM_FULLSCREEN_MONITORS.
    (void)x11_.request_fullscreen_on_placed_output();
    {
      QString shell_detail;
      const bool shelled =
          GnomePortalDialog::shellMoveClassToMeta(QStringLiteral("monaSphereYT"), &shell_detail);
      if (shelled) {
        emit logMessage(tr("GNOME Shell: Chrome moved onto Meta (%1)").arg(shell_detail));
      } else {
        VRP_LOG("GNOME Shell: Eval unavailable (%s) — using X11 FULLSCREEN_MONITORS",
                shell_detail.toUtf8().constData());
      }
    }
    // Re-assert Meta FS a few times (Mutter can ignore the first hint).
    const int gen = gnome_evacuate_gen_;
    for (int i = 0; i < 5; ++i) {
      QTimer::singleShot(300 + i * 600, this, [this, gen, i] {
        if (gen != gnome_evacuate_gen_ || !active_.load(std::memory_order_acquire)) return;
        (void)x11_.bind_output_by_name("Meta", place_w_, place_h_);
        (void)x11_.prepare_window(x11_.xtest_w() > 0 ? x11_.xtest_w() : place_w_,
                                  x11_.xtest_h() > 0 ? x11_.xtest_h() : place_h_, false, true);
        (void)x11_.request_fullscreen_on_placed_output();
        int rx2 = 0, ry2 = 0;
        (void)x11_.root_origin(&rx2, &ry2);
        VRP_LOG("GNOME: Meta FS keep %d root@%d,%d", i + 1, rx2, ry2);
      });
    }
    onPlaced();
    return;
  }

  // Wayland Chrome has no X11 xid — try Shell.Eval move, then assume Meta place.
  if (!x11_.have_window() && chrome_ && chrome_->running() && !chrome_->force_x11() &&
      place_tries_ >= 6) {
    QString shell_detail;
    if (GnomePortalDialog::shellMoveClassToMeta(QStringLiteral("monaSphereYT"), &shell_detail)) {
      emit logMessage(tr("GNOME Shell: Wayland Chrome moved onto Meta (%1)").arg(shell_detail));
    } else if (place_tries_ == 6) {
      VRP_LOG("GNOME Shell: Eval unavailable (%s) — relying on EIS Meta focus",
              shell_detail.toUtf8().constData());
    }
    // Re-focus Meta in case Chrome mapped on host before seat settled.
    if (portal_eis_ && portal_eis_->ready() && portal_eis_->region_w() > 0) {
      const double cx =
          static_cast<double>(portal_eis_->region_x()) + portal_eis_->region_w() * 0.5;
      const double cy =
          static_cast<double>(portal_eis_->region_y()) + portal_eis_->region_h() * 0.5;
      portal_eis_->warp_desktop(cx, cy);
    }
    VRP_LOG("GNOME: Wayland Chrome running without X11 xid — assume Meta place");
    emit logMessage(tr("GNOME: Wayland Chrome on Meta (no X11 window to bind)"));
    onPlaced();
    return;
  }

  ++place_tries_;
  if (place_tries_ < 60) {
    if (place_tries_ == 1 || place_tries_ % 5 == 0) {
      VRP_LOG("GNOME: waiting for live browser… (try %d pid=%lld)", place_tries_,
              static_cast<long long>(chrome_ ? chrome_->process_id() : 0));
      emit logMessage(tr("GNOME: waiting for live browser…"));
    }
    // Chrome still alive? If the wrapper died, stream continues without place.
    if (chrome_ && !chrome_->running() && place_tries_ >= 8) {
      VRP_LOG("GNOME: Chrome process exited before window bind");
      emit logMessage(tr("GNOME: Chrome exited before a window appeared"));
      if (active_.load(std::memory_order_acquire)) {
        setState(State::Streaming);
        mark_gnome_live_ready();
        return;
      }
      fail(tr("GNOME place failed — Chrome exited before window appeared"));
      return;
    }
    place_timer_.start(350);
    return;
  }
  if (active_.load(std::memory_order_acquire)) {
    VRP_LOG("GNOME place: browser window not found — stream continues");
    emit logMessage(tr("GNOME place: browser window not found yet — stream continues"));
    setState(State::Streaming);
    // Avoid infinite spinner if place timed out while capture is live.
    mark_gnome_live_ready();
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
  const bool gnome_virt = (desk_ == DesktopEnv::Gnome && cfg_.virtual_display);
  if (gnome_virt) {
    (void)resolveGnomeVirtualRect(place_w_, place_h_, &geo, &matched);
  } else {
    (void)resolveOutputRect(&geo, &matched);
  }
  if (!geo.isEmpty()) {
    place_x_ = geo.x();
    place_y_ = geo.y();
    if (!matched.isEmpty()) output_name_ = matched;
  }
  place_w_ = cfg_.virtual_w > 0 ? cfg_.virtual_w : (geo.width() > 0 ? geo.width() : 1280);
  place_h_ = cfg_.virtual_h > 0 ? cfg_.virtual_h : (geo.height() > 0 ? geo.height() : 720);
  // place_x_/y_ = compositor *logical* (EIS). Xrandr physical stays inside x11_
  // after bind — never overwrite with logical on GNOME (scale≠1 → host park).
  const bool bound = x11_.bind_output_by_name("Meta", place_w_, place_h_) ||
                     x11_.bind_output_by_name(output_name_.toStdString(), place_w_, place_h_);
  if (!bound) {
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
  x11_.set_eis_origin(place_x_, place_y_);
  // GNOME: move onto Virtual then FS. KDE: KWin already placed (move=false).
  const bool move = gnome_virt;
  const int move_w =
      (gnome_virt && bound && x11_.xtest_w() > 0) ? x11_.xtest_w() : place_w_;
  const int move_h =
      (gnome_virt && bound && x11_.xtest_h() > 0) ? x11_.xtest_h() : place_h_;
  (void)x11_.prepare_window(move_w, move_h, /*hide_host=*/false, /*move=*/move);
  if (x11_.have_window()) {
    if (gnome_virt) {
      (void)x11_.request_fullscreen_on_placed_output();
    }
    (void)x11_.refresh_geometry();
  }
  // In-place switch may have cleared x11_; re-attach EIS before cursor/activate.
  if (EisPointer* eis = activeEis()) x11_.set_eis_inject(eis);
  x11_.set_cursor(place_w_ * 0.5f, place_h_ * 0.5f);
  if (desk_ == DesktopEnv::Kde) {
    QString act_err;
    const bool force_fs = cfg_.browser != LiveBrowser::Firefox;
    if (KwinWindowPlacer::activate_youtube_window(kwin_window_id_, &act_err, force_fs)) {
      emit logMessage(tr("KWin: activated live browser for pointer input"));
    }
  }

  // Capture already streaming (GNOME Meta first place, or Live↔Live Chrome switch).
  if (active_.load(std::memory_order_acquire)) {
    setState(State::Streaming);
    if (switch_hold_content_) {
      // Reveal on next usable frame (no fixed settle sleep).
      pending_switch_reveal_ = true;
      if (using_mutter_virtual_) {
        gnome_live_ready_ = true;
        emit logMessage(tr("Live: Chrome placed — waiting for frame to reveal"));
      } else {
        emit logMessage(tr("Live: Chrome placed — waiting for frame to reveal"));
      }
      return;
    }
    if (using_mutter_virtual_) {
      // First GNOME place: reveal when a frame arrives (same gate as KDE switch).
      gnome_live_ready_ = true;
      emit logMessage(tr("GNOME: Chrome on Meta — showing live stream"));
      mark_frame_presented();
    }
    return;
  }
  // KDE / GNOME spare MONITOR: Virtual ready — start ScreenCast now.
  beginPortal(false);
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
        emit logMessage(tr("ScreenCast: portal dialog armed -> primary"));
      }
    }
    if (desk_ == DesktopEnv::Gnome && cfg_.virtual_display) {
      // Mutter RecordVirtual already owns Meta — no portal grant UI / HUD toast.
      emit logMessage(tr("ScreenCast: GNOME VIRTUAL — manager-owned Meta (no portal toast)"));
    } else {
      emit toast(tr("Host: share ONLY “%1” (not whole desktop)").arg(output_name_));
      emit logMessage(tr("ScreenCast: pick MONITOR “%1” only — do not share the full desktop / both screens")
                          .arg(output_name_));
    }
  } else {
    emit logMessage(tr("ScreenCast: restoring previous share…"));
    // GNOME Meta path never uses portal Start — no grant toast.
  }

  auto launch = [this, interactive, after_reject] {
    if (state_ != State::Portal || !loading_.load()) return;
    QString parent;
    if (QWidget* host = qobject_cast<QWidget*>(QObject::parent())) {
      // Pin grant UI to the host on primary (Meta-* often steals new dialogs).
      if (QScreen* primary = QGuiApplication::primaryScreen()) {
        const QRect g = primary->availableGeometry();
        host->show();
        host->raise();
        host->activateWindow();
        if (host->isMinimized()) host->showNormal();
        const int wx = g.x() + std::max(0, (g.width() - host->width()) / 2);
        const int wy = g.y() + std::max(40, (g.height() - host->height()) / 4);
        host->move(wx, wy);
        if (QWindow* wh = host->windowHandle()) {
          wh->setScreen(primary);
          wh->raise();
          wh->requestActivate();
        }
      }
      host->winId();
      const QString plat = QGuiApplication::platformName();
      if (plat == QLatin1String("xcb")) {
        parent = QStringLiteral("x11:") + QString::number(static_cast<qulonglong>(host->winId()));
      } else if (plat.contains(QLatin1String("wayland"))) {
        parent = QStringLiteral("wayland:") + QString::number(static_cast<qulonglong>(host->winId()));
        emit logMessage(tr("ScreenCast: Wayland parent — gnome-shell grant UI may still "
                           "open on Meta-* (X11 evacuate cannot move it)"));
      } else {
        emit logMessage(tr("ScreenCast: parent window unavailable on platform “%1” "
                           "(dialog may appear without parenting)")
                            .arg(plat));
      }
    }
    // GNOME Meta: VirtualDisplayManager::ensureGnomeMeta already started Mutter.
    if (desk_ == DesktopEnv::Gnome && cfg_.virtual_display) {
      using_mutter_virtual_ = true;
      emit logMessage(tr("GNOME: connecting PipeWire to Meta (manager-owned RecordVirtual)"));
      ScreencastPortal::Result r;
      r.node_id = gnome_node_id_;
      r.pw_fd = -1;
      r.eis_fd = gnome_eis_fd_;
      r.mapping_id = gnome_mapping_id_;
      gnome_eis_fd_ = -1;  // onPortalStarted / EisPointer owns fd
      armPortalWatchdog(false);
      QTimer::singleShot(0, this, [this, r] { onPortalStarted(r); });
      return;
    }
    using_mutter_virtual_ = false;
    const auto mode = (desk_ == DesktopEnv::Gnome)
                          ? ScreencastPortal::SessionMode::RemoteDesktopWithVideo
                          : ScreencastPortal::SessionMode::VideoOnly;
    const auto kind = ScreencastPortal::SourceKind::Monitor;
    portal_->start(parent, restore_token_, /*persist_mode*/ 2, kind, mode);
    armPortalWatchdog(!restore_token_.isEmpty() && !after_reject);
  };

  // Portal dialog arm (if any) already returned — Start immediately.
  launch();
}

void YouTubeSession::armPortalWatchdog(bool had_token) {
  const int gen = ++portal_gen_;
  // Interactive: picker should appear within a few seconds after Start.
  // Keep shorter than before so a missed Request Response cannot leave Chrome up.
  const int wait_ms = had_token ? 12000 : 25000;
  QTimer::singleShot(wait_ms, this, [this, gen, had_token] {
    if (gen != portal_gen_) return;
    if (active_.load() || state_ != State::Portal) return;
    const bool mutter_busy = vdisp_mgr_ && vdisp_mgr_->mutter() && vdisp_mgr_->mutter()->busy();
    const bool portal_busy = portal_ && portal_->busy();
    if (!portal_busy && !mutter_busy) {
      if (loading_.load() && !active_.load()) {
        fail(tr("ScreenCast: no stream after wait"));
      }
      return;
    }
    if (mutter_busy) {
      if (vdisp_mgr_) vdisp_mgr_->destroy();
      fail(tr("Mutter virtual MONITOR timed out"));
      return;
    }
    if (had_token && !portal_retry_used_) {
      portal_->stop();
      portal_retry_used_ = true;
      ++portal_restore_retries_;
      restore_token_.clear();
      if (vdisp_mgr_) {
        const int span = vdisp_mgr_->active_span() > 0
                             ? vdisp_mgr_->active_span()
                             : VirtualDisplayManager::spanFromName(output_name_);
        vdisp_mgr_->clearToken(span);
      } else {
        emit restoreTokenChanged(QString());
      }
      emit logMessage(tr("ScreenCast restore hung — opening host picker (stale token cleared)"));
      emit toast(tr("Host: pick Virtual-monaSphere"));
      QTimer::singleShot(0, this, [this] {
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

void YouTubeSession::launchBrowserAfterGnomeVirtual(int prefer_w, int prefer_h) {
  // Call after PipeWire capture has started — Mutter creates Meta-* only then.
  // Multiple settle-timers may fire; only one launch.
  if (chrome_ && chrome_->running() &&
      (state_ == State::Placing || x11_.have_window())) {
    return;
  }
  if (prefer_w <= 0) prefer_w = cfg_.virtual_w > 0 ? cfg_.virtual_w : place_w_;
  if (prefer_h <= 0) prefer_h = cfg_.virtual_h > 0 ? cfg_.virtual_h : place_h_;
  place_w_ = prefer_w;
  place_h_ = prefer_h;

  QRect geo;
  QString matched;
  bool found = false;
  for (int attempt = 0; attempt < 50; ++attempt) {
    if (!active_.load(std::memory_order_acquire) && state_ != State::Streaming &&
        state_ != State::Placing) {
      return;
    }
    if (resolveGnomeVirtualRect(prefer_w, prefer_h, &geo, &matched) && !geo.isEmpty()) {
      found = true;
      break;
    }
    if (attempt == 0 || attempt % 5 == 0) {
      emit logMessage(tr("GNOME Virtual: waiting for EIS/Meta-* rect (%1×%2)…")
                          .arg(prefer_w)
                          .arg(prefer_h));
    }
    QEventLoop loop;
    QTimer::singleShot(100, &loop, &QEventLoop::quit);
    loop.exec();
    if (state_ == State::Stopping || state_ == State::Idle || state_ == State::Failed) return;
  }

  // Window size on the desktop = logical EIS/QScreen rect. Cursor clamp stays
  // prefer_w×prefer_h (PipeWire / Virtual stream pixels).
  place_win_w_ = place_w_;
  place_win_h_ = place_h_;
  if (found) {
    place_x_ = geo.x();
    place_y_ = geo.y();
    if (!matched.isEmpty()) output_name_ = matched;
    if (geo.width() > 0) place_win_w_ = geo.width();
    if (geo.height() > 0) place_win_h_ = geo.height();
  } else {
    place_x_ = 0;
    place_y_ = 0;
    emit logMessage(tr("GNOME Virtual: EIS/QScreen rect missing — launching windowed "
                       "(will retry place while streaming)"));
  }

  emit logMessage(tr("GNOME Virtual ready: %1 (stream %2×%3, window %4×%5 @%6,%7) — "
                     "launching browser")
                      .arg(output_name_)
                      .arg(place_w_)
                      .arg(place_h_)
                      .arg(place_win_w_)
                      .arg(place_win_h_)
                      .arg(place_x_)
                      .arg(place_y_));

  // Platform Meta: Wayland Chrome follows the seat monitor. X11 ozone often
  // gets an xid on Meta CRTC that never composites into RecordVirtual.
  const bool wayland_chrome = using_mutter_virtual_;
  chrome_->set_force_x11(!wayland_chrome);
  configureLiveBrowserLaunch();
  chrome_->set_start_minimized(false);
  chrome_->set_start_fullscreen(wayland_chrome);

  (void)x11_.bind_output_by_name("Meta", place_w_, place_h_);
  x11_.set_eis_origin(place_x_, place_y_);

  // ConnectToEIS ran before Meta existed — rebind libei region now (fixes cursor
  // stuck until a later Meta damage "glitch" refreshed DEVICE_ADDED).
  if (portal_eis_ && portal_eis_->ready()) {
    bool rebound = false;
    for (int i = 0; i < 30; ++i) {
      if (portal_eis_->repick_regions() && portal_eis_->region_w() > 0 &&
          portal_eis_->region_h() > 0) {
        rebound = true;
        break;
      }
      QEventLoop loop;
      QTimer::singleShot(100, &loop, &QEventLoop::quit);
      loop.exec();
      if (state_ == State::Stopping || state_ == State::Idle || state_ == State::Failed) return;
    }
    if (rebound) {
      place_x_ = static_cast<int>(portal_eis_->region_x());
      place_y_ = static_cast<int>(portal_eis_->region_y());
      x11_.set_eis_origin(place_x_, place_y_);
      portal_eis_->set_buttons_enabled(true);
      emit logMessage(tr("GNOME: EIS rebound to Meta %1×%2 @%3,%4")
                          .arg(portal_eis_->region_w())
                          .arg(portal_eis_->region_h())
                          .arg(place_x_)
                          .arg(place_y_));
    } else {
      emit logMessage(tr("GNOME: EIS Meta region not ready — pointer may stay on primary"));
    }
  }

  // Focus Meta before map. Mutter places new Wayland windows on the pointer /
  // focused monitor — EIS warp to Xrandr physical coords (e.g. 4800) misses the
  // logical EIS desktop (Meta often 960x540+1920). Click wallpaper so Meta is
  // current even while monasphere stays focused on the host output.
  if (portal_eis_ && portal_eis_->ready() && portal_eis_->region_w() > 0 &&
      portal_eis_->region_h() > 0) {
    const double cx =
        static_cast<double>(portal_eis_->region_x()) + portal_eis_->region_w() * 0.5;
    const double cy =
        static_cast<double>(portal_eis_->region_y()) + portal_eis_->region_h() * 0.5;
    portal_eis_->warp_desktop(cx, cy);
    portal_eis_->click_left_forced();
    VRP_LOG("GNOME: EIS focus Meta @%.0f,%.0f (region %ux%u+%u+%u) before Chrome", cx, cy,
            portal_eis_->region_w(), portal_eis_->region_h(), portal_eis_->region_x(),
            portal_eis_->region_y());
  } else if (portal_eis_ && portal_eis_->ready()) {
    const int ww = place_win_w_ > 0 ? place_win_w_ : place_w_;
    const int wh = place_win_h_ > 0 ? place_win_h_ : place_h_;
    portal_eis_->warp_desktop(place_x_ + ww * 0.5, place_y_ + wh * 0.5);
    portal_eis_->click_left_forced();
    VRP_LOG("GNOME: EIS warped (no region) @%d,%d before Chrome", place_x_ + ww / 2,
            place_y_ + wh / 2);
  }
  // Host QCursor is a second seat on X11/XWayland — keep it off primary too.
  if (x11_.bind_output_by_name("Meta", place_w_, place_h_)) {
    const int hx = x11_.place_x() + std::max(1, x11_.xtest_w()) / 2;
    const int hy = x11_.place_y() + std::max(1, x11_.xtest_h()) / 2;
    QCursor::setPos(hx, hy);
    VRP_LOG("GNOME: QCursor → Meta physical @%d,%d", hx, hy);
  }
  {
    QEventLoop loop;
    QTimer::singleShot(120, &loop, &QEventLoop::quit);
    loop.exec();
  }

  if (wayland_chrome) {
    chrome_->clear_window_position();
    chrome_->set_window_size(place_win_w_ > 0 ? place_win_w_ : place_w_,
                             place_win_h_ > 0 ? place_win_h_ : place_h_);
    VRP_LOG("GNOME: Wayland Chrome on platform Meta (fullscreen, seat on Meta)");
  } else {
    int launch_x = place_x_;
    int launch_y = place_y_;
    int launch_w = place_win_w_ > 0 ? place_win_w_ : place_w_;
    int launch_h = place_win_h_ > 0 ? place_win_h_ : place_h_;
    if (x11_.bind_output_by_name("Meta", place_w_, place_h_) ||
        x11_.bind_output_by_name(output_name_.toStdString(), place_w_, place_h_)) {
      launch_x = x11_.place_x();
      launch_y = x11_.place_y();
      if (x11_.xtest_w() > 0) launch_w = x11_.xtest_w();
      if (x11_.xtest_h() > 0) launch_h = x11_.xtest_h();
    } else {
      x11_.set_screen_rect(place_x_, place_y_, place_w_, place_h_);
    }
    chrome_->set_window_position(launch_x, launch_y);
    chrome_->set_window_size(launch_w, launch_h);
  }

  x11_.destroy_damage_window();

  setState(State::Placing);
  chrome_ids_before_.clear();
  kwin_window_id_.clear();
  if (!chrome_->start()) {
    fail(tr("Could not start Chromium/Chrome for live capture"));
    return;
  }
  const qint64 cpid = chrome_->process_id();
  x11_.set_prefer_pid(static_cast<long>(cpid));
  place_tries_ = 0;
  place_timer_.start(200);
  VRP_LOG("GNOME: placing Chrome on Meta-* (pid %lld state=Placing wayland=%d)",
          static_cast<long long>(cpid), wayland_chrome ? 1 : 0);
  emit logMessage(tr("GNOME: placing Chrome on Meta-* (pid %1)").arg(cpid));
  // Keep spinner until pollPlaceGnome/onPlaced — do not reveal Meta wallpaper.
  x11_.set_cursor(place_w_ * 0.5f, place_h_ * 0.5f);
}

void YouTubeSession::onPortalStarted(const ScreencastPortal::Result& r) {
  if (state_ != State::Portal) {
    if (r.pw_fd >= 0) ::close(r.pw_fd);
    if (r.eis_fd >= 0) ::close(r.eis_fd);
    return;
  }
  KwinWindowPlacer::disarm_portal_dialog_on_primary();
  ++portal_gen_;

  // GNOME RecordVirtual: Meta-* appears only after PipeWire negotiates size.
  // Do NOT launch the browser before capture_->start().
  const bool gnome_launch_after_pw = gnome_defer_browser_;
  if (gnome_defer_browser_) gnome_defer_browser_ = false;

  // Stream must be the chosen MONITOR alone. A dual-desktop stream (e.g. 3840×1080)
  // leaves the cursor stuck on the host half. Skip for portal VIRTUAL (size is negotiated).
  const int vw = cfg_.virtual_w > 0 ? cfg_.virtual_w : place_w_;
  const int vh = cfg_.virtual_h > 0 ? cfg_.virtual_h : place_h_;
  const bool virtual_src =
      using_mutter_virtual_ ||
      (portal_ && portal_->active_kind() == ScreencastPortal::SourceKind::Virtual);
  if (!virtual_src && r.width > 0 &&
      r.height > 0 && vw > 0 && vh > 0 && (r.width > vw + 64 || r.height > vh + 64)) {
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
  // Portal OpenPipeWireRemote → pw_fd>=0. Mutter in-session → pw_fd=-1 + node_id
  // (default user PipeWire socket).
  if (r.pw_fd < 0 && r.node_id == 0) {
    if (r.eis_fd >= 0) ::close(r.eis_fd);
    fail(tr("ScreenCast: no PipeWire fd"));
    return;
  }
  if (!capture_) capture_ = std::make_unique<PipewireCapture>();
  // Prefer configured Virtual-* pixels so KWin does not resize the stream down
  // to a hardcoded 1080p default (or portal logical size under scale≠1) during
  // PipeWire format negotiation.
  const int prefer_w = cfg_.virtual_w > 0 ? cfg_.virtual_w : r.width;
  const int prefer_h = cfg_.virtual_h > 0 ? cfg_.virtual_h : r.height;
  // GNOME Meta-* / RecordVirtual: negotiate DMA-BUF first (same as KDE); SHM is
  // the EnumFormat fallback. Surface hitching matched SHM once pool/filter work
  // landed — no longer force prefer_shm.
  // Mutter-only unpainted-BO skip — KDE DMA often has chunk size=0 on valid frames.
  const bool gnome_meta = desk_ == DesktopEnv::Gnome && cfg_.virtual_display;
  capture_->set_skip_unpainted_bo(gnome_meta);
  if (r.pw_fd < 0) {
    VRP_LOG("pipewire: Mutter node %u via default socket (no portal fd)", r.node_id);
  }
  if (gnome_meta) {
    emit logMessage(cfg_.drm_modifiers.empty()
                        ? tr("PipeWire: GNOME VIRTUAL — DMA-BUF + SHM fallback (no consumer mods yet)")
                        : tr("PipeWire: GNOME VIRTUAL — DMA-BUF preferred (%1 mods), SHM fallback")
                              .arg(cfg_.drm_modifiers.size()));
  }
  if (!capture_->start(r.pw_fd, r.node_id, r.pipewire_serial, cfg_.drm_modifiers, prefer_w, prefer_h,
                       /*prefer_shm=*/false)) {
    const QString err = QString::fromStdString(capture_->last_error());
    if (r.pw_fd >= 0) ::close(r.pw_fd);
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
  if (desk_ == DesktopEnv::Gnome && cfg_.virtual_display) {
    // Keep RecordVirtual from idling (Hidden cursor → no damage → pool holds).
    if (!meta_paint_timer_.isActive()) meta_paint_timer_.start();
    (void)x11_.force_output_damage(/*quiet=*/false);
  }

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
      // GNOME VIRTUAL: Start dialog often lands on Meta-* (X11 evacuate may miss
      // shell surfaces). Hold pad clicks until evacuate settles.
      if (desk_ == DesktopEnv::Gnome && cfg_.virtual_display) {
        portal_eis_->set_buttons_enabled(false);
      } else {
        portal_eis_->set_buttons_enabled(true);
      }
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

  // Re-focus once the event loop turns (ScreenCast just stole activation).
  QTimer::singleShot(0, this, [this] {
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

  if (desk_ == DesktopEnv::Gnome && cfg_.virtual_display) {
    armGnomePortalEvacuate();
    (void)evacuateGnomePortalDialogsOnce();
    // Early wallpaper paint until Chrome maps (no EIS click — grant UI may be on Meta).
    const int gen = gnome_evacuate_gen_;
    for (int i = 0; i < 3; ++i) {
      QTimer::singleShot(200 + i * 400, this, [this, gen, i] {
        if (gen != gnome_evacuate_gen_ || !active_.load(std::memory_order_acquire)) return;
        (void)x11_.bind_output_by_name("Meta", place_w_, place_h_);
        const bool damaged = x11_.force_output_damage();
        VRP_LOG("GNOME: Meta paint nudge %d damage=%d", i + 1, damaged ? 1 : 0);
      });
    }
  }

  if (gnome_launch_after_pw) {
    if (gnome_capture_only_) {
      emit logMessage(tr("GNOME: capture-only (Meta wallpaper + soft cursor) — "
                         "browser deferred until portal dialogs stay off Meta-*"));
      mark_gnome_live_ready();
    } else {
      // RecordVirtual creates Meta-* during/after PW. Wait until portal buttons
      // are enabled (grant UI evacuated) so Chrome does not map under the dialog.
      const int bw = prefer_w;
      const int bh = prefer_h;
      const int gen = gnome_evacuate_gen_;
      for (int attempt = 0; attempt < 24; ++attempt) {
        QTimer::singleShot(600 + attempt * 400, this, [this, bw, bh, gen, attempt] {
          if (gen != gnome_evacuate_gen_ || !active_.load(std::memory_order_acquire)) return;
          if (chrome_ && chrome_->running()) return;  // already launched by an earlier tick
          (void)evacuateGnomePortalDialogsOnce();
          // Mutter RD: wait for EIS (host remote-control grant) before Chrome.
          // Without EIS, Wayland Chrome cannot be steered onto Meta reliably.
          const bool eis_ready = portal_eis_ && portal_eis_->ready();
          const bool buttons_ok = !portal_eis_ || portal_eis_->buttons_enabled();
          if (using_mutter_virtual_) {
            if (!eis_ready && attempt < 22) return;
          } else if (!buttons_ok && attempt < 20) {
            return;
          }
          VRP_LOG("GNOME: launching Chrome after settle (attempt %d eis=%d buttons=%d)",
                  attempt + 1, eis_ready ? 1 : 0, buttons_ok ? 1 : 0);
          launchBrowserAfterGnomeVirtual(bw, bh);
        });
      }
    }
  }
#else
  if (r.eis_fd >= 0) ::close(r.eis_fd);
  (void)r;
  (void)gnome_launch_after_pw;
  fail(tr("Built without PipeWire"));
#endif
}

int YouTubeSession::evacuateGnomePortalDialogsOnce() {
  if (desk_ != DesktopEnv::Gnome) return 0;
  int vx = 0, vy = 0, vw = 0, vh = 0;
  if (x11_.bind_output_by_name("Meta", place_w_, place_h_)) {
    vx = x11_.place_x();
    vy = x11_.place_y();
    vw = x11_.xtest_w();
    vh = x11_.xtest_h();
  }
  // vw/vh 0 → helper auto-detects Meta via Xrandr.
  QString detail;
  const int n = GnomePortalDialog::evacuateToPrimary(vx, vy, vw, vh, &detail);
  if (n > 0) {
    emit logMessage(tr("GNOME: moved portal dialog(s) to primary (%1)").arg(detail));
  }
  return n;
}

void YouTubeSession::armGnomePortalEvacuate() {
  if (desk_ != DesktopEnv::Gnome || !cfg_.virtual_display) return;
  const int gen = gnome_evacuate_gen_;
  // Start may show the grant UI before or after Meta appears — poll ~16s.
  for (int i = 0; i < 40; ++i) {
    QTimer::singleShot(150 + i * 400, this, [this, gen, i] {
      if (gen != gnome_evacuate_gen_) return;
      if (state_ != State::Portal && state_ != State::Streaming && state_ != State::Placing &&
          state_ != State::Placed) {
        return;
      }
      const int moved = evacuateGnomePortalDialogsOnce();
      // After ~2.5s of polling with no further moves, allow pad clicks.
      if (portal_eis_ && !portal_eis_->buttons_enabled() && i >= 6 && moved == 0) {
        portal_eis_->set_buttons_enabled(true);
        emit logMessage(tr("GNOME: pointer buttons enabled (portal dialogs evacuated)"));
      }
    });
  }
  // Hard enable after 8s even if classify misses Wayland-only shell dialogs.
  QTimer::singleShot(8000, this, [this, gen] {
    if (gen != gnome_evacuate_gen_) return;
    if (portal_eis_ && !portal_eis_->buttons_enabled()) {
      portal_eis_->set_buttons_enabled(true);
      emit logMessage(tr("GNOME: pointer buttons enabled (timeout)"));
    }
  });
}

void YouTubeSession::onPortalFailed(const QString& e) {
  if (state_ != State::Portal) return;
  // Spurious overlapping start() — in-flight portal must keep running.
  if (e.contains(QStringLiteral("already starting"), Qt::CaseInsensitive) ||
      e.contains(QStringLiteral("already in progress"), Qt::CaseInsensitive)) {
    VRP_LOG("live: ignoring overlapping ScreenCast start");
    return;
  }
  // Stale Request callbacks after stop/retry (empty session_path, parallel Start).
  if (e.contains(QStringLiteral("Sources not selected"), Qt::CaseInsensitive) ||
      e.contains(QStringLiteral("Invalid object path"), Qt::CaseInsensitive) ||
      e.contains(QStringLiteral("Marshalling failed"), Qt::CaseInsensitive)) {
    if (portal_retry_used_ || portal_restore_retries_ > 0 || (portal_ && portal_->busy())) {
      VRP_LOG("live: ignoring stale ScreenCast failure: %s", e.toUtf8().constData());
      return;
    }
  }
  KwinWindowPlacer::disarm_portal_dialog_on_primary();
  // Cancel restore/hung watchdogs before scheduling another Start.
  ++portal_gen_;

  const bool restore_rejected =
      e.contains(QStringLiteral("restore rejected"), Qt::CaseInsensitive) ||
      (portal_ && portal_->used_restore_token() &&
       e.contains(QStringLiteral("cancelled"), Qt::CaseInsensitive));

  // Stale / wrong-DE / revoked token never succeeds on silent retry — drop it and
  // open the host picker once so Live can continue instead of failing out.
  if (restore_rejected && loading_.load() && !portal_retry_used_) {
    if (portal_) portal_->stop();
    portal_retry_used_ = true;
    ++portal_restore_retries_;
    restore_token_.clear();
    if (vdisp_mgr_) {
      const int span = vdisp_mgr_->active_span() > 0
                           ? vdisp_mgr_->active_span()
                           : VirtualDisplayManager::spanFromName(output_name_);
      vdisp_mgr_->clearToken(span);
    } else {
      emit restoreTokenChanged(QString());
    }
    emit logMessage(tr("ScreenCast restore rejected — opening host picker "
                       "(stale token cleared; pick Virtual-monaSphere and Remember)"));
    emit toast(tr("Host: pick Virtual-monaSphere"));
    QTimer::singleShot(0, this, [this] {
      if (!loading_.load() || active_.load()) return;
      if (portal_ && portal_->busy()) portal_->stop();
      setState(State::Placed);
      beginPortal(true);
    });
    return;
  }

  fail(tr("ScreenCast failed: %1").arg(e));
}

}  // namespace vrp
