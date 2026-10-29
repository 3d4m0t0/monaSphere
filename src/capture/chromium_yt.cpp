#include "capture/chromium_yt.hpp"

#include "common.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QThread>

namespace vrp {
namespace {

void clearSingletonLocks(const QString& user_data_dir) {
  if (user_data_dir.isEmpty()) return;
  for (const char* name : {"SingletonLock", "SingletonSocket", "SingletonCookie"}) {
    QFile::remove(user_data_dir + QLatin1Char('/') + QLatin1String(name));
  }
}

void killBrowsersForProfile(const QString& user_data_dir) {
  if (user_data_dir.isEmpty()) return;
  QProcess pkill;
  pkill.start(QStringLiteral("pkill"),
              {QStringLiteral("-f"), QStringLiteral("user-data-dir=%1").arg(user_data_dir)});
  (void)pkill.waitForFinished(2500);
  QThread::msleep(150);
  clearSingletonLocks(user_data_dir);
}

bool chromeProcessRunning() {
  QProcess pgrep;
  pgrep.start(QStringLiteral("pgrep"),
              {QStringLiteral("-f"), QStringLiteral("[c]hrome|[c]hromium")});
  if (!pgrep.waitForFinished(1500) || pgrep.exitCode() != 0) return false;
  return !QString::fromUtf8(pgrep.readAllStandardOutput()).trimmed().isEmpty();
}

}  // namespace

ChromiumYtLauncher::ChromiumYtLauncher(QObject* parent) : QObject(parent) {
  QObject::connect(&proc_, &QProcess::started, this, &ChromiumYtLauncher::started);
  QObject::connect(&proc_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                   [this](int code, QProcess::ExitStatus) {
                     emit stopped(code);
                   });
  QObject::connect(&proc_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
    emit failed(proc_.errorString());
  });
}

ChromiumYtLauncher::~ChromiumYtLauncher() { stop(); }

QString ChromiumYtLauncher::detectBrowser() {
  const QStringList candidates = {
      QStringLiteral("google-chrome"),
      QStringLiteral("google-chrome-stable"),
      QStringLiteral("chromium"),
      QStringLiteral("chromium-browser"),
      QStringLiteral("chromium-freeworld"),
  };
  for (const QString& name : candidates) {
    const QString p = QStandardPaths::findExecutable(name);
    if (!p.isEmpty()) return p;
  }
  const QStringList abs = {
      QStringLiteral("/usr/bin/google-chrome"),
      QStringLiteral("/usr/bin/google-chrome-stable"),
      QStringLiteral("/usr/bin/chromium"),
      QStringLiteral("/usr/bin/chromium-browser"),
  };
  for (const QString& p : abs) {
    if (QFileInfo::exists(p)) return p;
  }
  return {};
}

QString ChromiumYtLauncher::profileDir() {
  const QString base = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
  return base + QStringLiteral("/monasphere/chromium-yt");
}

QString ChromiumYtLauncher::defaultUserDataDir(const QString& browser_path) {
  const QString base = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
  const QString bin = QFileInfo(browser_path).fileName().toLower();
  QStringList candidates;
  if (bin.contains(QLatin1String("chrome"))) {
    candidates << base + QStringLiteral("/google-chrome");
    candidates << base + QStringLiteral("/google-chrome-stable");
    candidates << base + QStringLiteral("/chromium");
  } else {
    candidates << base + QStringLiteral("/chromium");
    candidates << base + QStringLiteral("/chromium-browser");
    candidates << base + QStringLiteral("/google-chrome");
  }
  for (const QString& p : candidates) {
    if (QFileInfo::exists(p + QStringLiteral("/Default"))) return p;
  }
  for (const QString& p : candidates) {
    if (QDir(p).exists()) return p;
  }
  return {};
}

bool ChromiumYtLauncher::running() const {
  if (detached_pid_ > 0) return true;
  return proc_.state() != QProcess::NotRunning;
}

qint64 ChromiumYtLauncher::process_id() const {
  if (detached_pid_ > 0) return detached_pid_;
  return running() ? proc_.processId() : 0;
}

