#include "youtube_session.hpp"

#include "capture/kwin_window_placer.hpp"
#include "common.hpp"

#include <QCoreApplication>
#include <QCursor>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QScreen>
#include <QThread>
#include <QWidget>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <unistd.h>

namespace vrp {

YouTubeSession::YouTubeSession(QObject* parent) : QObject(parent) {
  desk_ = detect_desktop_env();
  chrome_ = new ChromiumYtLauncher(this);
  portal_ = new ScreencastPortal(this);
  kde_eis_ = new KdeEisPointer();
  portal_eis_ = new EisPointer();
  vdisp_ = new KdeVirtualDisplay(this);
#if defined(VRP_HAS_PIPEWIRE)
  capture_ = std::make_unique<PipewireCapture>();
#endif
  place_timer_.setSingleShot(true);
  connect(&place_timer_, &QTimer::timeout, this, &YouTubeSession::pollPlace);

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
  connect(vdisp_, &KdeVirtualDisplay::failed, this, [this](const QString& e) {
    if (e.contains(QStringLiteral("クラッシュ"), Qt::CaseInsensitive) ||
        e.contains(QStringLiteral("crashed"), Qt::CaseInsensitive)) {
      return;
    }
    emit logMessage(tr("Virtual display: %1").arg(e));
  });
}

YouTubeSession::~YouTubeSession() {
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
  destroyVirtualDisplay();
  loading_.store(false, std::memory_order_release);
  active_.store(false, std::memory_order_release);
  audio_wanted_.store(false, std::memory_order_release);
  setState(State::Failed);
  emit failed(err);
  emit logMessage(err);
  setState(State::Idle);
}

void YouTubeSession::cleanupStreaming() {
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
      if (!closed) closed = KwinWindowPlacer::close_youtube_window(&err);
      // Place never got an id (class often google-chrome under default-profile
      // singleton) — close windows that appeared after our pre-launch snapshot.
      if (!closed && kwin_window_id_.isEmpty()) {
        closed = KwinWindowPlacer::close_chrome_except(chrome_ids_before_, &err);
      }
    }
    // After compositor close the X11 xid is often already invalid. Safe close
    // traps BadWindow; never leave a stale id for a later inject/close.
    if (!closed && x11_.have_window()) closed = x11_.close_window();
    x11_.clear();
    emit logMessage(closed ? tr("Live browser window closed")
                           : tr("Live browser window close: %1")
                                 .arg(err.isEmpty() ? tr("not found") : err));
  }
  kwin_window_id_.clear();
  chrome_ids_before_.clear();
  if (chrome_) chrome_->stop();
}

void YouTubeSession::destroyVirtualDisplay() {
  if (!vdisp_) return;
  if (vdisp_->owned() || vdisp_->running()) {
    vdisp_->stop();
    emit logMessage(tr("Virtual display destroyed"));
  }
  (void)KdeVirtualDisplay::killOrphans(QStringLiteral("monaSphere"));
  (void)KdeVirtualDisplay::killOrphans(QStringLiteral("monaSphereYT"));
}

