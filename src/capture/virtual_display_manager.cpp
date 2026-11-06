#include "capture/virtual_display_manager.hpp"

#include "capture/kwin_window_placer.hpp"
#include "common.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QRegularExpression>
#include <QScreen>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <future>
#include <mutex>

namespace vrp {

VirtualDisplayManager::VirtualDisplayManager(QObject* parent) : QObject(parent) {
  vdisp_ = new KdeVirtualDisplay(this);
  connect(vdisp_, &KdeVirtualDisplay::failed, this, [this](const QString& e) {
    if (e.contains(QStringLiteral("クラッシュ"), Qt::CaseInsensitive) ||
        e.contains(QStringLiteral("crashed"), Qt::CaseInsensitive)) {
      return;
    }
    emit logMessage(tr("Virtual display: %1").arg(e));
  });
}

int VirtualDisplayManager::spanFromName(const QString& name_or_output) {
  const QRegularExpression re(QStringLiteral("(?:Virtual-)?monaSphere-x([1-3])"),
                              QRegularExpression::CaseInsensitiveOption);
  const auto m = re.match(name_or_output);
  if (m.hasMatch()) return std::clamp(m.captured(1).toInt(), 1, 3);
  // Canonical shared name — span comes from caller (resolution), not the name.
  return 1;
}

VirtualDisplayManager::Spec VirtualDisplayManager::normalize(Spec want) {
  if (want.name.isEmpty()) want.name = KdeVirtualDisplay::nameForSpan(1);
  if (want.width < 640) want.width = 1280;
  if (want.height < 360) want.height = 720;
  if (want.scale < 1.f || want.scale > 3.f) want.scale = 2.f;
  if (want.span < 1 || want.span > 3) want.span = spanFromName(want.name);
  return want;
}

bool VirtualDisplayManager::spec_equal(const Spec& a, int w, int h, float sc, const QString& name) {
  return a.name == name && std::abs(a.width - w) <= 16 && std::abs(a.height - h) <= 16 &&
         std::abs(a.scale - sc) <= 0.05f;
}

bool VirtualDisplayManager::alive() const {
  // Guard: callers may race Host teardown (YouTubeSession dtor / deleteChildren).
  if (!vdisp_) return false;
  return vdisp_->owned() || vdisp_->running();
}

bool VirtualDisplayManager::matches(const Spec& want_in) const {
  if (!alive() || !have_current_) return false;
  const Spec want = normalize(want_in);
  return spec_equal(want, current_.width, current_.height, current_.scale, current_.name);
}

QString VirtualDisplayManager::output_name() const {
  if (!output_name_.isEmpty()) return output_name_;
  return vdisp_ ? vdisp_->output_name() : QString();
}

QString VirtualDisplayManager::name() const { return have_current_ ? current_.name : QString(); }

int VirtualDisplayManager::width() const { return have_current_ ? current_.width : 0; }

int VirtualDisplayManager::height() const { return have_current_ ? current_.height : 0; }

float VirtualDisplayManager::scale() const { return have_current_ ? current_.scale : 0.f; }

QString VirtualDisplayManager::token(int /*span*/) const {
  // One Remember grant for Virtual-monaSphere — span only selects resolution.
  for (int s = 1; s <= 3; ++s) {
    const QString t = tokens_.value(s);
    if (!t.isEmpty()) return t;
  }
  return {};
}

void VirtualDisplayManager::loadToken(int /*span*/, const QString& token) {
  tokens_.clear();
  if (!token.isEmpty()) tokens_.insert(1, token);
}

void VirtualDisplayManager::setToken(int /*span*/, const QString& token) {
  if (token.isEmpty()) {
    clearToken(1);
    return;
  }
  tokens_.clear();
  tokens_.insert(1, token);
  emit logMessage(tr("Virtual: token saved (%1 chars)").arg(token.size()));
  emit tokenChanged(1, token);
}

void VirtualDisplayManager::clearToken(int /*span*/) {
  if (tokens_.isEmpty()) return;
  tokens_.clear();
  emit logMessage(tr("Virtual: token cleared"));
  emit tokenChanged(1, QString());
}

void VirtualDisplayManager::clearAllTokens() { clearToken(1); }

bool VirtualDisplayManager::wait_gone(int timeout_ms, int gone_budget) {
  int left = std::max(0, gone_budget);
  QElapsedTimer t;
  t.start();
  while (t.elapsed() < timeout_ms) {
    if (!KdeVirtualDisplay::anyManagedOutputExists()) {
      add_progress(left);
      return true;
    }
    // Do not processEvents here — re-enters Live start/stop and deadlocks the spinner.
    if (left > 6) {
      add_progress(6);
      left -= 6;
    }
    QThread::msleep(100);
  }
  const bool gone = !KdeVirtualDisplay::anyManagedOutputExists();
  add_progress(left);
  return gone;
}

void VirtualDisplayManager::set_progress_sink(std::function<void(int)> sink) {
  std::lock_guard<std::mutex> lock(progress_mu_);
  progress_sink_ = std::move(sink);
}

void VirtualDisplayManager::begin_progress() {
  std::function<void(int)> sink;
  {
    std::lock_guard<std::mutex> lock(progress_mu_);
    progress_ = 0;
    sink = progress_sink_;
  }
  if (sink) sink(0);
}

void VirtualDisplayManager::add_progress(int delta) {
  if (delta <= 0) return;
  std::function<void(int)> sink;
  int now = 0;
  {
    std::lock_guard<std::mutex> lock(progress_mu_);
    if (progress_ < 0) progress_ = 0;
    progress_ = std::min(100, progress_ + delta);
    now = progress_;
    sink = progress_sink_;
  }
  if (sink) sink(now);
}

void VirtualDisplayManager::commit_identity(const Spec& want, const QString& matched, Result* r,
                                            int known_index, bool index_checked) {
  current_ = want;
  have_current_ = true;
  const QString expected = QStringLiteral("Virtual-") + want.name;
  output_name_ = !matched.isEmpty() ? matched : (vdisp_ ? vdisp_->output_name() : expected);

  QString idx_err;
  if (index_checked && known_index >= 0) {
    screen_index_ = known_index;
  } else {
    screen_index_ = KwinWindowPlacer::query_output_index(output_name_, &idx_err);
    if (screen_index_ < 0 && output_name_ != expected) {
      screen_index_ = KwinWindowPlacer::query_output_index(expected, &idx_err);
      if (screen_index_ >= 0) output_name_ = expected;
    }
  }
  if (screen_index_ < 0) {
    VRP_LOG("Virtual: identity name=%s index unresolved (%s)",
            output_name_.toUtf8().constData(), idx_err.toUtf8().constData());
    emit logMessage(tr("Virtual: KWin screen index unresolved for %1 (%2)")
                        .arg(output_name_, idx_err.isEmpty() ? tr("no report") : idx_err));
  } else {
    VRP_LOG("Virtual: identity name=%s index=%d", output_name_.toUtf8().constData(),
            screen_index_);
    emit logMessage(
        tr("Virtual: identity %1 -> KWin screen #%2").arg(output_name_).arg(screen_index_));
  }
  if (r) {
    r->output_name = output_name_;
    r->screen_index = screen_index_;
  }
}

bool VirtualDisplayManager::refresh_identity(QString* error_out) {
  if (!have_current_ && !alive()) {
    if (error_out) *error_out = QStringLiteral("no virtual");
    screen_index_ = -1;
    output_name_.clear();
    return false;
  }
  const QString needle =
      !output_name_.isEmpty()
          ? output_name_
          : (have_current_ ? (QStringLiteral("Virtual-") + current_.name)
                           : (vdisp_ ? vdisp_->output_name() : QString()));
  if (needle.isEmpty()) {
    if (error_out) *error_out = QStringLiteral("empty output name");
    return false;
  }
  // Name is already known. kscreen geometry and the KWin screen index do not
  // depend on each other, so they run together. Both results are checked.
  auto snap = std::async(std::launch::async, [needle] {
    return KdeVirtualDisplay::describeOutput(needle);
  });
  QString idx_err;
  screen_index_ = KwinWindowPlacer::query_output_index(needle, &idx_err);
  const KdeVirtualDisplay::OutputInfo info = snap.get();
  if (info.listed && !info.matched.isEmpty()) output_name_ = info.matched;
  else output_name_ = needle;
  if (screen_index_ < 0 && output_name_ != needle) {
    screen_index_ = KwinWindowPlacer::query_output_index(output_name_, &idx_err);
  }
  if (screen_index_ < 0) {
    if (error_out) *error_out = idx_err.isEmpty() ? QStringLiteral("no screen index") : idx_err;
    return false;
  }
  emit logMessage(
      tr("Virtual: refreshed identity %1 -> KWin screen #%2").arg(output_name_).arg(screen_index_));
  return true;
}

bool VirtualDisplayManager::wait_ready(const Spec& want, QString* matched_out,
                                       int* screen_index_out) {
  const QString out = QStringLiteral("Virtual-") + want.name;
  int place_x = 0, place_y = 0;
  if (QScreen* primary = QGuiApplication::primaryScreen()) {
    place_x = primary->geometry().right() + 1;
    place_y = primary->geometry().top();
  }

  QElapsedTimer wait;
  wait.start();
  QString matched;
  // KWin index only needs the output name, which is fixed. The script runs on
  // this thread (its D-Bus reply is delivered here) while the first kscreen
  // snapshot runs beside it. A miss is not final — commit_identity queries
  // again after the output is actually up.
  int early_index = -1;
  bool index_done = false;
  // Do NOT call enableOutput every poll tick — each call runs 3× kscreen-doctor and
  // thrashs KWin/plasmashell (unexisting screen geometry / delayed input).
  int enable_count = 0;
  qint64 last_enable_ms = -100000;
  bool ready = false;
  // Create-phase shares, summed with the caller's start/identity credits to 100:
  // index 15, snapshots 20, enable 30.
  int snap_left = 20;
  int enable_left = 30;
  auto credit_snap = [&] {
    if (snap_left <= 5) return;
    add_progress(5);
    snap_left -= 5;
  };
  auto credit_enable = [&] {
    const int step = std::min(10, enable_left);
    enable_left -= step;
    add_progress(step);
  };
  while (wait.elapsed() < 10000) {
    auto snap = std::async(std::launch::async, [&] {
      const KdeVirtualDisplay::OutputInfo info = KdeVirtualDisplay::describeOutput(out);
      credit_snap();
      return info;
    });
    if (!index_done) {
      QString idx_err;
      early_index = KwinWindowPlacer::query_output_index(out, &idx_err);
      index_done = true;
      add_progress(15);
    }
    const KdeVirtualDisplay::OutputInfo info = snap.get();
    const bool have_rect = info.listed && !info.rect.isEmpty();
    const bool have_good_mode =
        info.have_mode && std::abs(info.mode_w - want.width) <= 16 &&
        std::abs(info.mode_h - want.height) <= 16 && std::abs(info.scale - want.scale) <= 0.15f;
    if (info.listed && !info.matched.isEmpty()) matched = info.matched;

    if (have_good_mode && have_rect) {
      add_progress(snap_left + enable_left);
      snap_left = 0;
      enable_left = 0;
      ready = true;
      break;
    }

    // Place/enable at most twice: once when the output first appears, once if still
    // missing a good mode after 2s (kscreen sometimes needs a second nudge).
    // enable → position → scale stays in that order inside enableOutput.
    const qint64 now = wait.elapsed();
    const bool want_enable =
        info.listed && enable_count < 2 &&
        (enable_count == 0 || (now - last_enable_ms >= 2000 && !have_good_mode));
    if (want_enable) {
      (void)KdeVirtualDisplay::enableOutput(out, place_x, place_y, want.scale, credit_enable);
      ++enable_count;
      last_enable_ms = now;
    }

    if (vdisp_->matches_launch(want.width, want.height, want.scale) &&
        (have_rect || wait.elapsed() > 2000)) {
      add_progress(snap_left + enable_left);
      snap_left = 0;
      enable_left = 0;
      ready = true;
      break;
    }

    QThread::msleep(150);
  }
  if (screen_index_out) *screen_index_out = early_index;
  if (matched_out && ready) *matched_out = matched;
  return ready;
}

VirtualDisplayManager::Result VirtualDisplayManager::ensure(const Spec& want_in) {
  Result r;
  const Spec want = normalize(want_in);
  active_span_ = want.span;
  r.span = want.span;
  r.restore_token = token(want.span);

  // Same name + resolution + scale → reuse (x3→x3 must not delete).
  if (alive() && have_current_ &&
      spec_equal(want, current_.width, current_.height, current_.scale, current_.name)) {
    // Reuse: launch 20, kscreen 40, first index 25, identity 15.
    begin_progress();
    if (!vdisp_->matches_launch(want.width, want.height, want.scale)) {
      vdisp_->set_name(want.name);
      vdisp_->set_resolution(want.width, want.height);
      vdisp_->set_scale(want.scale);
      (void)vdisp_->start();
    }
    add_progress(20);
    r.ok = true;
    r.action = Action::Reused;
    // Always re-resolve name+index — screen order can change while Virtual stays up.
    // Output name is known. Geometry and KWin index have no order between them.
    const QString out = QStringLiteral("Virtual-") + want.name;
    auto snap = std::async(std::launch::async, [this, out] {
      const KdeVirtualDisplay::OutputInfo info = KdeVirtualDisplay::describeOutput(out);
      add_progress(40);
      return info;
    });
    QString idx_err;
    const int idx = KwinWindowPlacer::query_output_index(out, &idx_err);
    add_progress(25);
    const KdeVirtualDisplay::OutputInfo info = snap.get();
    const QString matched = info.listed ? info.matched : QString();
    // The overlapped index is for `out`. A different kscreen name, or a miss,
    // is resolved again inside commit_identity.
    const bool index_ok = info.listed && idx >= 0 && (matched.isEmpty() || matched == out);
    commit_identity(want, matched, &r, idx, index_ok);
    add_progress(15);
    r.restore_token = token(want.span);
    VRP_LOG("Virtual: reuse %s idx=%d %dx%d (x%d)", r.output_name.toUtf8().constData(),
            r.screen_index, want.width, want.height, want.span);
    emit logMessage(tr("Virtual: reuse %1 (#%2) %3×%4 @%5× (x%6 token %7)")
                        .arg(r.output_name)
                        .arg(r.screen_index)
                        .arg(want.width)
                        .arg(want.height)
                        .arg(QString::number(static_cast<double>(want.scale), 'f', 2))
                        .arg(want.span)
                        .arg(r.restore_token.isEmpty() ? tr("no")
                                                       : tr("%1 chars").arg(r.restore_token.size())));
    return r;
  }

  // Output still listed at requested size but bookkeeping lost — adopt, do not recreate.
  {
    int mw = 0, mh = 0;
    float sc = 0.f;
    QString matched;
    const QString out = QStringLiteral("Virtual-") + want.name;
    // Adopt: mode query 40, krfb start 35, identity 25. A miss falls through and resets.
    begin_progress();
    if (KdeVirtualDisplay::queryOutputMode(out, &mw, &mh, &sc, &matched) &&
        std::abs(mw - want.width) <= 16 && std::abs(mh - want.height) <= 16 &&
        std::abs(sc - want.scale) <= 0.15f) {
      add_progress(40);
      vdisp_->set_name(want.name);
      vdisp_->set_resolution(want.width, want.height);
      vdisp_->set_scale(want.scale);
      if (!vdisp_->start()) {
        r.error = tr("krfb-virtualmonitor failed to start");
        emit busyStatus(QString());
        emit logMessage(r.error);
        return r;
      }
      add_progress(35);
      r.ok = true;
      r.action = Action::Reused;
      commit_identity(want, matched, &r, -1, false);
      add_progress(25);
      r.restore_token = token(want.span);
      VRP_LOG("Virtual: adopt existing %s idx=%d %dx%d (x%d)", r.output_name.toUtf8().constData(),
              r.screen_index, want.width, want.height, want.span);
      emit logMessage(tr("Virtual: adopt existing %1 (#%2) %3×%4 (x%5)")
                          .arg(r.output_name)
                          .arg(r.screen_index)
                          .arg(want.width)
                          .arg(want.height)
                          .arg(want.span));
      return r;
    }
  }

  const bool had = alive() || KdeVirtualDisplay::anyManagedOutputExists();
  if (had) {
    // Delete: rules 20, stop 25, orphans 15, output gone 30, settle 10.
    begin_progress();
    emit busyStatus(tr("Virtual 削除中"));
    VRP_LOG("Virtual: recreate %dx%d (x%d) — deleting old first", want.width, want.height,
            want.span);
    emit logMessage(tr("Virtual: change -> remove then create %1×%2 (x%3)")
                        .arg(want.width)
                        .arg(want.height)
                        .arg(want.span));
    // Drop Force-screen rules before the output vanishes (dead index → Plasma thrash).
    KwinWindowPlacer::clear_live_session_rules();
    add_progress(20);
    vdisp_->stop(/*kill_orphans=*/false);
    add_progress(25);
    (void)KdeVirtualDisplay::killAllManaged();
    add_progress(15);
    have_current_ = false;
    output_name_.clear();
    screen_index_ = -1;
    if (!wait_gone(8000, 30)) {
      emit logMessage(tr("Virtual: still listed after stop — continuing"));
    }
    // Let plasmashell finish screenRemoved before a new Virtual appears.
    QThread::msleep(400);
    add_progress(10);
  }

  // Create: nothing-to-delete 5 (fresh only) or folded into start 20 (after delete),
  // start 15 when fresh, index 15 + snapshots 20 + enable 30 inside wait_ready, identity 15.
  begin_progress();
  emit busyStatus(tr("Virtual 作成中"));
  if (!had) add_progress(5);
  vdisp_->set_name(want.name);
  vdisp_->set_resolution(want.width, want.height);
  vdisp_->set_scale(want.scale);
  if (!vdisp_->start()) {
    r.error = tr("krfb-virtualmonitor failed to start");
    emit busyStatus(QString());
    emit logMessage(r.error);
    return r;
  }
  add_progress(had ? 20 : 15);

  QString matched;
  int early_index = -1;
  if (!wait_ready(want, &matched, &early_index)) {
    r.error = tr("Virtual “%1” did not become ready (%2×%3)")
                  .arg(vdisp_->output_name())
                  .arg(want.width)
                  .arg(want.height);
    emit busyStatus(QString());
    emit logMessage(r.error);
    return r;
  }

  r.ok = true;
  r.action = had ? Action::Recreated : Action::Created;
  commit_identity(want, matched, &r, early_index, early_index >= 0);
  add_progress(15);
  r.restore_token = token(want.span);
  emit logMessage(tr("Virtual: %1 %2 (#%3) %4×%5 @%6× (x%7 token %8)")
                      .arg(r.action == Action::Recreated ? tr("recreated") : tr("created"))
                      .arg(r.output_name)
                      .arg(r.screen_index)
                      .arg(want.width)
                      .arg(want.height)
                      .arg(QString::number(static_cast<double>(want.scale), 'f', 2))
                      .arg(want.span)
                      .arg(r.restore_token.isEmpty() ? tr("no")
                                                     : tr("%1 chars").arg(r.restore_token.size())));
  return r;
}

void VirtualDisplayManager::destroy() {
  if (!vdisp_) {
    have_current_ = false;
    output_name_.clear();
    screen_index_ = -1;
    return;
  }
  if (!alive() && !KdeVirtualDisplay::anyManagedOutputExists()) {
    have_current_ = false;
    output_name_.clear();
    screen_index_ = -1;
    // Still scrub leftover rules from a prior aborted session.
    KwinWindowPlacer::clear_live_session_rules();
    emit busyStatus(QString());
    return;
  }
  // Delete: rules 20, stop 25, orphans 15, output gone 30, settle 10.
  begin_progress();
  emit busyStatus(tr("Virtual 削除中"));
  VRP_LOG("Virtual: destroy (tokens kept)");
  emit logMessage(tr("Virtual: destroy (tokens kept)"));
  KwinWindowPlacer::clear_live_session_rules();
  add_progress(20);
  vdisp_->stop(/*kill_orphans=*/false);
  add_progress(25);
  (void)KdeVirtualDisplay::killAllManaged();
  add_progress(15);
  have_current_ = false;
  output_name_.clear();
  screen_index_ = -1;
  if (wait_gone(8000, 30)) {
    emit logMessage(tr("Virtual: removed"));
  } else {
    emit logMessage(tr("Virtual: still listed after destroy — continuing"));
  }
  // Brief settle so plasmashell can drop containments for the gone output.
  QThread::msleep(400);
  add_progress(10);
  emit busyStatus(QString());
}

}  // namespace vrp