bool ChromiumYtLauncher::start() {
  if (running() && shared_session_) {
    // Detached shared Chrome: window may still be open; caller closes via KWin.
    // Do not early-return for isolated — we restart to re-apply scale.
    return true;
  }

  QString bin = browser_path_;
  if (bin.isEmpty()) bin = detectBrowser();
  if (bin.isEmpty()) {
    emit failed(QStringLiteral("Chromium/Chrome not found"));
    return false;
  }
  browser_path_ = bin;

  const int w = window_w_ > 0 ? window_w_ : 1280;
  const int h = window_h_ > 0 ? window_h_ : 720;
  const float scale = device_scale_ >= 1.f ? device_scale_ : 1.f;
  const bool chrome_running = chromeProcessRunning();

  QStringList args;
  shared_session_ = false;
  QString profile_note;

  if (use_default_profile_) {
    // System Chrome profile — logins/cookies for subscriptions.
    // No --user-data-dir: attaches to running Chrome when present.
    args << QStringLiteral("--profile-directory=Default");
    shared_session_ = true;
    profile_note = QStringLiteral("system-default");
    if (chrome_running && scale > 1.01f) {
      VRP_LOG("live: Chrome already running — --force-device-scale-factor=%.2f may be ignored "
              "(quit Chrome first, or set live_chrome_profile=isolated / use live_virtual_scale)",
              static_cast<double>(scale));
    }
  } else {
    // Isolated monasphere profile: own process every launch so scale always applies.
    const QString profile = profileDir();
    QDir().mkpath(profile);
    stop();
    killBrowsersForProfile(profile);
    args << QStringLiteral("--user-data-dir=") + profile;
    args << QStringLiteral("--profile-directory=Default");
    profile_note = profile;
  }

  args << QStringLiteral("--class=monaSphereYT");
  args << QStringLiteral("--name=monaSphereYT");
  args << QStringLiteral("--no-first-run");
  args << QStringLiteral("--no-default-browser-check");
  args << QStringLiteral("--disable-features=TranslateUI,ChromeWhatsNewUI");
  // Passing --force-device-scale-factor=1 on Wayland overrides the output's 200%
  // with 100%; passing virtual_scale on top of output scale doubles it (~400%).
  // follow_output_scale_: omit the flag so DE scale is the single source of truth.
  if (!follow_output_scale_) {
    args << QStringLiteral("--force-device-scale-factor=%1")
                .arg(QString::number(static_cast<double>(scale), 'f', 2));
  }
  args << QStringLiteral("--window-size=%1,%2").arg(w).arg(h);
  if (have_pos_) {
    args << QStringLiteral("--window-position=%1,%2").arg(window_x_).arg(window_y_);
  }
  if (start_minimized_) {
    args << QStringLiteral("--start-minimized");
  }
  if (start_fullscreen_) {
    args << QStringLiteral("--start-fullscreen");
  }
  // Default Wayland so Virtual live_virtual_scale matches desktop Chrome.
  // Opt out: MONASPHERE_YT_X11=1 (old XWayland path; XTest fallback only).
  if (!qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
    if (!qEnvironmentVariableIsEmpty("MONASPHERE_YT_X11")) {
      args << QStringLiteral("--ozone-platform=x11");
    } else {
      args << QStringLiteral("--ozone-platform=wayland");
    }
  }
  // Shared singleton often navigates an existing tab — force a new app window.
  if (shared_session_ || chrome_running) {
    args << QStringLiteral("--new-window");
  }
  args << QStringLiteral("--app=") + url_;

  VRP_LOG("live: launching %s %s (scale=%.2f shared=%d chrome_running=%d profile=%s)",
          bin.toUtf8().constData(), args.join(QLatin1Char(' ')).toUtf8().constData(),
          static_cast<double>(scale), shared_session_ ? 1 : 0, chrome_running ? 1 : 0,
          profile_note.toUtf8().constData());

  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  if (route_audio_to_yt_sink_) {
    // Only when Flat live monitor owns the null sink (enable_audio).
    env.insert(QStringLiteral("PULSE_SINK"), QStringLiteral("monaSphereYt"));
    env.insert(QStringLiteral("PIPEWIRE_NODE"), QStringLiteral("monaSphereYt"));
    if (shared_session_ || chrome_running) {
      VRP_LOG("live: Chrome shared/running — PULSE_SINK may not apply; rely on Pulse move");
    }
  } else {
    // Avoid a leftover monaSphereYt becoming Chrome's (and then the session's)
    // default — that showed up as an unknown “microphone” output in Plasma.
    env.remove(QStringLiteral("PULSE_SINK"));
    env.remove(QStringLiteral("PIPEWIRE_NODE"));
  }

  if (shared_session_) {
    QProcess det;
    det.setProgram(bin);
    det.setArguments(args);
    det.setProcessEnvironment(env);
    qint64 pid = 0;
    if (!det.startDetached(&pid)) {
      emit failed(QStringLiteral("Failed to start Chrome (detached)"));
      return false;
    }
    detached_pid_ = pid > 0 ? pid : 1;
    emit started();
    return true;
  }

  detached_pid_ = 0;
  proc_.setProgram(bin);
  proc_.setArguments(args);
  proc_.setProcessEnvironment(env);
  proc_.start();
  if (!proc_.waitForStarted(5000)) {
    emit failed(proc_.errorString().isEmpty() ? QStringLiteral("Failed to start Chrome")
                                              : proc_.errorString());
    return false;
  }
  return true;
}

void ChromiumYtLauncher::stop() {
  // Window close is handled by KWin / X11 from the session. Never kill a shared
  // system Chrome — that would destroy the user's browser session.
  detached_pid_ = 0;
  if (proc_.state() == QProcess::NotRunning) {
    shared_session_ = false;
    return;
  }
  if (shared_session_) {
    proc_.terminate();
    (void)proc_.waitForFinished(800);
    shared_session_ = false;
    return;
  }
  const QString profile = profileDir();
  proc_.terminate();
  if (!proc_.waitForFinished(1200)) {
    proc_.kill();
    (void)proc_.waitForFinished(400);
  }
  killBrowsersForProfile(profile);
  shared_session_ = false;
}

}  // namespace vrp