void YouTubeSession::mark_frame_presented() {
  if (loading_.load(std::memory_order_acquire)) {
    loading_.store(false, std::memory_order_release);
  }
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
  restore_token_ = cfg.restore_token;
  // Incoming conf token: assume it matches current cfg size until Start refreshes grant.
  if (!restore_token_.isEmpty() && (token_grant_w_ <= 0 || token_grant_h_ <= 0)) {
    token_grant_w_ = cfg_.virtual_w;
    token_grant_h_ = cfg_.virtual_h;
  }
  if (restore_token_.isEmpty()) {
    token_grant_w_ = token_grant_h_ = 0;
  }
  output_name_ = cfg.output_hint.isEmpty() ? QStringLiteral("Virtual-monaSphere") : cfg.output_hint;
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

  if (capture_) capture_->stop();
  have_saved_cursor_ = false;  // new session — do not restore mid-restart
  stopEis();
  if (portal_) portal_->stop();
  if (chrome_) chrome_->stop();

  desk_ = detect_desktop_env();
  emit logMessage(tr("Live desktop: %1")
                      .arg(QString::fromLatin1(desktop_env_name(desk_))));

  if (cfg_.virtual_display) {
    if (desk_ == DesktopEnv::Gnome) {
      fail(tr("Managed virtual display (live_virtual=1) is KDE-only. "
              "On GNOME use a spare MONITOR or set live_capture=window / live_virtual=0."));
      return;
    }
    if (!ensureVirtualDisplay()) {
      fail(tr("Live setup aborted (virtual display)"));
      return;
    }
    setState(State::VDispReady);
  }

  QRect geo;
  QString matched;
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

  chrome_->set_url(cfg_.url);
  chrome_->set_device_scale_factor(cfg_.chrome_scale);
  chrome_->set_use_default_profile(cfg_.use_default_chrome_profile);
  chrome_->set_window_size(place_w_, place_h_);
  chrome_->clear_window_position();
  // Borderless fullscreen on the target MONITOR (KWin also sets fullScreen after place).
  chrome_->set_start_fullscreen(true);
  chrome_->set_start_minimized(cfg_.virtual_display && desk_ == DesktopEnv::Kde);
  // XTest must use Xrandr/X11 geometry (not kscreen logical) when scale≠1.
  if (!x11_.bind_output_by_name(output_name_.toStdString(), place_w_, place_h_)) {
    x11_.set_screen_rect(place_x_, place_y_, place_w_, place_h_);
  }
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
  if (state_ == State::Idle && !loading_.load() && !active_.load() && !capture_alive &&
      !chrome_alive && kwin_window_id_.isEmpty()) {
    return;
  }
  place_timer_.stop();
  ++portal_gen_;
  setState(State::Stopping);
  loading_.store(false, std::memory_order_release);
  active_.store(false, std::memory_order_release);
  audio_wanted_.store(false, std::memory_order_release);
  cleanupStreaming();
  destroyVirtualDisplay();
  setState(State::Idle);
  emit stopped();
  emit logMessage(tr("Live capture stopped"));
}

void YouTubeSession::shutdown() {
  stop();
}

