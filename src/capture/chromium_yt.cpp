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

bool profileBrowsersAlive(const QString& profile, LiveBrowser kind) {
  if (profile.isEmpty()) return false;
  QProcess pgrep;
  if (kind == LiveBrowser::Firefox) {
    pgrep.start(QStringLiteral("pgrep"),
                {QStringLiteral("-f"), QStringLiteral("--profile[= ]%1").arg(profile)});
  } else {
    pgrep.start(QStringLiteral("pgrep"),
                {QStringLiteral("-f"), QStringLiteral("user-data-dir=%1").arg(profile)});
  }
  if (!pgrep.waitForFinished(1500) || pgrep.exitCode() != 0) return false;
  return !QString::fromUtf8(pgrep.readAllStandardOutput()).trimmed().isEmpty();
}

void killBrowsersForProfile(const QString& profile, LiveBrowser kind, bool force_kill = false) {
  if (profile.isEmpty()) return;
  QStringList args;
  if (force_kill) args << QStringLiteral("-9");
  args << QStringLiteral("-f");
  if (kind == LiveBrowser::Firefox) {
    // Match both "--profile /path" and "--profile=/path".
    args << QStringLiteral("--profile[= ]%1").arg(profile);
  } else {
    args << QStringLiteral("user-data-dir=%1").arg(profile);
  }
  QProcess pkill;
  pkill.start(QStringLiteral("pkill"), args);
  (void)pkill.waitForFinished(2500);
  QThread::msleep(force_kill ? 80 : 150);
  if (kind != LiveBrowser::Firefox) clearSingletonLocks(profile);
}

void waitProfileBrowsersGone(const QString& profile, LiveBrowser kind) {
  if (profile.isEmpty()) return;
  for (int i = 0; i < 40; ++i) {
    if (!profileBrowsersAlive(profile, kind)) return;
    if (i == 12) {
      VRP_LOG("live: isolated profile still alive — SIGKILL");
      killBrowsersForProfile(profile, kind, /*force_kill=*/true);
    }
    QThread::msleep(100);
  }
  if (profileBrowsersAlive(profile, kind)) {
    VRP_LOG("live: WARNING isolated profile processes still alive after kill (%s)",
            profile.toUtf8().constData());
  }
}

QByteArray readProcPidFile(qint64 pid, const char* name) {
  QFile f(QStringLiteral("/proc/%1/%2").arg(pid).arg(QLatin1String(name)));
  if (!f.open(QIODevice::ReadOnly)) return {};
  return f.readAll();
}

QString chromeCmdline(qint64 pid) {
  QByteArray raw = readProcPidFile(pid, "cmdline");
  for (char& c : raw) {
    if (c == '\0') c = ' ';
  }
  return QString::fromLocal8Bit(raw).trimmed();
}

bool chromeEnvironHas(qint64 pid, const QByteArray& key_eq) {
  const QByteArray raw = readProcPidFile(pid, "environ");
  if (raw.isEmpty()) return false;
  int start = 0;
  while (start < raw.size()) {
    int end = raw.indexOf('\0', start);
    if (end < 0) end = raw.size();
    const QByteArray entry = raw.mid(start, end - start);
    if (entry.startsWith(key_eq)) return true;
    start = end + 1;
  }
  return false;
}

/** True for the browser main process — not crashpad, sandbox, or --type= helpers. */
bool cmdlineLooksLikeChromeMain(const QString& cmd) {
  if (cmd.isEmpty()) return false;
  const QString lower = cmd.toLower();
  if (lower.contains(QLatin1String("--type="))) return false;
  if (lower.contains(QLatin1String("crashpad"))) return false;
  if (lower.contains(QLatin1String("nacl_helper"))) return false;
  if (lower.contains(QLatin1String("chrome_sandbox")) ||
      lower.contains(QLatin1String("chrome-sandbox"))) {
    return false;
  }
  // First argv token = executable. Accept google-chrome*, chromium*, or bare "chrome".
  // Reject chrome_crashpad_handler / chrome_crashpad_handler (underscore helpers).
  const QString exe = cmd.section(QLatin1Char(' '), 0, 0);
  const QString base = QFileInfo(exe).fileName().toLower();
  if (base == QLatin1String("chrome") || base == QLatin1String("chromium") ||
      base == QLatin1String("chromium-browser") || base == QLatin1String("chromium-freeworld") ||
      base.startsWith(QLatin1String("google-chrome"))) {
    return true;
  }
  return false;
}

