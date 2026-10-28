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

QString VirtualDisplayManager::token(int span) const {
  const int s = std::clamp(span, 1, 3);
  return tokens_.value(s);
}

void VirtualDisplayManager::loadToken(int span, const QString& token) {
  const int s = std::clamp(span, 1, 3);
  if (token.isEmpty()) tokens_.remove(s);
  else tokens_.insert(s, token);
}

void VirtualDisplayManager::setToken(int span, const QString& token) {
  const int s = std::clamp(span, 1, 3);
  if (token.isEmpty()) {
    clearToken(s);
    return;
  }
  tokens_.insert(s, token);
  emit logMessage(tr("Virtual: token x%1 saved (%2 chars)").arg(s).arg(token.size()));
  emit tokenChanged(s, token);
}

void VirtualDisplayManager::clearToken(int span) {
  const int s = std::clamp(span, 1, 3);
  if (!tokens_.contains(s) || tokens_.value(s).isEmpty()) return;
  tokens_.remove(s);
  emit logMessage(tr("Virtual: token x%1 cleared").arg(s));
  emit tokenChanged(s, QString());
}

void VirtualDisplayManager::clearAllTokens() {
  const QList<int> keys = tokens_.keys();
  for (int s : keys) clearToken(s);
}

bool VirtualDisplayManager::wait_gone(int timeout_ms) {
  QElapsedTimer t;
  t.start();
  while (t.elapsed() < timeout_ms) {
    if (!KdeVirtualDisplay::anyManagedOutputExists()) return true;
    // Do not processEvents here — re-enters Live start/stop and deadlocks the spinner.
    QThread::msleep(100);
  }
  return !KdeVirtualDisplay::anyManagedOutputExists();
}

void VirtualDisplayManager::commit_identity(const Spec& want, const QString& matched, Result* r) {
  current_ = want;
  have_current_ = true;
  const QString expected = QStringLiteral("Virtual-") + want.name;
  output_name_ = !matched.isEmpty() ? matched : (vdisp_ ? vdisp_->output_name() : expected);

  QString idx_err;
  screen_index_ = KwinWindowPlacer::query_output_index(output_name_, &idx_err);
  if (screen_index_ < 0 && output_name_ != expected) {
    screen_index_ = KwinWindowPlacer::query_output_index(expected, &idx_err);
    if (screen_index_ >= 0) output_name_ = expected;
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
        tr("Virtual: identity %1 → KWin screen #%2").arg(output_name_).arg(screen_index_));
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
  QString matched;
  QRect r;
  if (KdeVirtualDisplay::queryOutputRect(needle, &r, &matched) && !matched.isEmpty()) {
    output_name_ = matched;
  } else {
    output_name_ = needle;
  }
  QString idx_err;
  screen_index_ = KwinWindowPlacer::query_output_index(output_name_, &idx_err);
  if (screen_index_ < 0) {
    if (error_out) *error_out = idx_err.isEmpty() ? QStringLiteral("no screen index") : idx_err;
    return false;
  }
  emit logMessage(
      tr("Virtual: refreshed identity %1 → KWin screen #%2").arg(output_name_).arg(screen_index_));
  return true;
}

