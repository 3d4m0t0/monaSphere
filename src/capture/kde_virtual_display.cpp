#include "capture/kde_virtual_display.hpp"

#include "common.hpp"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThread>

#include <algorithm>
#include <cmath>

namespace vrp {
namespace {

QString findKscreenDoctor() {
  const QString p = QStandardPaths::findExecutable(QStringLiteral("kscreen-doctor"));
  if (!p.isEmpty()) return p;
  const QString abs = QStringLiteral("/usr/bin/kscreen-doctor");
  return QFileInfo::exists(abs) ? abs : QString();
}

bool runKscreen(const QStringList& args, QByteArray* stdout_out, int timeout_ms = 4000) {
  const QString bin = findKscreenDoctor();
  if (bin.isEmpty()) return false;
  QProcess p;
  p.setProcessChannelMode(QProcess::MergedChannels);
  p.start(bin, args);
  if (!p.waitForStarted(2000)) return false;
  if (!p.waitForFinished(timeout_ms)) {
    p.kill();
    (void)p.waitForFinished(500);
    return false;
  }
  if (stdout_out) *stdout_out = p.readAllStandardOutput();
  return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

QRect rectFromOutputObject(const QJsonObject& o) {
  // Plasma 6 JSON: pos.{x,y} + size.{width,height} or geometry.{x,y,width,height}
  int x = 0, y = 0, w = 0, h = 0;
  if (o.contains(QStringLiteral("geometry")) && o.value(QStringLiteral("geometry")).isObject()) {
    const QJsonObject g = o.value(QStringLiteral("geometry")).toObject();
    x = g.value(QStringLiteral("x")).toInt();
    y = g.value(QStringLiteral("y")).toInt();
    w = g.value(QStringLiteral("width")).toInt();
    h = g.value(QStringLiteral("height")).toInt();
  } else {
    if (o.contains(QStringLiteral("pos")) && o.value(QStringLiteral("pos")).isObject()) {
      const QJsonObject p = o.value(QStringLiteral("pos")).toObject();
      x = p.value(QStringLiteral("x")).toInt();
      y = p.value(QStringLiteral("y")).toInt();
    } else {
      x = o.value(QStringLiteral("pos_x")).toInt(o.value(QStringLiteral("x")).toInt());
      y = o.value(QStringLiteral("pos_y")).toInt(o.value(QStringLiteral("y")).toInt());
    }
    if (o.contains(QStringLiteral("size")) && o.value(QStringLiteral("size")).isObject()) {
      const QJsonObject s = o.value(QStringLiteral("size")).toObject();
      w = s.value(QStringLiteral("width")).toInt();
      h = s.value(QStringLiteral("height")).toInt();
    } else {
      w = o.value(QStringLiteral("width")).toInt();
      h = o.value(QStringLiteral("height")).toInt();
    }
  }
  if (w <= 0 || h <= 0) return {};
  return QRect(x, y, w, h);
}

bool findOutputObject(const QString& needle_in, QJsonObject* out_obj, QString* matched_name,
                      bool require_enabled) {
  if (!out_obj || needle_in.isEmpty()) return false;
  QByteArray json;
  if (!runKscreen({QStringLiteral("-j")}, &json, 5000)) return false;
  const QJsonDocument doc = QJsonDocument::fromJson(json);
  if (!doc.isObject()) return false;
  const QJsonObject root = doc.object();
  QJsonArray outputs = root.value(QStringLiteral("outputs")).toArray();
  if (outputs.isEmpty()) outputs = root.value(QStringLiteral("screens")).toArray();
  const QString needle = needle_in.toLower();
  auto try_one = [&](bool only_enabled) -> bool {
    for (const QJsonValue& v : outputs) {
      if (!v.isObject()) continue;
      const QJsonObject o = v.toObject();
      const QString name = o.value(QStringLiteral("name")).toString();
      if (!name.toLower().contains(needle)) continue;
      if (only_enabled && o.contains(QStringLiteral("enabled")) &&
          !o.value(QStringLiteral("enabled")).toBool()) {
        continue;
      }
      *out_obj = o;
      if (matched_name) *matched_name = name;
      return true;
    }
    return false;
  };
  if (require_enabled && try_one(true)) return true;
  return try_one(false);
}

void modeSizeFromObject(const QJsonObject& o, int* mode_w, int* mode_h, float* scale) {
  float sc = static_cast<float>(o.value(QStringLiteral("scale")).toDouble(1.0));
  if (sc < 0.5f) sc = 1.f;
  if (scale) *scale = sc;

  int mw = 0, mh = 0;
  if (o.contains(QStringLiteral("mode")) && o.value(QStringLiteral("mode")).isObject()) {
    const QJsonObject m = o.value(QStringLiteral("mode")).toObject();
    if (m.contains(QStringLiteral("size")) && m.value(QStringLiteral("size")).isObject()) {
      const QJsonObject ms = m.value(QStringLiteral("size")).toObject();
      mw = ms.value(QStringLiteral("width")).toInt();
      mh = ms.value(QStringLiteral("height")).toInt();
    } else {
      mw = m.value(QStringLiteral("width")).toInt();
      mh = m.value(QStringLiteral("height")).toInt();
    }
  }
  if ((mw <= 0 || mh <= 0) && o.contains(QStringLiteral("size")) &&
      o.value(QStringLiteral("size")).isObject()) {
    const QJsonObject s = o.value(QStringLiteral("size")).toObject();
    mw = s.value(QStringLiteral("width")).toInt();
    mh = s.value(QStringLiteral("height")).toInt();
  }
  if (mw <= 0 || mh <= 0) {
    const QRect r = rectFromOutputObject(o);
    mw = static_cast<int>(std::lround(static_cast<double>(r.width()) * static_cast<double>(sc)));
    mh = static_cast<int>(std::lround(static_cast<double>(r.height()) * static_cast<double>(sc)));
  }
  if (mode_w) *mode_w = mw;
  if (mode_h) *mode_h = mh;
}

}  // namespace

KdeVirtualDisplay::KdeVirtualDisplay(QObject* parent) : QObject(parent) {
  QObject::connect(&proc_, &QProcess::started, this, &KdeVirtualDisplay::started);
  QObject::connect(&proc_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                   [this](int code, QProcess::ExitStatus) {
                     owned_ = false;
                     // Intentional terminate/kill looks like CrashExit — do not treat as failure.
                     if (!stopping_) emit stopped(code);
                   });
  QObject::connect(&proc_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError err) {
    if (stopping_) return;  // kill() → Crashed; stop() already owns teardown
    if (err == QProcess::FailedToStart) owned_ = false;
    // Crashed without our stop() = real unexpected death.
    emit failed(proc_.errorString());
  });
}

KdeVirtualDisplay::~KdeVirtualDisplay() { stop(); }

QString KdeVirtualDisplay::output_name() const {
  return QStringLiteral("Virtual-") + name_;
}

QString KdeVirtualDisplay::nameForSpan(int span) {
  const int s = std::clamp(span, 1, 3);
  return QStringLiteral("monaSphere-x%1").arg(s);
}

QString KdeVirtualDisplay::outputNameForSpan(int span) {
  return QStringLiteral("Virtual-") + nameForSpan(span);
}

QStringList KdeVirtualDisplay::allManagedNames() {
  // Legacy single name (pre per-span) + x1–x3 + old YT suffix.
  return {QStringLiteral("monaSphere"), QStringLiteral("monaSphereYT"), nameForSpan(1),
          nameForSpan(2), nameForSpan(3)};
}

int KdeVirtualDisplay::killAllManaged() {
  int n = 0;
  for (const QString& name : allManagedNames()) n += killOrphans(name);
  return n;
}

bool KdeVirtualDisplay::anyManagedOutputExists() {
  for (const QString& name : allManagedNames()) {
    if (outputExists(name) || outputExists(QStringLiteral("Virtual-") + name)) return true;
  }
  return false;
}

QString KdeVirtualDisplay::detectBinary() {
  const QString p = QStandardPaths::findExecutable(QStringLiteral("krfb-virtualmonitor"));
  if (!p.isEmpty()) return p;
  const QString abs = QStringLiteral("/usr/bin/krfb-virtualmonitor");
  if (QFileInfo::exists(abs)) return abs;
  return {};
}

bool KdeVirtualDisplay::waylandSession() {
  return !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") ||
         qEnvironmentVariable("XDG_SESSION_TYPE").compare(QLatin1String("wayland"),
                                                          Qt::CaseInsensitive) == 0;
}

bool KdeVirtualDisplay::enableOutput(const QString& output_name, int x, int y, float scale) {
  if (output_name.isEmpty() || findKscreenDoctor().isEmpty()) return false;
  const QString target = QStringLiteral("output.") + output_name;
  QByteArray ignored;
  // enable first, then position / scale (order matters on some Plasma builds)
  (void)runKscreen({target + QStringLiteral(".enable")}, &ignored);
  (void)runKscreen({target + QStringLiteral(".position.") + QString::number(x) + QLatin1Char(',') +
                    QString::number(y)},
                   &ignored);
  if (scale >= 1.f && scale <= 3.f) {
    // Prefer "1" / "1.5" — some Plasma builds reject trailing zeros.
    QString s = QString::number(static_cast<double>(scale), 'f', 2);
    while (s.endsWith(QLatin1Char('0')) && s.contains(QLatin1Char('.'))) s.chop(1);
    if (s.endsWith(QLatin1Char('.'))) s.chop(1);
    (void)runKscreen({target + QStringLiteral(".scale.") + s}, &ignored);
  }
  return true;
}

bool KdeVirtualDisplay::queryOutputRect(const QString& output_name_substr, QRect* out,
                                        QString* matched_name) {
  if (!out || output_name_substr.isEmpty()) return false;
  QJsonObject o;
  QString name;
  if (!findOutputObject(output_name_substr, &o, &name, /*require_enabled=*/true)) {
    if (!findOutputObject(output_name_substr, &o, &name, /*require_enabled=*/false)) return false;
    const QRect r = rectFromOutputObject(o);
    if (r.isEmpty()) {
      if (matched_name) *matched_name = name;
      return false;
    }
    *out = r;
    if (matched_name) *matched_name = name;
    return true;
  }
  const QRect r = rectFromOutputObject(o);
  if (r.isEmpty()) return false;
  *out = r;
  if (matched_name) *matched_name = name;
  return true;
}

bool KdeVirtualDisplay::queryOutputMode(const QString& output_name_substr, int* mode_w, int* mode_h,
                                        float* scale, QString* matched_name) {
  QJsonObject o;
  QString name;
  if (!findOutputObject(output_name_substr, &o, &name, /*require_enabled=*/false)) return false;
  modeSizeFromObject(o, mode_w, mode_h, scale);
  if (matched_name) *matched_name = name;
  return mode_w && mode_h && *mode_w > 0 && *mode_h > 0;
}

bool KdeVirtualDisplay::outputExists(const QString& output_name_substr) {
  QJsonObject o;
  return findOutputObject(output_name_substr, &o, nullptr, /*require_enabled=*/false);
}

bool KdeVirtualDisplay::running() const {
  return proc_.state() != QProcess::NotRunning;
}

int KdeVirtualDisplay::killOrphans(const QString& name) {
  if (name.isEmpty()) return 0;
  // Match argv: krfb-virtualmonitor … --name <name>
  QProcess pgrep;
  pgrep.start(QStringLiteral("pgrep"),
              {QStringLiteral("-f"),
               QStringLiteral("krfb-virtualmonitor.*--name[ =]%1").arg(name)});
  if (!pgrep.waitForFinished(2000) || pgrep.exitCode() != 0) return 0;
  const QStringList pids =
      QString::fromUtf8(pgrep.readAllStandardOutput()).split(QRegularExpression(QStringLiteral("\\s+")),
                                                             Qt::SkipEmptyParts);
  int killed = 0;
  for (const QString& ps : pids) {
    bool ok = false;
    const qint64 pid = ps.toLongLong(&ok);
    if (!ok || pid <= 1) continue;
    QProcess::execute(QStringLiteral("kill"), {QStringLiteral("-TERM"), QString::number(pid)});
    ++killed;
  }
  if (killed > 0) {
    QThread::msleep(400);
    // Force leftovers.
    for (const QString& ps : pids) {
      bool ok = false;
      const qint64 pid = ps.toLongLong(&ok);
      if (!ok || pid <= 1) continue;
      QProcess::execute(QStringLiteral("kill"), {QStringLiteral("-KILL"), QString::number(pid)});
    }
    QThread::msleep(200);
    VRP_LOG("live: killed %d orphan krfb-virtualmonitor (%s)", killed, name.toUtf8().constData());
  }
  return killed;
}

bool KdeVirtualDisplay::start() {
  if (running()) {
    owned_ = true;
    return true;
  }
  if (!waylandSession()) {
    emit failed(QStringLiteral("Virtual display requires a Wayland session (krfb-virtualmonitor)"));
    return false;
  }
  const QString bin = detectBinary();
  if (bin.isEmpty()) {
    emit failed(QStringLiteral("krfb-virtualmonitor not found (install package krfb)"));
    return false;
  }

  // Previous crashes left duplicate Virtual-* outputs; always clear first.
  (void)killOrphans(name_);

  const int w = width_ > 0 ? width_ : 1280;
  const int h = height_ > 0 ? height_ : 720;
  QStringList args;
  args << QStringLiteral("--name") << name_;
  args << QStringLiteral("--resolution") << QStringLiteral("%1x%2").arg(w).arg(h);
  // Older krfb builds require these; unused by monaSphere (no VNC client).
  args << QStringLiteral("--password") << QStringLiteral("monasphere");
  args << QStringLiteral("--port") << QString::number(port_);
  {
    QString s = QString::number(static_cast<double>(scale_ >= 1.f ? scale_ : 1.f), 'f', 2);
    // Drop trailing zeros: krfb accepts "1.5" / "2"
    while (s.endsWith(QLatin1Char('0')) && s.contains(QLatin1Char('.'))) s.chop(1);
    if (s.endsWith(QLatin1Char('.'))) s.chop(1);
    args << QStringLiteral("--scale") << s;
  }

  VRP_LOG("live: virtual display %s %s", bin.toUtf8().constData(),
          args.join(QLatin1Char(' ')).toUtf8().constData());
  proc_.setProgram(bin);
  proc_.setArguments(args);
  // Detach from TTY noise; keep stderr for diagnosis via QProcess::read.
  proc_.setProcessChannelMode(QProcess::MergedChannels);
  proc_.start();
  if (!proc_.waitForStarted(4000)) {
    owned_ = false;
    launched_w_ = launched_h_ = 0;
    launched_scale_ = 0.f;
    emit failed(proc_.errorString().isEmpty() ? QStringLiteral("krfb-virtualmonitor failed to start")
                                              : proc_.errorString());
    return false;
  }
  owned_ = true;
  launched_w_ = w;
  launched_h_ = h;
  launched_scale_ = scale_ >= 1.f ? scale_ : 1.f;
  return true;
}

void KdeVirtualDisplay::stop() {
  stopping_ = true;
  launched_w_ = launched_h_ = 0;
  launched_scale_ = 0.f;
  if (proc_.state() != QProcess::NotRunning) {
    VRP_LOG("live: stopping virtual display (pid %lld)",
            static_cast<long long>(proc_.processId()));
    proc_.terminate();
    if (!proc_.waitForFinished(1500)) {
      proc_.kill();
      (void)proc_.waitForFinished(800);
    }
  }
  owned_ = false;
  // Also clear orphans (e.g. previous crash left a second Virtual-* output).
  (void)killOrphans(name_);
  stopping_ = false;
}

}  // namespace vrp