bool chromeProcessRunning() {
  // Match real browser mains only — chrome_crashpad_handler also matches a naive
  // "chrome" pgrep and is often left behind after quit.
  QProcess pgrep;
  pgrep.start(QStringLiteral("pgrep"),
              {QStringLiteral("-f"), QStringLiteral("[c]hrome|[c]hromium")});
  if (!pgrep.waitForFinished(1500) || pgrep.exitCode() != 0) return false;
  const QString out = QString::fromUtf8(pgrep.readAllStandardOutput());
  for (const QString& ps : out.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
    bool ok = false;
    const qint64 pid = ps.trimmed().toLongLong(&ok);
    if (!ok || pid <= 0) continue;
    if (cmdlineLooksLikeChromeMain(chromeCmdline(pid))) return true;
  }
  return false;
}

bool firefoxProcessRunning() {
  QProcess pgrep;
  pgrep.start(QStringLiteral("pgrep"), {QStringLiteral("-x"), QStringLiteral("firefox")});
  if (!pgrep.waitForFinished(1500) || pgrep.exitCode() != 0) {
    pgrep.start(QStringLiteral("pgrep"),
                {QStringLiteral("-f"), QStringLiteral("[f]irefox")});
    if (!pgrep.waitForFinished(1500) || pgrep.exitCode() != 0) return false;
  }
  return !QString::fromUtf8(pgrep.readAllStandardOutput()).trimmed().isEmpty();
}

QString findExecutable(const QStringList& names, const QStringList& abs_paths) {
  for (const QString& name : names) {
    const QString p = QStandardPaths::findExecutable(name);
    if (!p.isEmpty()) return p;
  }
  for (const QString& p : abs_paths) {
    if (QFileInfo::exists(p)) return p;
  }
  return {};
}

struct WarmState {
  bool owned = false;
  bool isolated = false;
  qint64 pid = 0;
  QString profile;
};

WarmState g_warm;
/** Live cold-started system Chrome (shared profile); quit on Live stop. */
bool g_shared_live_owned = false;

void terminateChromeMainProcesses() {
  QProcess pgrep;
  pgrep.start(QStringLiteral("pgrep"),
              {QStringLiteral("-f"), QStringLiteral("[c]hrome|[c]hromium")});
  if (!pgrep.waitForFinished(1500) || pgrep.exitCode() != 0) return;
  const QString out = QString::fromUtf8(pgrep.readAllStandardOutput());
  QList<qint64> mains;
  for (const QString& ps : out.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
    bool ok = false;
    const qint64 pid = ps.trimmed().toLongLong(&ok);
    if (!ok || pid <= 0) continue;
    const QString cmd = chromeCmdline(pid);
    if (!cmdlineLooksLikeChromeMain(cmd)) continue;
    // Never touch isolated monasphere profile browsers here.
    if (cmd.contains(QLatin1String("monasphere/chromium-yt"))) continue;
    mains.push_back(pid);
  }
  for (qint64 pid : mains) {
    QProcess kill;
    kill.start(QStringLiteral("kill"),
               {QStringLiteral("-TERM"), QStringLiteral("--"), QString::number(pid)});
    (void)kill.waitForFinished(800);
  }
  for (int i = 0; i < 25; ++i) {
    if (!chromeProcessRunning()) return;
    QThread::msleep(80);
  }
  for (qint64 pid : mains) {
    QProcess kill;
    kill.start(QStringLiteral("kill"),
               {QStringLiteral("-KILL"), QStringLiteral("--"), QString::number(pid)});
    (void)kill.waitForFinished(500);
  }
  QThread::msleep(100);
}