bool VirtualDisplayManager::wait_ready(const Spec& want, QString* matched_out) {
  const QString out = QStringLiteral("Virtual-") + want.name;
  int place_x = 0, place_y = 0;
  if (QScreen* primary = QGuiApplication::primaryScreen()) {
    place_x = primary->geometry().right() + 1;
    place_y = primary->geometry().top();
  }

  QElapsedTimer wait;
  wait.start();
  QString matched;
  while (wait.elapsed() < 10000) {
    (void)KdeVirtualDisplay::enableOutput(out, place_x, place_y, want.scale);
    int mw = 0, mh = 0;
    float sc = 1.f;
    QRect r;
    QString m2;
    const bool have_rect = KdeVirtualDisplay::queryOutputRect(out, &r, &m2) && !r.isEmpty();
    const bool have_good_mode =
        KdeVirtualDisplay::queryOutputMode(out, &mw, &mh, &sc, &matched) &&
        std::abs(mw - want.width) <= 16 && std::abs(mh - want.height) <= 16 &&
        std::abs(sc - want.scale) <= 0.15f;

    if (have_good_mode && have_rect) {
      if (matched_out) *matched_out = !matched.isEmpty() ? matched : m2;
      return true;
    }

    if (vdisp_->matches_launch(want.width, want.height, want.scale) &&
        (have_rect || wait.elapsed() > 2000)) {
      if (matched_out) *matched_out = !m2.isEmpty() ? m2 : matched;
      return true;
    }

    QThread::msleep(150);
  }
  return false;
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
    if (!vdisp_->matches_launch(want.width, want.height, want.scale)) {
      vdisp_->set_name(want.name);
      vdisp_->set_resolution(want.width, want.height);
      vdisp_->set_scale(want.scale);
      (void)vdisp_->start();
    }
    r.ok = true;
    r.action = Action::Reused;
    // Always re-resolve name+index — screen order can change while Virtual stays up.
    QString matched;
    QRect rect;
    const QString out = QStringLiteral("Virtual-") + want.name;
    (void)KdeVirtualDisplay::queryOutputRect(out, &rect, &matched);
    commit_identity(want, matched, &r);
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
    emit busyStatus(QString());
    return r;
  }

  // Output still listed at requested size but bookkeeping lost — adopt, do not recreate.
  {
    int mw = 0, mh = 0;
    float sc = 0.f;
    QString matched;
    const QString out = QStringLiteral("Virtual-") + want.name;
    if (KdeVirtualDisplay::queryOutputMode(out, &mw, &mh, &sc, &matched) &&
        std::abs(mw - want.width) <= 16 && std::abs(mh - want.height) <= 16 &&
        std::abs(sc - want.scale) <= 0.15f) {
      vdisp_->set_name(want.name);
      vdisp_->set_resolution(want.width, want.height);
      vdisp_->set_scale(want.scale);
      (void)vdisp_->start();
      r.ok = true;
      r.action = Action::Reused;
      commit_identity(want, matched, &r);
      r.restore_token = token(want.span);
      VRP_LOG("Virtual: adopt existing %s idx=%d %dx%d (x%d)", r.output_name.toUtf8().constData(),
              r.screen_index, want.width, want.height, want.span);
      emit logMessage(tr("Virtual: adopt existing %1 (#%2) %3×%4 (x%5)")
                          .arg(r.output_name)
                          .arg(r.screen_index)
                          .arg(want.width)
                          .arg(want.height)
                          .arg(want.span));
      emit busyStatus(QString());
      return r;
    }
  }

  const bool had = alive() || KdeVirtualDisplay::anyManagedOutputExists();
  if (had) {
    emit busyStatus(tr("Virtual 削除中"));
    VRP_LOG("Virtual: recreate %dx%d (x%d) — deleting old first", want.width, want.height,
            want.span);
    emit logMessage(tr("Virtual: change → remove then create %1×%2 (x%3)")
                        .arg(want.width)
                        .arg(want.height)
                        .arg(want.span));
    vdisp_->stop();
    (void)KdeVirtualDisplay::killAllManaged();
    have_current_ = false;
    output_name_.clear();
    screen_index_ = -1;
    if (!wait_gone(8000)) {
      emit logMessage(tr("Virtual: still listed after stop — continuing"));
    }
  }

  emit busyStatus(tr("Virtual 作成中"));
  vdisp_->set_name(want.name);
  vdisp_->set_resolution(want.width, want.height);
  vdisp_->set_scale(want.scale);
  if (!vdisp_->start()) {
    r.error = tr("krfb-virtualmonitor failed to start");
    emit busyStatus(QString());
    emit logMessage(r.error);
    return r;
  }

  QString matched;
  if (!wait_ready(want, &matched)) {
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
  commit_identity(want, matched, &r);
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
  emit busyStatus(QString());
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
    emit busyStatus(QString());
    return;
  }
  emit busyStatus(tr("Virtual 削除中"));
  VRP_LOG("Virtual: destroy (tokens kept)");
  emit logMessage(tr("Virtual: destroy (tokens kept)"));
  vdisp_->stop();
  (void)KdeVirtualDisplay::killAllManaged();
  have_current_ = false;
  output_name_.clear();
  screen_index_ = -1;
  if (wait_gone(8000)) {
    emit logMessage(tr("Virtual: removed"));
  } else {
    emit logMessage(tr("Virtual: still listed after destroy — continuing"));
  }
  emit busyStatus(QString());
}

}  // namespace vrp