bool YouTubeSession::ensureVirtualDisplay() {
  vdisp_->set_name(QStringLiteral("monaSphere"));
  vdisp_->set_resolution(cfg_.virtual_w, cfg_.virtual_h);
  const float want_scale =
      (cfg_.virtual_scale >= 1.f && cfg_.virtual_scale <= 3.f) ? cfg_.virtual_scale : 2.f;
  vdisp_->set_scale(want_scale);
  output_name_ = vdisp_->output_name();

  auto mode_ok = [&](int mw, int mh, float sc) {
    return std::abs(mw - cfg_.virtual_w) <= 16 && std::abs(mh - cfg_.virtual_h) <= 16 &&
           std::abs(sc - want_scale) <= 0.15f;
  };

  int cur_w = 0, cur_h = 0;
  float cur_sc = 0.f;
  const bool have_mode = KdeVirtualDisplay::queryOutputMode(output_name_, &cur_w, &cur_h, &cur_sc);
  const bool mode_good = have_mode && mode_ok(cur_w, cur_h, cur_sc);
  const bool size_cfg_ok = vdisp_->width() == cfg_.virtual_w &&
                           vdisp_->height() == cfg_.virtual_h &&
                           std::abs(vdisp_->scale() - want_scale) <= 0.05f;

  // Prefer our owned krfb process at the configured size. After ScreenCast stop,
  // kscreen sometimes reports a stale mode (e.g. 1920×1080) even though the
  // Virtual-* is still 1280×720 — recreating then wiped the restore_token and
  // forced a second host picker during setup verify.
  const bool trust_owned = vdisp_->running() && size_cfg_ok;
  if (trust_owned) {
    if (!mode_good && have_mode) {
      emit logMessage(tr("Virtual display kscreen says %1×%2 — keeping owned %3×%4 (token kept)")
                          .arg(cur_w)
                          .arg(cur_h)
                          .arg(cfg_.virtual_w)
                          .arg(cfg_.virtual_h));
    } else {
      emit logMessage(tr("Reusing virtual display %1 %2×%3 @%4×")
                          .arg(output_name_)
                          .arg(cfg_.virtual_w)
                          .arg(cfg_.virtual_h)
                          .arg(QString::number(static_cast<double>(want_scale), 'f', 2)));
    }
  } else if (mode_good && size_cfg_ok) {
    emit logMessage(tr("Reusing virtual display %1 %2×%3 @%4×")
                        .arg(output_name_)
                        .arg(cur_w)
                        .arg(cur_h)
                        .arg(QString::number(static_cast<double>(cur_sc), 'f', 2)));
  } else {
    if (have_mode && !mode_ok(cur_w, cur_h, cur_sc)) {
      emit logMessage(tr("Virtual display stale mode %1×%2 @%3 — recreating %4×%5 @%6×")
                          .arg(cur_w)
                          .arg(cur_h)
                          .arg(QString::number(static_cast<double>(cur_sc), 'f', 2))
                          .arg(cfg_.virtual_w)
                          .arg(cfg_.virtual_h)
                          .arg(QString::number(static_cast<double>(want_scale), 'f', 2)));
    }
    // Clear token only when the configured capture size changed since grant.
    // Recreating to the same cfg size must not wipe a just-saved restore_token.
    const bool cfg_differs_from_grant =
        token_grant_w_ > 0 && token_grant_h_ > 0 &&
        (std::abs(cfg_.virtual_w - token_grant_w_) > 16 ||
         std::abs(cfg_.virtual_h - token_grant_h_) > 16);
    if (cfg_differs_from_grant && !restore_token_.isEmpty()) {
      restore_token_.clear();
      token_grant_w_ = token_grant_h_ = 0;
      emit restoreTokenChanged(QString());
      emit logMessage(tr("Cleared ScreenCast restore token (virtual display resolution changed)"));
    }
    vdisp_->stop();
    (void)KdeVirtualDisplay::killOrphans(QStringLiteral("monaSphere"));
    (void)KdeVirtualDisplay::killOrphans(QStringLiteral("monaSphereYT"));
    QElapsedTimer gone;
    gone.start();
    while (gone.elapsed() < 4000 &&
           (KdeVirtualDisplay::outputExists(QStringLiteral("monaSphere")) ||
            KdeVirtualDisplay::outputExists(QStringLiteral("monaSphereYT")))) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
      QThread::msleep(100);
    }
    if (!vdisp_->start()) return false;
    emit logMessage(tr("Virtual display starting: %1 (%2×%3 @%4×)")
                        .arg(vdisp_->output_name())
                        .arg(cfg_.virtual_w)
                        .arg(cfg_.virtual_h)
                        .arg(QString::number(static_cast<double>(want_scale), 'f', 2)));
  }

  int place_x = 0, place_y = 0;
  if (QScreen* primary = QGuiApplication::primaryScreen()) {
    place_x = primary->geometry().right() + 1;
    place_y = primary->geometry().top();
  }

  QElapsedTimer wait;
  wait.start();
  QString matched;
  while (wait.elapsed() < 10000) {
    (void)KdeVirtualDisplay::enableOutput(output_name_, place_x, place_y, want_scale);
    int mw = 0, mh = 0;
    float sc = 1.f;
    QRect r;
    QString m2;
    const bool have_rect =
        KdeVirtualDisplay::queryOutputRect(output_name_, &r, &m2) && !r.isEmpty();
    const bool have_good_mode =
        KdeVirtualDisplay::queryOutputMode(output_name_, &mw, &mh, &sc, &matched) &&
        mode_ok(mw, mh, sc);

    if (have_good_mode && have_rect) {
      if (!matched.isEmpty()) output_name_ = matched;
      else if (!m2.isEmpty()) output_name_ = m2;
      emit logMessage(tr("Virtual display ready: %1 mode %2×%3")
                          .arg(output_name_)
                          .arg(mw)
                          .arg(mh));
      return true;
    }

    // Owned krfb at cfg size: do not fail on stale kscreen mode — only need the
    // output present so place/portal can target Virtual-monaSphere.
    if (trust_owned && (have_rect || wait.elapsed() > 2000)) {
      if (!m2.isEmpty()) output_name_ = m2;
      else if (!matched.isEmpty()) output_name_ = matched;
      emit logMessage(tr("Virtual display ready: %1 owned %2×%3 (kscreen mode ignored)")
                          .arg(output_name_)
                          .arg(cfg_.virtual_w)
                          .arg(cfg_.virtual_h));
      return true;
    }

    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QThread::msleep(150);
  }
  emit logMessage(tr("Virtual display “%1” did not reach %2×%3 @1.0")
                      .arg(output_name_)
                      .arg(cfg_.virtual_w)
                      .arg(cfg_.virtual_h));
  return false;
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
  if (!KwinWindowPlacer::arm_place_on_open(output_name_, chrome_ids_before_, &arm_err)) {
    fail(tr("KWin place arm failed: %1 — abort (will not leave Chrome on primary)")
             .arg(arm_err.isEmpty() ? tr("(no detail)") : arm_err));
    return;
  }
  emit logMessage(tr("KWin: armed — next Chrome window → %1").arg(output_name_));

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