void applyChromeOzoneEnv(QProcessEnvironment& env, bool force_x11) {
  if (!force_x11 && qEnvironmentVariableIsEmpty("MONASPHERE_YT_X11")) return;
  env.insert(QStringLiteral("OZONE_PLATFORM"), QStringLiteral("x11"));
  env.insert(QStringLiteral("GDK_BACKEND"), QStringLiteral("x11"));
  env.remove(QStringLiteral("WAYLAND_DISPLAY"));
  env.remove(QStringLiteral("WAYLAND_SOCKET"));
}

bool chromeCmdlineIsMonasphereOwned(const QString& cmd) {
  // Isolated Live/warm profile, or our warm/Live WM_CLASS in argv.
  if (cmd.contains(QLatin1String("monasphere/chromium-yt"))) return true;
  if (cmd.contains(QLatin1String("monaSphereYTWarm"))) return true;
  if (cmd.contains(QLatin1String("--class=monaSphereYT"))) return true;
  return false;
}

bool chromeProcessIsX11Ozone(qint64 pid, const QString& cmd) {
  if (cmd.contains(QLatin1String("--ozone-platform=x11"), Qt::CaseInsensitive)) return true;
  if (chromeEnvironHas(pid, QByteArrayLiteral("OZONE_PLATFORM=x11"))) return true;
  return false;
}

bool chromeProcessIsWaylandOzone(qint64 pid, const QString& cmd) {
  if (cmd.contains(QLatin1String("--ozone-platform=wayland"), Qt::CaseInsensitive)) return true;
  if (chromeEnvironHas(pid, QByteArrayLiteral("OZONE_PLATFORM=wayland"))) return true;
  // Default on a Wayland session: WAYLAND_DISPLAY inherited, no ozone=x11.
  if (chromeEnvironHas(pid, QByteArrayLiteral("WAYLAND_DISPLAY=")) &&
      !chromeProcessIsX11Ozone(pid, cmd)) {
    return true;
  }
  return false;
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

ChromiumYtLauncher::~ChromiumYtLauncher() {
  // Only tear down a Live launch we own. A stack helper used solely for
  // ensureWarm() must not scrub the isolated warm profile on destruction.
  if (proc_.state() != QProcess::NotRunning || detached_pid_ > 0 || shared_session_) {
    stop();
  }
}

QString ChromiumYtLauncher::detectBrowser(LiveBrowser kind) {
  if (kind == LiveBrowser::Firefox) {
    return findExecutable(
        {QStringLiteral("firefox"), QStringLiteral("firefox-bin")},
        {QStringLiteral("/usr/bin/firefox"), QStringLiteral("/usr/lib64/firefox/firefox"),
         QStringLiteral("/usr/lib/firefox/firefox")});
  }
  return findExecutable(
      {QStringLiteral("google-chrome"), QStringLiteral("google-chrome-stable"),
       QStringLiteral("chromium"), QStringLiteral("chromium-browser"),
       QStringLiteral("chromium-freeworld")},
      {QStringLiteral("/usr/bin/google-chrome"), QStringLiteral("/usr/bin/google-chrome-stable"),
       QStringLiteral("/usr/bin/chromium"), QStringLiteral("/usr/bin/chromium-browser")});
}

QString ChromiumYtLauncher::profileDir(LiveBrowser kind) {
  const QString base = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
  if (kind == LiveBrowser::Firefox) {
    return base + QStringLiteral("/monasphere/firefox-yt");
  }
  return base + QStringLiteral("/monasphere/chromium-yt");
}

QString ChromiumYtLauncher::defaultUserDataDir(const QString& browser_path) {
  const QString base = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
  const QString bin = QFileInfo(browser_path).fileName().toLower();
  QStringList candidates;
  if (bin.contains(QLatin1String("firefox"))) {
    candidates << base + QStringLiteral("/mozilla/firefox");
    candidates << base + QStringLiteral("/firefox");
  } else if (bin.contains(QLatin1String("chrome"))) {
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
    // Detached shared browser: window may still be open; caller closes via KWin.
    // Do not early-return for isolated — we restart to re-apply scale.
    return true;
  }

  const bool firefox = browser_ == LiveBrowser::Firefox;
  QString bin = browser_path_;
  if (bin.isEmpty()) bin = detectBrowser(browser_);
  if (bin.isEmpty()) {
    emit failed(firefox ? QStringLiteral("Firefox not found")
                        : QStringLiteral("Chromium/Chrome not found"));
    return false;
  }
  browser_path_ = bin;

  const int w = window_w_ > 0 ? window_w_ : 1280;
  const int h = window_h_ > 0 ? window_h_ : 720;
  const float scale = device_scale_ >= 1.f ? device_scale_ : 1.f;
  const bool already_running = firefox ? firefoxProcessRunning() : chromeProcessRunning();

  QStringList args;
  shared_session_ = false;
  QString profile_note;

  if (firefox) {
    // Never remoting into a running Firefox: --new-window hands off to the existing
    // Wayland process and drops --class / env (MOZ_ENABLE_WAYLAND=0), so KWin rules
    // miss the window and YouTube FS double-scales at live_virtual_scale=2.
    // Always --new-instance so launch flags apply. Wayland FS bug is X11-only OK
    // (Mozilla 1869724) → force XWayland below.
    args << QStringLiteral("--class=monaSphereYT");
    args << QStringLiteral("--new-instance");
    bool use_isolated = !use_default_profile_;
    if (!use_isolated && already_running) {
      // Same profile cannot be opened twice; fall back so Live still gets a real process.
      use_isolated = true;
      VRP_LOG("live: Firefox already running — Live uses isolated profile so class/XWayland apply "
              "(quit Firefox for system-default logins)");
    }
    if (use_isolated) {
      const QString profile = profileDir(LiveBrowser::Firefox);
      QDir().mkpath(profile);
      stop();
      killBrowsersForProfile(profile, LiveBrowser::Firefox);
      args << QStringLiteral("--profile") << profile;
      profile_note = profile;
    } else {
      profile_note = QStringLiteral("system-default-new-instance");
    }
    args << url_;
    shared_session_ = false;
  } else {
    if (use_default_profile_) {
      // System Chrome profile — logins/cookies for subscriptions.
      // No --user-data-dir: attaches to running Chrome when present.
      args << QStringLiteral("--profile-directory=Default");
      shared_session_ = true;
      profile_note = QStringLiteral("system-default");
      if (!already_running) {
        // GNOME Wayland cannot WM_DELETE the Live window — we must quit this
        // browser on stop so Meta teardown does not dump FS windows on the host.
        g_shared_live_owned = true;
        VRP_LOG("live: shared Chrome cold-start — owned (quit on Live stop)");
      } else if (!g_shared_live_owned) {
        VRP_LOG("live: shared Chrome already running (user session — will not quit on stop)");
      }
      if (already_running && scale > 1.01f) {
        VRP_LOG("live: Chrome already running — --force-device-scale-factor=%.2f may be ignored "
                "(quit Chrome first, or set live_chrome_profile=isolated / use live_virtual_scale)",
                static_cast<double>(scale));
      }
    } else {
      // Isolated monasphere profile. If warm (or any) Chrome already holds the
      // profile, skip kill — same precondition as manual pre-open — and attach
      // with --new-window (shared_session_ so stop() does not scrub warm).
      const QString profile = profileDir(LiveBrowser::Chrome);
      QDir().mkpath(profile);
      const bool profile_alive = profileBrowsersAlive(profile, LiveBrowser::Chrome);
      if (!profile_alive) {
        stop();
        killBrowsersForProfile(profile, LiveBrowser::Chrome);
      } else {
        VRP_LOG("live: isolated Chrome already warm/alive — skip profile kill (new-window)");
        shared_session_ = true;
      }
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
    // X11: MONASPHERE_YT_X11=1 or set_force_x11 (GNOME window-position place).
    if (!qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
      if (force_x11_ || !qEnvironmentVariableIsEmpty("MONASPHERE_YT_X11")) {
        args << QStringLiteral("--ozone-platform=x11");
      } else {
        args << QStringLiteral("--ozone-platform=wayland");
      }
    }
    // Shared singleton / warm / already-running: force a new app window.
    if (shared_session_ || already_running) {
      args << QStringLiteral("--new-window");
    }
    args << QStringLiteral("--app=") + url_;
  }

  VRP_LOG("live: launching %s %s (browser=%s scale=%.2f shared=%d running=%d profile=%s)",
          bin.toUtf8().constData(), args.join(QLatin1Char(' ')).toUtf8().constData(),
          firefox ? "firefox" : "chrome", static_cast<double>(scale), shared_session_ ? 1 : 0,
          already_running ? 1 : 0, profile_note.toUtf8().constData());

  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  if (firefox) {
    // Wayland + 200% + (browser or video) fullscreen double-scales YouTube (1869724).
    // XWayland does not; keep WAYLAND_DISPLAY so XWayland can attach to the session.
    env.insert(QStringLiteral("MOZ_ENABLE_WAYLAND"), QStringLiteral("0"));
    VRP_LOG("live: Firefox → XWayland (MOZ_ENABLE_WAYLAND=0) + --new-instance");
  }
  if (!firefox) {
    // Belt-and-suspenders: some Chrome builds ignore --ozone-platform when a
    // Wayland session Chrome is already running. Prefer X11 so Meta place +
    // WM_CLASS are visible to our XQueryTree walker (Wayland surfaces are not).
    applyChromeOzoneEnv(env, force_x11_);
  }
  if (route_audio_to_yt_sink_) {
    // Only when Flat live monitor owns the null sink (enable_audio).
    env.insert(QStringLiteral("PULSE_SINK"), QStringLiteral("monaSphereYt"));
    env.insert(QStringLiteral("PIPEWIRE_NODE"), QStringLiteral("monaSphereYt"));
    if (shared_session_ || already_running) {
      VRP_LOG("live: browser shared/running — PULSE_SINK may not apply; rely on Pulse move");
    }
  } else {
    // Avoid a leftover monaSphereYt becoming the browser's (and then the session's)
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
      emit failed(firefox ? QStringLiteral("Failed to start Firefox (detached)")
                          : QStringLiteral("Failed to start Chrome (detached)"));
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
    emit failed(proc_.errorString().isEmpty()
                    ? (firefox ? QStringLiteral("Failed to start Firefox")
                               : QStringLiteral("Failed to start Chrome"))
                    : proc_.errorString());
    return false;
  }
  return true;
}

void ChromiumYtLauncher::killIsolatedLiveBrowsers(LiveBrowser kind) {
  const QString profile = profileDir(kind);
  killBrowsersForProfile(profile, kind, /*force_kill=*/false);
  waitProfileBrowsersGone(profile, kind);
  if (g_warm.isolated && g_warm.profile == profile) {
    g_warm = WarmState{};
  }
}

bool ChromiumYtLauncher::userWaylandSharedChromeRunning() {
  // X11-only session: system Chrome is not a Wayland singleton.
  if (qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) return false;

  QProcess pgrep;
  pgrep.start(QStringLiteral("pgrep"),
              {QStringLiteral("-f"), QStringLiteral("[c]hrome|[c]hromium")});
  if (!pgrep.waitForFinished(1500) || pgrep.exitCode() != 0) return false;
  const QString out = QString::fromUtf8(pgrep.readAllStandardOutput());
  const QStringList pid_strs = out.split(QLatin1Char('\n'), Qt::SkipEmptyParts);

  bool saw_user_wayland = false;
  for (const QString& ps : pid_strs) {
    bool ok = false;
    const qint64 pid = ps.toLongLong(&ok);
    if (!ok || pid <= 0) continue;
    const QString cmd = chromeCmdline(pid);
    if (!cmdlineLooksLikeChromeMain(cmd)) continue;
    if (chromeCmdlineIsMonasphereOwned(cmd)) continue;
    if (chromeProcessIsX11Ozone(pid, cmd)) continue;
    if (chromeProcessIsWaylandOzone(pid, cmd)) {
      saw_user_wayland = true;
      break;
    }
    // Unspecified ozone on Wayland session — treat as Wayland (GNOME default).
    saw_user_wayland = true;
    break;
  }
  if (saw_user_wayland) {
    VRP_LOG("live: detected user Wayland shared Chrome (blocks GNOME default-profile place)");
  }
  return saw_user_wayland;
}

bool ChromiumYtLauncher::warmOwned() { return g_warm.owned; }

bool ChromiumYtLauncher::warmAlive() const {
  if (browser_ == LiveBrowser::Firefox) return false;
  if (use_default_profile_) return chromeProcessRunning();
  return profileBrowsersAlive(profileDir(LiveBrowser::Chrome), LiveBrowser::Chrome);
}

bool ChromiumYtLauncher::ensureWarm() {
  if (browser_ == LiveBrowser::Firefox) {
    VRP_LOG("live: chrome warm skipped (Firefox — v1 Chrome/Chromium only)");
    return false;
  }
  if (warmAlive()) {
    if (!g_warm.owned) {
      VRP_LOG("live: chrome warm no-op (already running)");
    }
    return true;
  }

  QString bin = browser_path_;
  if (bin.isEmpty()) bin = detectBrowser(LiveBrowser::Chrome);
  if (bin.isEmpty()) {
    VRP_LOG("live: chrome warm failed — Chromium/Chrome not found");
    return false;
  }
  browser_path_ = bin;

  const float scale = device_scale_ >= 1.f ? device_scale_ : 1.f;
  QStringList args;
  QString profile_note;
  g_warm = WarmState{};
  g_warm.isolated = !use_default_profile_;

  if (use_default_profile_) {
    args << QStringLiteral("--profile-directory=Default");
    profile_note = QStringLiteral("system-default");
  } else {
    const QString profile = profileDir(LiveBrowser::Chrome);
    QDir().mkpath(profile);
    clearSingletonLocks(profile);
    args << QStringLiteral("--user-data-dir=") + profile;
    args << QStringLiteral("--profile-directory=Default");
    g_warm.profile = profile;
    profile_note = profile;
  }

  // Distinct class so KWin/GNOME Live place never picks up a warm window if one maps.
  args << QStringLiteral("--class=monaSphereYTWarm");
  args << QStringLiteral("--name=monaSphereYTWarm");
  args << QStringLiteral("--no-first-run");
  args << QStringLiteral("--no-default-browser-check");
  args << QStringLiteral("--disable-features=TranslateUI,ChromeWhatsNewUI");
  if (!follow_output_scale_) {
    args << QStringLiteral("--force-device-scale-factor=%1")
                .arg(QString::number(static_cast<double>(scale), 'f', 2));
  }
  if (!qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
    if (force_x11_ || !qEnvironmentVariableIsEmpty("MONASPHERE_YT_X11")) {
      args << QStringLiteral("--ozone-platform=x11");
    } else {
      args << QStringLiteral("--ozone-platform=wayland");
    }
  }
  // Keep the process alive with no visible window. --app=about:blank +
  // --start-minimized still maps a large host window on GNOME (seen as
  // google-chrome/monaSphereYTWarm Viewable). Live then --new-window --app=URL.
  args << QStringLiteral("--no-startup-window");

  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  applyChromeOzoneEnv(env, force_x11_);
  // Manual pre-open has no yt sink; Live moves streams after Soft starts.
  env.remove(QStringLiteral("PULSE_SINK"));
  env.remove(QStringLiteral("PIPEWIRE_NODE"));

  VRP_LOG("live: chrome warm launching %s (scale=%.2f follow_out=%d isolated=%d profile=%s)",
          bin.toUtf8().constData(), static_cast<double>(scale), follow_output_scale_ ? 1 : 0,
          g_warm.isolated ? 1 : 0, profile_note.toUtf8().constData());

  QProcess det;
  det.setProgram(bin);
  det.setArguments(args);
  det.setProcessEnvironment(env);
  qint64 pid = 0;
  if (!det.startDetached(&pid)) {
    VRP_LOG("live: chrome warm failed to startDetached");
    g_warm = WarmState{};
    return false;
  }
  g_warm.owned = true;
  g_warm.pid = pid > 0 ? pid : 0;
  VRP_LOG("live: chrome warm owned pid=%lld", static_cast<long long>(g_warm.pid));
  return true;
}

void ChromiumYtLauncher::shutdownWarm() {
  if (!g_warm.owned) {
    g_warm = WarmState{};
    return;
  }
  VRP_LOG("live: chrome warm shutdown (owned isolated=%d pid=%lld)", g_warm.isolated ? 1 : 0,
          static_cast<long long>(g_warm.pid));
  if (g_warm.isolated) {
    const QString profile =
        g_warm.profile.isEmpty() ? profileDir(LiveBrowser::Chrome) : g_warm.profile;
    killBrowsersForProfile(profile, LiveBrowser::Chrome);
    waitProfileBrowsersGone(profile, LiveBrowser::Chrome);
  } else if (g_warm.pid > 0) {
    // We cold-started system Chrome for warm — quit that process tree only.
    QProcess kill;
    kill.start(QStringLiteral("kill"),
               {QStringLiteral("--"), QString::number(g_warm.pid)});
    (void)kill.waitForFinished(1500);
    QThread::msleep(120);
    if (chromeProcessRunning()) {
      QProcess pkill;
      pkill.start(QStringLiteral("pkill"),
                  {QStringLiteral("-f"), QStringLiteral("--class=monaSphereYTWarm")});
      (void)pkill.waitForFinished(2000);
    }
  } else {
    QProcess pkill;
    pkill.start(QStringLiteral("pkill"),
                {QStringLiteral("-f"), QStringLiteral("--class=monaSphereYTWarm")});
    (void)pkill.waitForFinished(2000);
  }
  g_warm = WarmState{};
}

bool ChromiumYtLauncher::sharedLiveOwned() { return g_shared_live_owned; }

void ChromiumYtLauncher::quitOwnedSharedChrome() {
  if (!g_shared_live_owned) return;
  VRP_LOG("live: quitting owned shared Chrome");
  g_shared_live_owned = false;
  terminateChromeMainProcesses();
  if (chromeProcessRunning()) {
    VRP_LOG("live: WARNING owned shared Chrome still alive after quit");
  } else {
    VRP_LOG("live: owned shared Chrome gone");
  }
}

void ChromiumYtLauncher::stop() {
  // Window close is handled by KWin / X11 from the session. Never kill a shared
  // system browser the user already had open — only quit one we cold-started.
  detached_pid_ = 0;
  if (shared_session_) {
    // Shared Live uses startDetached + KWin closeWindow; do not wait on a helper
    // QProcess (would stall Live↔Live switch for up to waitForFinished).
    if (proc_.state() != QProcess::NotRunning) {
      proc_.terminate();
      (void)proc_.waitForFinished(100);
    }
    // Default-profile Live we started: quit so Wayland FS windows do not survive
    // Meta teardown and reappear on the host (GNOME Shell.Eval often unavailable).
    if (use_default_profile_) quitOwnedSharedChrome();
    shared_session_ = false;
    return;
  }
  // Isolated: always scrub profile processes — even when our QProcess already
  // exited. Chrome zygotes survive parent death and stay mapped on Meta-*.
  const bool isolated = !use_default_profile_;
  const QString profile = isolated ? profileDir(browser_) : QString();
  if (proc_.state() != QProcess::NotRunning) {
    proc_.terminate();
    if (!proc_.waitForFinished(1200)) {
      proc_.kill();
      (void)proc_.waitForFinished(400);
    }
  }
  if (isolated) {
    killBrowsersForProfile(profile, browser_);
    waitProfileBrowsersGone(profile, browser_);
  }
  shared_session_ = false;
}

}  // namespace vrp