void YouTubeSession::pollPlace() {
  if (state_ != State::Placing) return;
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

  // Brief settle for compositor before portal maps.
  QTimer::singleShot(300, this, [this] {
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
    token_grant_w_ = token_grant_h_ = 0;
    emit restoreTokenChanged(QString());
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
    token_grant_w_ = cfg_.virtual_w;
    token_grant_h_ = cfg_.virtual_h;
    emit restoreTokenChanged(restore_token_);
    emit logMessage(tr("Live ScreenCast token saved (%1 chars)").arg(restore_token_.size()));
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
  if (!capture_->start(r.pw_fd, r.node_id, r.pipewire_serial)) {
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
      emit logMessage(tr("Pointer: portal EIS (%1) mapping_id=%2")
                          .arg(portal_eis_->region_local() ? tr("region-local")
                                                          : tr("desktop-global"))
                          .arg(r.mapping_id.isEmpty() ? tr("(none)") : r.mapping_id));
    } else {
      emit logMessage(tr("Pointer: portal EIS failed (%1) — XTest fallback")
                          .arg(QString::fromStdString(portal_eis_->last_error())));
    }
  } else {
    if (r.eis_fd >= 0) ::close(r.eis_fd);
    if (kde_eis_ && kde_eis_->start(output_name_.toStdString())) {
      x11_.set_eis_inject(&kde_eis_->core());
      eis_ok = true;
      emit logMessage(tr("Pointer: KWin EIS region for “%1” (local=%2, logical origin %3,%4)")
                          .arg(output_name_)
                          .arg(kde_eis_->core().region_local() ? tr("yes") : tr("no"))
                          .arg(place_x_)
                          .arg(place_y_));
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
  KwinWindowPlacer::disarm_portal_dialog_on_primary();

  const bool restore_rejected =
      e.contains(QStringLiteral("restore rejected"), Qt::CaseInsensitive) ||
      (portal_ && portal_->used_restore_token() &&
       e.contains(QStringLiteral("cancelled"), Qt::CaseInsensitive));

  if (restore_rejected && !restore_token_.isEmpty() && loading_.load()) {
    // Portal session is dead after Response≠0 — drop it before retry.
    if (portal_) portal_->stop();
    // Brief settle: previous ScreenCast Close can race the next CreateSession.
    if (portal_restore_retries_ < 3 && !portal_retry_used_) {
      ++portal_restore_retries_;
      emit logMessage(tr("ScreenCast restore rejected — silent retry %1/3 (token kept)")
                          .arg(portal_restore_retries_));
      // Virtual may still be coming up after recreate; settle before Start again.
      QTimer::singleShot(3500, this, [this] {
        if (!loading_.load() || active_.load()) return;
        setState(State::Placed);
        beginPortal(false);
      });
      return;
    }
    if (!portal_retry_used_) {
      portal_retry_used_ = true;
      // Memory only — keep conf token until a successful Start replaces it.
      emit logMessage(tr("ScreenCast restore still rejected — opening host picker "
                         "(conf token kept until a new one is saved)"));
      restore_token_.clear();
      emit toast(tr("Host: pick Virtual-monaSphere"));
      QTimer::singleShot(900, this, [this] {
        if (!loading_.load() || active_.load()) return;
        setState(State::Placed);
        beginPortal(true);
      });
      return;
    }
  }

  fail(tr("ScreenCast failed: %1").arg(e));
}

}  // namespace vrp
