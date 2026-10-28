#include "capture/kwin_window_placer.hpp"

#include "common.hpp"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>

namespace vrp {
namespace {

constexpr const char* kTempRuleId = "monaSphereYT";
/** Session rule for [アプリ] — matches discovered wmclass (e.g. retroarch). */
constexpr const char* kDeskRuleId = "monaSphereDesk";

QString jsEscape(const QString& s) {
  QString o = s;
  o.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
  o.replace(QLatin1Char('\''), QStringLiteral("\\'"));
  return o;
}

class PlaceReporter : public QObject {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.monaSphere.YtPlace")
 public:
  QString last;
 public slots:
  Q_SCRIPTABLE void report(const QString& msg) {
    last = msg;
    VRP_LOG("kwin-place: script reports: %s", msg.toUtf8().constData());
  }
};

/**
 * Process-wide D-Bus reporter. Nested BusGuard used to unregister the service while
 * the arm script was still alive, so later callDBus reports vanished ("no placement
 * report") and windowAdded ok: messages were lost.
 */
struct PlaceHub {
  PlaceReporter reporter;
  bool ok = false;
  PlaceHub() {
    auto bus = QDBusConnection::sessionBus();
    (void)bus.registerService(QStringLiteral("org.monaSphere.Yt"));
    ok = bus.registerObject(QStringLiteral("/monaSphereYtPlace"),
                            QStringLiteral("org.monaSphere.YtPlace"), &reporter,
                            QDBusConnection::ExportScriptableSlots);
    if (!ok) {
      ok = bus.registerObject(QStringLiteral("/monaSphereYtPlace"), &reporter,
                              QDBusConnection::ExportAllSlots);
    }
  }
};

PlaceHub& placeHub() {
  static PlaceHub hub;
  return hub;
}

struct BusGuard {
  bool ok = false;
  QString snapshot;
  BusGuard() {
    ok = placeHub().ok;
    // Do not clear reporter.last here — arm may still be listening. Callers that
    // need a fresh read should note the previous value or use takeReport().
  }
  QString last() const { return placeHub().reporter.last; }
  void clearLast() { placeHub().reporter.last.clear(); }
};

/** KWin unloadScript(s) takes the *plugin name*, not a D-Bus path or numeric id. */
constexpr const char* kScriptNamePlace = "monaSphereYtPlace";
constexpr const char* kScriptNameOnce = "monaSphereYtOnce";
constexpr const char* kScriptNamePortal = "monaSpherePortalPlace";
constexpr const char* kScriptNamePortalOnce = "monaSpherePortalOnce";

bool unloadScriptByName(const QString& name) {
  if (name.isEmpty()) return true;
  // stop() if we can resolve a running ScriptN — best-effort.
  QDBusMessage loaded = QDBusMessage::createMethodCall(
      QStringLiteral("org.kde.KWin"), QStringLiteral("/Scripting"),
      QStringLiteral("org.kde.kwin.Scripting"), QStringLiteral("isScriptLoaded"));
  loaded << name;
  QDBusReply<bool> is_loaded = QDBusConnection::sessionBus().call(loaded, QDBus::Block, 2000);
  if (is_loaded.isValid() && !is_loaded.value()) return true;

  QDBusMessage msg = QDBusMessage::createMethodCall(
      QStringLiteral("org.kde.KWin"), QStringLiteral("/Scripting"),
      QStringLiteral("org.kde.kwin.Scripting"), QStringLiteral("unloadScript"));
  msg << name;
  QDBusReply<bool> r = QDBusConnection::sessionBus().call(msg, QDBus::Block, 2000);
  return r.isValid() && r.value();
}

bool unloadScript(int script_id, const QString& name) {
  if (script_id >= 0) {
    QDBusMessage stop = QDBusMessage::createMethodCall(
        QStringLiteral("org.kde.KWin"), QStringLiteral("/Scripting/Script%1").arg(script_id),
        QStringLiteral("org.kde.kwin.Script"), QStringLiteral("stop"));
    (void)QDBusConnection::sessionBus().call(stop, QDBus::Block, 2000);
  }
  return unloadScriptByName(name);
}

int loadAndRunScript(const QString& script, QString* error_out, bool keep_loaded,
                     QTemporaryDir** out_keep_dir = nullptr,
                     const char* plugin_name_override = nullptr) {
  QTemporaryDir stack_dir;
  QTemporaryDir* use_dir = &stack_dir;
  QTemporaryDir* heap_dir = nullptr;
  if (keep_loaded) {
    heap_dir = new QTemporaryDir();
    if (!heap_dir->isValid()) {
      if (error_out) *error_out = QStringLiteral("temp dir failed");
      delete heap_dir;
      return -1;
    }
    use_dir = heap_dir;
  } else if (!stack_dir.isValid()) {
    if (error_out) *error_out = QStringLiteral("temp dir failed");
    return -1;
  }
  const QString path = use_dir->filePath(QStringLiteral("monasphere_kwin_place.js"));
  {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
      if (error_out) *error_out = QStringLiteral("cannot write kwin script");
      delete heap_dir;
      return -1;
    }
    QTextStream(&f) << script;
  }

  // Named load returns -1 if that name is already registered. Always unload first.
  // Only unload *this* plugin name — never tear down monaSphereYtPlace while a
  // one-shot (Once / PortalOnce) runs; pollPlace used to kill the armed hook.
  const QString plugin_name =
      plugin_name_override
          ? QString::fromUtf8(plugin_name_override)
          : (keep_loaded ? QString::fromUtf8(kScriptNamePlace) : QString::fromUtf8(kScriptNameOnce));
  (void)unloadScriptByName(plugin_name);

  QDBusMessage load = QDBusMessage::createMethodCall(
      QStringLiteral("org.kde.KWin"), QStringLiteral("/Scripting"),
      QStringLiteral("org.kde.kwin.Scripting"), QStringLiteral("loadScript"));
  load << path << plugin_name;
  QDBusReply<int> load_reply = QDBusConnection::sessionBus().call(load, QDBus::Block, 5000);
  if (!load_reply.isValid() || load_reply.value() < 0) {
    // Fallback: anonymous load (no sticky name).
    (void)unloadScriptByName(plugin_name);
    QDBusMessage load1 = QDBusMessage::createMethodCall(
        QStringLiteral("org.kde.KWin"), QStringLiteral("/Scripting"),
        QStringLiteral("org.kde.kwin.Scripting"), QStringLiteral("loadScript"));
    load1 << path;
    load_reply = QDBusConnection::sessionBus().call(load1, QDBus::Block, 5000);
  }
  if (!load_reply.isValid()) {
    const QString err = load_reply.error().message();
    if (error_out) *error_out = err.isEmpty() ? QStringLiteral("KWin loadScript failed") : err;
    delete heap_dir;
    return -1;
  }
  const int script_id = load_reply.value();
  if (script_id < 0) {
    if (error_out)
      *error_out = QStringLiteral("KWin loadScript returned %1 (name still loaded?)").arg(script_id);
    delete heap_dir;
    return -1;
  }
  QDBusMessage run = QDBusMessage::createMethodCall(
      QStringLiteral("org.kde.KWin"), QStringLiteral("/Scripting/Script%1").arg(script_id),
      QStringLiteral("org.kde.kwin.Script"), QStringLiteral("run"));
  QDBusReply<void> run_reply = QDBusConnection::sessionBus().call(run, QDBus::Block, 5000);
  const bool ok = run_reply.isValid() || run_reply.error().type() == QDBusError::NoError;
  if (!ok && error_out) {
    *error_out = run_reply.error().message();
  }
  if (!keep_loaded) {
    // Wait for callDBus report — no nested QEventLoop (deadlocks poll_armed_place).
    QThread::msleep(250);
    (void)unloadScript(script_id, plugin_name);
    delete heap_dir;
    return ok ? script_id : -1;
  }
  if (!ok) {
    // Keep-loaded arm failed after load — unload or KWin keeps the script forever.
    (void)unloadScript(script_id, plugin_name);
    delete heap_dir;
    if (out_keep_dir) *out_keep_dir = nullptr;
    return -1;
  }
  if (out_keep_dir) *out_keep_dir = heap_dir;
  else delete heap_dir;
  return script_id;
}

bool runScript(const QString& script, QString* error_out) {
  return loadAndRunScript(script, error_out, /*keep_loaded=*/false) >= 0;
}

void kwinReconfigure() {
  QDBusMessage msg = QDBusMessage::createMethodCall(
      QStringLiteral("org.kde.KWin"), QStringLiteral("/KWin"), QStringLiteral("org.kde.KWin"),
      QStringLiteral("reconfigure"));
  (void)QDBusConnection::sessionBus().call(msg, QDBus::Block, 3000);
}

QString rulesPath() {
  return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) +
         QStringLiteral("/kwinrulesrc");
}

/** ruleValue: 2 = Force (see KWin SetRule / ForceRule). */
bool installTempWindowRule(int screen_index, bool session_keep, QString* error_out) {
  if (screen_index < 0) {
    if (error_out) *error_out = QStringLiteral("bad screen index");
    return false;
  }
  QSettings s(rulesPath(), QSettings::IniFormat);
  s.beginGroup(QStringLiteral("General"));
  QStringList order = s.value(QStringLiteral("Order")).toString().split(QLatin1Char(','),
                                                                        Qt::SkipEmptyParts);
  // Plasma 6 also uses "rules=" in some builds.
  QStringList rules = s.value(QStringLiteral("rules")).toString().split(QLatin1Char(','),
                                                                        Qt::SkipEmptyParts);
  s.endGroup();

  auto ensure = [](QStringList& list, const QString& id) {
    if (!list.contains(id)) list.prepend(id);  // evaluate our rule first
  };
  const QString id = QString::fromUtf8(kTempRuleId);
  ensure(order, id);
  ensure(rules, id);

  s.beginGroup(QStringLiteral("General"));
  s.setValue(QStringLiteral("Order"), order.join(QLatin1Char(',')));
  s.setValue(QStringLiteral("rules"), rules.join(QLatin1Char(',')));
  s.setValue(QStringLiteral("count"), order.size());
  s.endGroup();

  s.beginGroup(id);
  s.setValue(QStringLiteral("Description"),
             session_keep ? QStringLiteral("monaSphere live → Force fullscreen on Virtual")
                          : QStringLiteral("monaSphere live → virtual display (place)"));
  // Match --class=monaSphereYT only. Never match a normal browser caption — that would
  // Force-screen the user's already-open Chrome tab onto the virtual display.
  s.setValue(QStringLiteral("wmclass"), QStringLiteral("monaSphereYT"));
  s.setValue(QStringLiteral("wmclasscomplete"), false);
  s.setValue(QStringLiteral("wmclassmatch"), 1);  // Substring
  s.setValue(QStringLiteral("titlematch"), 0);    // Unimportant
  // types=1 → Normal window
  s.setValue(QStringLiteral("types"), 1);
  s.setValue(QStringLiteral("screen"), screen_index);
  s.setValue(QStringLiteral("screenrule"), 2);  // Force → Virtual-*
  if (session_keep) {
    // Session: always Force fullscreen / noborder (not only --start-fullscreen).
    s.setValue(QStringLiteral("minimize"), false);
    s.setValue(QStringLiteral("minimizerule"), 0);
  } else {
    // Place: map minimized once so the primary never flashes; script unminimizes.
    s.setValue(QStringLiteral("minimize"), true);
    s.setValue(QStringLiteral("minimizerule"), 1);  // Apply Initially
  }
  s.setValue(QStringLiteral("fullscreen"), true);
  s.setValue(QStringLiteral("fullscreenrule"), 2);  // Force
  s.setValue(QStringLiteral("noborder"), true);
  s.setValue(QStringLiteral("noborderrule"), 2);  // Force
  s.setValue(QStringLiteral("skiptaskbar"), true);
  s.setValue(QStringLiteral("skiptaskbarrule"), 2);
  s.setValue(QStringLiteral("skippager"), true);
  s.setValue(QStringLiteral("skippagerrule"), 2);
  s.endGroup();
  s.sync();
  if (s.status() != QSettings::NoError) {
    if (error_out) *error_out = QStringLiteral("kwinrulesrc write failed");
    return false;
  }
  kwinReconfigure();
  VRP_LOG("kwin-place: installed Window Rule '%s' Force screen=%d fullscreen noborder keep=%d",
          kTempRuleId, screen_index, session_keep ? 1 : 0);
  return true;
}

void removeTempWindowRuleById(const char* rule_id) {
  QSettings s(rulesPath(), QSettings::IniFormat);
  s.beginGroup(QStringLiteral("General"));
  QStringList order = s.value(QStringLiteral("Order")).toString().split(QLatin1Char(','),
                                                                        Qt::SkipEmptyParts);
  QStringList rules = s.value(QStringLiteral("rules")).toString().split(QLatin1Char(','),
                                                                        Qt::SkipEmptyParts);
  s.endGroup();
  const QString id = QString::fromUtf8(rule_id);
  order.removeAll(id);
  rules.removeAll(id);
  s.beginGroup(QStringLiteral("General"));
  s.setValue(QStringLiteral("Order"), order.join(QLatin1Char(',')));
  s.setValue(QStringLiteral("rules"), rules.join(QLatin1Char(',')));
  s.setValue(QStringLiteral("count"), order.size());
  s.endGroup();
  s.remove(id);
  s.sync();
  kwinReconfigure();
  VRP_LOG("kwin-place: removed temporary Window Rule '%s'", rule_id);
}

void removeTempWindowRule() { removeTempWindowRuleById(kTempRuleId); }

bool installDesktopWindowRuleLocked(const QString& wmclass, int screen_index, QString* error_out) {
  if (wmclass.isEmpty() || screen_index < 0) {
    if (error_out) *error_out = QStringLiteral("bad wmclass/screen");
    return false;
  }
  QSettings s(rulesPath(), QSettings::IniFormat);
  s.beginGroup(QStringLiteral("General"));
  QStringList order = s.value(QStringLiteral("Order")).toString().split(QLatin1Char(','),
                                                                        Qt::SkipEmptyParts);
  QStringList rules = s.value(QStringLiteral("rules")).toString().split(QLatin1Char(','),
                                                                        Qt::SkipEmptyParts);
  s.endGroup();

  auto ensure = [](QStringList& list, const QString& id) {
    if (!list.contains(id)) list.prepend(id);
  };
  const QString id = QString::fromUtf8(kDeskRuleId);
  ensure(order, id);
  ensure(rules, id);

  s.beginGroup(QStringLiteral("General"));
  s.setValue(QStringLiteral("Order"), order.join(QLatin1Char(',')));
  s.setValue(QStringLiteral("rules"), rules.join(QLatin1Char(',')));
  s.setValue(QStringLiteral("count"), order.size());
  s.endGroup();

  s.beginGroup(id);
  s.setValue(QStringLiteral("Description"),
             QStringLiteral("monaSphere [アプリ] → Force screen on Virtual (app owns fullscreen)"));
  // After map only (KWin Wayland ignores many pre-start Force-screen rules).
  s.setValue(QStringLiteral("wmclass"), wmclass);
  s.setValue(QStringLiteral("wmclasscomplete"), true);
  s.setValue(QStringLiteral("wmclassmatch"), 1);  // ExactMatch
  s.setValue(QStringLiteral("titlematch"), 0);
  s.setValue(QStringLiteral("types"), 1);
  s.setValue(QStringLiteral("screen"), screen_index);
  s.setValue(QStringLiteral("screenrule"), 2);  // Force → Virtual-*
  s.setValue(QStringLiteral("fullscreenrule"), 0);
  s.setValue(QStringLiteral("noborderrule"), 0);
  s.remove(QStringLiteral("skiptaskbar"));
  s.remove(QStringLiteral("skiptaskbarrule"));
  s.remove(QStringLiteral("skippager"));
  s.remove(QStringLiteral("skippagerrule"));
  s.endGroup();
  s.sync();
  if (s.status() != QSettings::NoError) {
    if (error_out) *error_out = QStringLiteral("kwinrulesrc write failed");
    return false;
  }
  kwinReconfigure();
  VRP_LOG("kwin-place: installed Desk Window Rule complete='%s' Force screen=%d",
          wmclass.toUtf8().constData(), screen_index);
  return true;
}

QString jsHelpers() {
  return QStringLiteral(
      "function report(msg) {\n"
      "  try {\n"
      "    callDBus('org.monaSphere.Yt', '/monaSphereYtPlace',\n"
      "             'org.monaSphere.YtPlace', 'report', msg);\n"
      "  } catch (e) { print('monaSphere report failed: ' + e); }\n"
      "}\n"
      "function isChrome(w) {\n"
      "  const rc = (w.resourceClass || '').toLowerCase();\n"
      "  const rn = (w.resourceName || '').toLowerCase();\n"
      "  if (rc.indexOf('monasphereyt') >= 0 || rn.indexOf('monasphereyt') >= 0) return true;\n"
      "  if (rn.indexOf('crx_') === 0) return true;\n"
      "  if (rc.indexOf('chrome') >= 0 || rc.indexOf('chromium') >= 0) return true;\n"
      "  if (rn.indexOf('chrome') >= 0 || rn.indexOf('chromium') >= 0) return true;\n"
      "  return false;\n"
      "}\n"
      "function isOurs(w) {\n"
      "  if (!isChrome(w)) return false;\n"
      "  const rc = (w.resourceClass || '').toLowerCase();\n"
      "  const rn = (w.resourceName || '').toLowerCase();\n"
      "  // Dedicated --class / --app id only. Do NOT treat a normal tab caption as ours:\n"
      "  // that steals the user's already-open Chrome tab window.\n"
      "  if (rc.indexOf('monasphereyt') >= 0 || rn.indexOf('monasphereyt') >= 0) return true;\n"
      "  if (rn.indexOf('crx_') === 0) return true;\n"
      "  return false;\n"
      "}\n"
      "function isNormalWin(w) {\n"
      "  if (!w || w.deleted) return false;\n"
      "  try { if (w.desktopWindow) return false; } catch (e0) {}\n"
      "  try { if (w.dock) return false; } catch (e1) {}\n"
      "  try { if (w.toolbar) return false; } catch (e2) {}\n"
      "  try { if (w.menu) return false; } catch (e3) {}\n"
      "  try { if (w.splash) return false; } catch (e4) {}\n"
      "  try { if (w.notification) return false; } catch (e5) {}\n"
      "  try { if (w.criticalNotification) return false; } catch (e6) {}\n"
      "  try { if (w.appletPopup) return false; } catch (e7) {}\n"
      "  try { if (typeof w.normalWindow === 'boolean' && !w.normalWindow) return false; } catch (e8) {}\n"
      "  return true;\n"
      "}\n"
      "function findOutput(needle) {\n"
      "  const screens = workspace.screens;\n"
      "  for (let i = 0; i < screens.length; ++i) {\n"
      "    const n = (screens[i].name || '').toLowerCase();\n"
      "    if (n.indexOf(needle) >= 0) return screens[i];\n"
      "  }\n"
      "  return null;\n"
      "}\n"
      "function outputIndex(out) {\n"
      "  const screens = workspace.screens;\n"
      "  for (let i = 0; i < screens.length; ++i) if (screens[i] === out) return i;\n"
      "  for (let i = 0; i < screens.length; ++i) {\n"
      "    if ((screens[i].name || '') === (out.name || '')) return i;\n"
      "  }\n"
      "  return -1;\n"
      "}\n"
      // Chrome live: minimize while moving, then force compositor fullscreen.
      "function placeOnOutput(w, targetOut) {\n"
      "  try {\n"
      "    w.fullScreen = false;\n"
      "    w.minimized = true;\n"
      "    if (typeof workspace.setMaximize === 'function')\n"
      "      workspace.setMaximize(w, false, false);\n"
      "    try {\n"
      "      if (typeof workspace.currentDesktopForScreen === 'function') {\n"
      "        const desk = workspace.currentDesktopForScreen(targetOut);\n"
      "        if (desk) w.desktops = [desk];\n"
      "      }\n"
      "    } catch (eD) {}\n"
      "    if (typeof workspace.sendClientToScreen === 'function')\n"
      "      workspace.sendClientToScreen(w, targetOut);\n"
      "    try { w.output = targetOut; } catch (eO) {}\n"
      "    w.skipTaskbar = true;\n"
      "    w.skipPager = true;\n"
      "    w.skipSwitcher = true;\n"
      "    w.minimized = false;\n"
      "    try { w.noBorder = true; } catch (eNb) {}\n"
      "    w.fullScreen = true;\n"
      "    if (typeof workspace.sendClientToScreen === 'function')\n"
      "      workspace.sendClientToScreen(w, targetOut);\n"
      "    try { w.output = targetOut; } catch (eO2) {}\n"
      "    try { w.noBorder = true; } catch (eNb2) {}\n"
      "    w.fullScreen = true;\n"
      "    return true;\n"
      "  } catch (e) { print(e); return false; }\n"
      "}\n"
      // [アプリ]: leave FS so sendClientToScreen sticks; do NOT minimize — GL apps\n"
      // (RetroArch) go permanently black after minimize/unminimize on Virtual.\n"
      "function desktopOnTarget(w, targetOut) {\n"
      "  try {\n"
      "    return !!(w.output && targetOut && String(w.output.name) === String(targetOut.name));\n"
      "  } catch (e) { return false; }\n"
      "}\n"
      "function placeOnOutputDesktop(w, targetOut) {\n"
      "  try {\n"
      "    if (desktopOnTarget(w, targetOut)) return true;  // Window Rule already pinned\n"
      "    const wasFs = !!w.fullScreen;\n"
      "    try { w.fullScreen = false; } catch (eFs0) {}\n"
      "    if (typeof workspace.setMaximize === 'function')\n"
      "      workspace.setMaximize(w, false, false);\n"
      "    try {\n"
      "      if (typeof workspace.currentDesktopForScreen === 'function') {\n"
      "        const desk = workspace.currentDesktopForScreen(targetOut);\n"
      "        if (desk) w.desktops = [desk];\n"
      "      }\n"
      "    } catch (eD) {}\n"
      "    if (typeof workspace.sendClientToScreen === 'function')\n"
      "      workspace.sendClientToScreen(w, targetOut);\n"
      "    try { w.output = targetOut; } catch (eO) {}\n"
      "    if (wasFs) { try { w.fullScreen = true; } catch (eFs1) {} }\n"
      "    if (typeof workspace.sendClientToScreen === 'function')\n"
      "      workspace.sendClientToScreen(w, targetOut);\n"
      "    try { w.output = targetOut; } catch (eO2) {}\n"
      "    return true;\n"
      "  } catch (e) { print(e); return false; }\n"
      "}\n");
}

QString jsExcludeArray(const QStringList& exclude_ids) {
  QString excl = QStringLiteral("[");
  for (int i = 0; i < exclude_ids.size(); ++i) {
    if (i) excl += QLatin1Char(',');
    excl += QLatin1Char('\'') + jsEscape(exclude_ids[i]) + QLatin1Char('\'');
  }
  excl += QLatin1Char(']');
  return excl;
}

QString jsExcludedFn() {
  return QStringLiteral(
      "function excluded(id) {\n"
      "  const s = String(id);\n"
      "  for (let i = 0; i < exclude.length; ++i) if (exclude[i] === s) return true;\n"
      "  return false;\n"
      "}\n"
      "function scoreWin(w, idx, total) {\n"
      "  const id = String(w.internalId);\n"
      "  if (excluded(id)) return -1;\n"
      "  if (!isChrome(w)) return -1;\n"
      "  const fg = w.frameGeometry;\n"
      "  const a = Math.max(1, fg.width) * Math.max(1, fg.height);\n"
      "  const mini = !!w.minimized;\n"
      "  // --start-minimized often reports tiny geometry; still placeable.\n"
      "  if (!mini && a < 10000) return -1;\n"
      "  const rc = (w.resourceClass || '').toLowerCase();\n"
      "  const rn = (w.resourceName || '').toLowerCase();\n"
      "  let score = (total - idx);\n"
      "  // Prefer --class=monaSphereYT / --app, but also accept any brand-new Chrome\n"
      "  // (!excluded). Default-profile singleton often drops --class on Wayland.\n"
      "  if (isOurs(w)) {\n"
      "    score += 200000;\n"
      "    if (rc.indexOf('monasphereyt') >= 0 || rn.indexOf('monasphereyt') >= 0) score += 100000;\n"
      "    if (rn.indexOf('crx_') === 0) score += 80000;\n"
      "  } else {\n"
      "    score += 50000;\n"
      "  }\n"
      "  return score;\n"
      "}\n");
}

struct ArmState {
  QMutex mu;
  bool armed = false;
  int script_id = -1;
  QString last_report;
  QTemporaryDir* script_dir = nullptr;
  bool rule_installed = false;
  int rule_screen_index = -1;
  QStringList exclude_ids;
  QString output_substr;
  ~ArmState() {
    // Process exit while still armed — free temp dir; best-effort KWin unload.
    if (script_id >= 0 || script_dir) {
      (void)unloadScript(script_id, QString::fromUtf8(kScriptNamePlace));
      script_id = -1;
    }
    delete script_dir;
    script_dir = nullptr;
  }
};
ArmState& arm() {
  static ArmState s;
  return s;
}

struct PortalArmState {
  QMutex mu;
  bool armed = false;
  int script_id = -1;
  QTemporaryDir* script_dir = nullptr;
  ~PortalArmState() {
    if (script_id >= 0 || script_dir) {
      (void)unloadScript(script_id, QString::fromUtf8(kScriptNamePortal));
      script_id = -1;
    }
    delete script_dir;
    script_dir = nullptr;
  }
};
PortalArmState& portalArm() {
  static PortalArmState s;
  return s;
}

void disarmLocked(bool keep_fullscreen_rule) {
  if (arm().script_id >= 0 || arm().script_dir) {
    (void)unloadScript(arm().script_id, QString::fromUtf8(kScriptNamePlace));
    arm().script_id = -1;
  }
  (void)unloadScriptByName(QString::fromUtf8(kScriptNamePlace));
  arm().armed = false;
  arm().last_report.clear();
  arm().exclude_ids.clear();
  arm().output_substr.clear();
  delete arm().script_dir;
  arm().script_dir = nullptr;
  if (!arm().rule_installed) return;
  if (keep_fullscreen_rule && arm().rule_screen_index >= 0) {
    QString err;
    if (installTempWindowRule(arm().rule_screen_index, /*session_keep=*/true, &err)) {
      VRP_LOG("kwin-place: kept Force fullscreen rule (screen=%d)", arm().rule_screen_index);
      return;
    }
    VRP_LOG("kwin-place: keep fullscreen rule failed (%s) — removing", err.toUtf8().constData());
  }
  removeTempWindowRule();
  arm().rule_installed = false;
  arm().rule_screen_index = -1;
}

void disarmPortalLocked() {
  if (portalArm().script_id >= 0 || portalArm().script_dir) {
    (void)unloadScript(portalArm().script_id, QString::fromUtf8(kScriptNamePortal));
    portalArm().script_id = -1;
  }
  (void)unloadScriptByName(QString::fromUtf8(kScriptNamePortal));
  portalArm().armed = false;
  delete portalArm().script_dir;
  portalArm().script_dir = nullptr;
}

/** Detect + move xdg-desktop-portal / ScreenCast dialogs to a non-Virtual output. */
QString jsPortalHelpers() {
  return QStringLiteral(
      "function isVirtualOut(out) {\n"
      "  const n = ((out && out.name) || '').toLowerCase();\n"
      "  return n.indexOf('virtual') >= 0 || n.indexOf('monasphere') >= 0;\n"
      "}\n"
      "function findPrimaryOut() {\n"
      "  const screens = workspace.screens;\n"
      "  let first = null;\n"
      "  for (let i = 0; i < screens.length; ++i) {\n"
      "    if (isVirtualOut(screens[i])) continue;\n"
      "    if (!first) first = screens[i];\n"
      "    const n = (screens[i].name || '').toLowerCase();\n"
      "    if (n.indexOf('edp') >= 0 || n.indexOf('hdmi') >= 0 || n.indexOf('dp-') === 0 ||\n"
      "        n.indexOf('displayport') >= 0)\n"
      "      return screens[i];\n"
      "  }\n"
      "  return first || (screens.length ? screens[0] : null);\n"
      "}\n"
      "function isPortalDialog(w) {\n"
      "  if (!w || w.deleted) return false;\n"
      "  const rc = (w.resourceClass || '').toLowerCase();\n"
      "  const rn = (w.resourceName || '').toLowerCase();\n"
      "  const cap = (w.caption || '').toLowerCase();\n"
      "  const desk = String(w.desktopFileName || '').toLowerCase();\n"
      "  if (rc.indexOf('xdg-desktop-portal') >= 0 || rn.indexOf('xdg-desktop-portal') >= 0)\n"
      "    return true;\n"
      "  if (desk.indexOf('xdg-desktop-portal') >= 0) return true;\n"
      "  if (rc.indexOf('portal') >= 0 && (rc.indexOf('kde') >= 0 || rc.indexOf('gtk') >= 0))\n"
      "    return true;\n"
      "  // Caption heuristics (EN / JP) — dialog often maps with a generic class first.\n"
      "  if (cap.indexOf('screen sharing') >= 0 || cap.indexOf('share this') >= 0) return true;\n"
      "  if (cap.indexOf('select what to share') >= 0 || cap.indexOf('choose what to share') >= 0)\n"
      "    return true;\n"
      "  if (cap.indexOf('画面共有') >= 0 || cap.indexOf('画面を共有') >= 0) return true;\n"
      "  if (cap.indexOf('共有する内容') >= 0 || cap.indexOf('共有する画面') >= 0) return true;\n"
      "  if (cap.indexOf('リモート制御') >= 0) return true;\n"
      "  return false;\n"
      "}\n"
      "function placePortalOnPrimary(w, targetOut) {\n"
      "  try {\n"
      "    const cur = w.output || null;\n"
      "    if (cur && !isVirtualOut(cur) && cur === targetOut) {\n"
      "      try {\n"
      "        if (typeof workspace.activateWindow === 'function') workspace.activateWindow(w);\n"
      "        else workspace.activeWindow = w;\n"
      "      } catch (eA) {}\n"
      "      return false;  // already on primary\n"
      "    }\n"
      "    w.minimized = false;\n"
      "    if (typeof workspace.sendClientToScreen === 'function')\n"
      "      workspace.sendClientToScreen(w, targetOut);\n"
      "    try { w.output = targetOut; } catch (eO) {}\n"
      "    try {\n"
      "      if (typeof workspace.activateWindow === 'function') workspace.activateWindow(w);\n"
      "      else workspace.activeWindow = w;\n"
      "    } catch (eA2) {}\n"
      "    return true;\n"
      "  } catch (e) { print(e); return false; }\n"
      "}\n");
}

QString portalArmScript() {
  return jsHelpers() + jsPortalHelpers() +
         QStringLiteral(
             "const targetOut = findPrimaryOut();\n"
             "if (!targetOut) { report('fail:no-primary'); }\n"
             "else {\n"
             "  function tryMove(w, why) {\n"
             "    if (!isPortalDialog(w)) return false;\n"
             "    if (placePortalOnPrimary(w, targetOut)) {\n"
             "      report('portal-ok:' + String(w.internalId) + ':' + targetOut.name + ':' + why);\n"
             "      return true;\n"
             "    }\n"
             "    return false;\n"
             "  }\n"
             "  function scan(why) {\n"
             "    const wins = (typeof workspace.windowList === 'function')\n"
             "        ? workspace.windowList() : workspace.windowList;\n"
             "    for (let i = 0; i < wins.length; ++i) tryMove(wins[i], why);\n"
             "  }\n"
             "  function watch(w) {\n"
             "    tryMove(w, 'added');\n"
             "    if (!w) return;\n"
             "    try {\n"
             "      if (w.captionChanged && w.captionChanged.connect)\n"
             "        w.captionChanged.connect(function() { tryMove(w, 'caption'); });\n"
             "    } catch (eC) {}\n"
             "  }\n"
             "  try { workspace.windowAdded.connect(watch); } catch (eA) {}\n"
             "  try { workspace.clientAdded.connect(watch); } catch (eB) {}\n"
             "  report('portal-armed:' + targetOut.name);\n"
             "  scan('existing');\n"
             "  try {\n"
             "    const timer = new QTimer();\n"
             "    let ticks = 0;\n"
             "    timer.interval = 250;\n"
             "    timer.triggered.connect(function() {\n"
             "      scan('timer');\n"
             "      if (++ticks > 240) timer.stop();  // ~60s\n"
             "    });\n"
             "    timer.start();\n"
             "  } catch (eT) { print('monaSphere portal timer: ' + eT); }\n"
             "}\n");
}

QString portalMoveOnceScript() {
  return jsHelpers() + jsPortalHelpers() +
         QStringLiteral(
             "const targetOut = findPrimaryOut();\n"
             "if (!targetOut) { report('fail:no-primary'); }\n"
             "else {\n"
             "  const wins = (typeof workspace.windowList === 'function')\n"
             "      ? workspace.windowList() : workspace.windowList;\n"
             "  let n = 0;\n"
             "  for (let i = 0; i < wins.length; ++i) {\n"
             "    const w = wins[i];\n"
             "    if (!isPortalDialog(w)) continue;\n"
             "    if (placePortalOnPrimary(w, targetOut)) ++n;\n"
             "  }\n"
             "  report(n > 0 ? ('portal-moved:' + n + ':' + targetOut.name)\n"
             "               : ('portal-none:' + targetOut.name));\n"
             "}\n");
}

/** Ask KWin for the screen index of the named output (for Window Rules). */
int queryOutputIndex(const QString& output_substr, QString* error_out) {
  BusGuard guard;
  guard.clearLast();
  const QString script =
      jsHelpers() +
      QStringLiteral(
          "const out = findOutput('%1');\n"
          "if (!out) report('fail:no-output');\n"
          "else report('idx:' + outputIndex(out) + ':' + out.name);\n")
          .arg(jsEscape(output_substr.toLower()));
  if (!runScript(script, error_out)) return -1;
  if (!guard.last().startsWith(QLatin1String("idx:"))) {
    if (error_out) *error_out = guard.last().isEmpty() ? QStringLiteral("empty report") : guard.last();
    return -1;
  }
  const QString rest = guard.last().mid(4);
  const int colon = rest.indexOf(QLatin1Char(':'));
  const int idx = (colon >= 0 ? rest.left(colon) : rest).toInt();
  return idx;
}

}  // namespace

QStringList KwinWindowPlacer::list_candidate_ids(QString* error_out) {
  BusGuard guard;
  guard.clearLast();
  // All Chrome-family windows — empty-caption --app windows must be excluded too.
  const QString script = jsHelpers() + QStringLiteral(
      "const wins = (typeof workspace.windowList === 'function')\n"
      "    ? workspace.windowList() : workspace.windowList;\n"
      "const ids = [];\n"
      "for (let i = 0; i < wins.length; ++i) {\n"
      "  const w = wins[i];\n"
      "  if (!w || w.deleted || !isChrome(w)) continue;\n"
      "  ids.push(String(w.internalId));\n"
      "}\n"
      "report('ids:' + ids.join(','));\n");
  if (!runScript(script, error_out)) return {};
  QStringList out;
  if (guard.last().startsWith(QLatin1String("ids:"))) {
    const QString body = guard.last().mid(4);
    if (!body.isEmpty()) out = body.split(QLatin1Char(','), Qt::SkipEmptyParts);
  }
  return out;
}

bool KwinWindowPlacer::query_window_wmclass(const QString& internal_id, QString* resource_class,
                                            QString* resource_name, QString* error_out) {
  if (internal_id.isEmpty()) {
    if (error_out) *error_out = QStringLiteral("empty id");
    return false;
  }
  BusGuard guard;
  guard.clearLast();
  const QString script =
      jsHelpers() +
      QStringLiteral(
          "const want = '%1';\n"
          "const wins = (typeof workspace.windowList === 'function')\n"
          "    ? workspace.windowList() : workspace.windowList;\n"
          "for (let i = 0; i < wins.length; ++i) {\n"
          "  const w = wins[i];\n"
          "  if (!w || w.deleted) continue;\n"
          "  if (String(w.internalId) !== want) continue;\n"
          "  const rc = String(w.resourceClass || '');\n"
          "  const rn = String(w.resourceName || '');\n"
          "  report('class:' + rc + '\\n' + rn);\n"
          "  break;\n"
          "}\n")
          .arg(jsEscape(internal_id));
  if (!runScript(script, error_out)) return false;
  const QString rep = guard.last();
  if (!rep.startsWith(QLatin1String("class:"))) {
    if (error_out) *error_out = rep.isEmpty() ? QStringLiteral("window not found") : rep;
    return false;
  }
  const QString body = rep.mid(6);
  const int nl = body.indexOf(QLatin1Char('\n'));
  const QString rc = (nl >= 0 ? body.left(nl) : body).trimmed();
  const QString rn = (nl >= 0 ? body.mid(nl + 1) : QString()).trimmed();
  if (resource_class) *resource_class = rc;
  if (resource_name) *resource_name = rn;
  return !rc.isEmpty() || !rn.isEmpty();
}

namespace {
QString g_desk_rule_class;
QString g_desk_rule_output;
}  // namespace

int KwinWindowPlacer::query_output_index(const QString& output_substr, QString* error_out) {
  return queryOutputIndex(output_substr, error_out);
}

bool KwinWindowPlacer::install_desktop_window_rule(const QString& wmclass, int screen_index,
                                                   QString* error_out) {
  if (wmclass.isEmpty()) {
    if (error_out) *error_out = QStringLiteral("empty wmclass");
    return false;
  }
  if (screen_index < 0) {
    if (error_out) *error_out = QStringLiteral("bad screen index");
    return false;
  }
  const QString key = QStringLiteral("idx:%1").arg(screen_index);
  if (g_desk_rule_class.compare(wmclass, Qt::CaseInsensitive) == 0 &&
      g_desk_rule_output == key) {
    VRP_LOG("kwin-place: Desk Window Rule already active class='%s' screen=%d",
            wmclass.toUtf8().constData(), screen_index);
    return true;
  }
  if (!installDesktopWindowRuleLocked(wmclass, screen_index, error_out)) return false;
  g_desk_rule_class = wmclass;
  g_desk_rule_output = key;
  return true;
}

bool KwinWindowPlacer::install_desktop_window_rule(const QString& wmclass,
                                                   const QString& output_substr,
                                                   QString* error_out) {
  if (wmclass.isEmpty()) {
    if (error_out) *error_out = QStringLiteral("empty wmclass");
    return false;
  }
  if (g_desk_rule_class.compare(wmclass, Qt::CaseInsensitive) == 0 &&
      g_desk_rule_output.compare(output_substr, Qt::CaseInsensitive) == 0) {
    VRP_LOG("kwin-place: Desk Window Rule already active class='%s'",
            wmclass.toUtf8().constData());
    return true;
  }
  QString idx_err;
  const int screen_idx = queryOutputIndex(output_substr, &idx_err);
  if (screen_idx < 0) {
    if (error_out)
      *error_out = idx_err.isEmpty() ? QStringLiteral("no output index") : idx_err;
    return false;
  }
  if (!installDesktopWindowRuleLocked(wmclass, screen_idx, error_out)) return false;
  g_desk_rule_class = wmclass;
  g_desk_rule_output = output_substr;
  return true;
}

void KwinWindowPlacer::remove_desktop_window_rule() {
  removeTempWindowRuleById(kDeskRuleId);
  g_desk_rule_class.clear();
  g_desk_rule_output.clear();
}

QStringList KwinWindowPlacer::list_all_window_ids(QString* error_out) {
  BusGuard guard;
  guard.clearLast();
  const QString script = jsHelpers() + QStringLiteral(
      "const wins = (typeof workspace.windowList === 'function')\n"
      "    ? workspace.windowList() : workspace.windowList;\n"
      "const ids = [];\n"
      "for (let i = 0; i < wins.length; ++i) {\n"
      "  const w = wins[i];\n"
      "  if (!isNormalWin(w)) continue;\n"
      "  ids.push(String(w.internalId));\n"
      "}\n"
      "report('ids:' + ids.join(','));\n");
  if (!runScript(script, error_out)) return {};
  QStringList out;
  if (guard.last().startsWith(QLatin1String("ids:"))) {
    const QString body = guard.last().mid(4);
    if (!body.isEmpty()) out = body.split(QLatin1Char(','), Qt::SkipEmptyParts);
  }
  return out;
}

bool KwinWindowPlacer::arm_place_on_open(const QString& output_substr,
                                         const QStringList& exclude_ids, QString* error_out,
                                         int screen_index) {
  if (output_substr.isEmpty()) {
    if (error_out) *error_out = QStringLiteral("empty output");
    return false;
  }
  // Disarm under lock, then release before any D-Bus / script work. Holding the
  // mutex across nested event processing deadlocks with poll_armed_place (ABRT).
  {
    QMutexLocker lock(&arm().mu);
    disarmLocked(/*keep_fullscreen_rule=*/false);
  }

  if (!placeHub().ok) {
    if (error_out) *error_out = QStringLiteral("D-Bus place reporter failed");
    return false;
  }

  bool rule_ok = false;
  QString idx_err;
  int screen_idx = screen_index;
  if (screen_idx < 0) screen_idx = queryOutputIndex(output_substr, &idx_err);
  if (screen_idx >= 0) {
    QString rule_err;
    if (installTempWindowRule(screen_idx, /*session_keep=*/false, &rule_err)) {
      rule_ok = true;
    } else {
      VRP_LOG("kwin-place: Window Rule install failed: %s", rule_err.toUtf8().constData());
    }
  } else {
    VRP_LOG("kwin-place: could not resolve screen index (%s) — scripting only",
            idx_err.toUtf8().constData());
  }

  placeHub().reporter.last.clear();

  // windowAdded + QTimer scan — XWayland Chrome often maps after the signal, or
  // with an empty class on the first tick; polling catches it within ~1s.
  const QString script =
      jsHelpers() +
      QStringLiteral(
          "const outNeedle = '%1'.toLowerCase();\n"
          "const exclude = %2;\n")
          .arg(jsEscape(output_substr.toLower()), jsExcludeArray(exclude_ids)) +
      jsExcludedFn() +
      QStringLiteral(
          "const targetOut = findOutput(outNeedle);\n"
          "if (!targetOut) { report('fail:no-output'); }\n"
          "else {\n"
          "  let done = false;\n"
          "  function tryPlace(w, why) {\n"
          "    if (done || !w || w.deleted) return false;\n"
          "    if (!isChrome(w)) return false;\n"
          "    if (excluded(w.internalId)) return false;\n"
          "    // Any brand-new Chrome (!excluded). Singleton often keeps class=google-chrome.\n"
          "    if (placeOnOutput(w, targetOut)) {\n"
          "      done = true;\n"
          "      const tag = isOurs(w) ? 'ours' : 'newchrome';\n"
          "      report('ok:' + String(w.internalId) + ':' + targetOut.name + ':' + why + ':' + tag);\n"
          "      return true;\n"
          "    }\n"
          "    return false;\n"
          "  }\n"
          "  function scan(why) {\n"
          "    if (done) return;\n"
          "    const wins = (typeof workspace.windowList === 'function')\n"
          "        ? workspace.windowList() : workspace.windowList;\n"
          "    let best = null, bestScore = -1;\n"
          "    for (let i = wins.length - 1; i >= 0; --i) {\n"
          "      const w = wins[i];\n"
          "      if (!w || w.deleted) continue;\n"
          "      const sc = scoreWin(w, i, wins.length);\n"
          "      if (sc > bestScore) { bestScore = sc; best = w; }\n"
          "    }\n"
          "    if (best) tryPlace(best, why);\n"
          "  }\n"
          "  function watch(w) {\n"
          "    tryPlace(w, 'added');\n"
          "    if (done || !w) return;\n"
          "    try {\n"
          "      if (w.captionChanged && w.captionChanged.connect)\n"
          "        w.captionChanged.connect(function() { tryPlace(w, 'caption'); });\n"
          "    } catch (eC) {}\n"
          "  }\n"
          "  try { workspace.windowAdded.connect(watch); } catch (eA) {}\n"
          "  try { workspace.clientAdded.connect(watch); } catch (eB) {}\n"
          "  // Do NOT scan('existing') — that stole already-open Chrome when exclude\n"
          "  // was empty or incomplete. Only new windows + dedicated app ids.\n"
          "  report('armed:' + targetOut.name + ':screen' + outputIndex(targetOut));\n"
          "  try {\n"
          "    const timer = new QTimer();\n"
          "    timer.interval = 300;\n"
          "    timer.triggered.connect(function() {\n"
          "      if (done) { timer.stop(); return; }\n"
          "      scan('timer');\n"
          "    });\n"
          "    timer.start();\n"
          "  } catch (eT) { print('monaSphere timer: ' + eT); }\n"
          "}\n");

  QString err;
  QTemporaryDir* keep = nullptr;
  const int id = loadAndRunScript(script, &err, /*keep_loaded=*/true, &keep);

  QMutexLocker lock(&arm().mu);
  arm().rule_installed = rule_ok;
  arm().rule_screen_index = rule_ok ? screen_idx : -1;
  arm().exclude_ids = exclude_ids;
  arm().output_substr = output_substr;
  if (id < 0) {
    if (error_out) *error_out = err.isEmpty() ? QStringLiteral("arm failed") : err;
    delete keep;
    arm().script_id = -1;
    arm().script_dir = nullptr;
    if (!rule_ok) {
      arm().armed = false;
      return false;
    }
    arm().armed = true;
    arm().last_report = QStringLiteral("armed:rule-only");
    VRP_LOG("kwin-place: scripting arm failed (%s); Window Rule still active",
            err.toUtf8().constData());
    return true;
  }
  arm().armed = true;
  arm().script_id = id;
  arm().script_dir = keep;
  arm().last_report = placeHub().reporter.last;
  VRP_LOG("kwin-place: armed compositor place on '%s' (rule=%d script=%d)",
          output_substr.toUtf8().constData(), arm().rule_installed ? 1 : 0, id);
  return true;
}

QList<qint64> KwinWindowPlacer::pid_tree(qint64 root_pid) {
  QList<qint64> out;
  if (root_pid <= 0) return out;
  QSet<qint64> want;
  want.insert(root_pid);
  // Expand a few times so short-lived wrappers / forks are covered.
  for (int pass = 0; pass < 4; ++pass) {
    const QFileInfoList entries =
        QDir(QStringLiteral("/proc"))
            .entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    bool grew = false;
    for (const QFileInfo& fi : entries) {
      bool ok = false;
      const qint64 pid = fi.fileName().toLongLong(&ok);
      if (!ok || pid <= 1) continue;
      QFile st(fi.absoluteFilePath() + QStringLiteral("/status"));
      if (!st.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
      qint64 ppid = -1;
      while (!st.atEnd()) {
        const QByteArray line = st.readLine();
        if (line.startsWith("PPid:")) {
          ppid = line.mid(5).trimmed().toLongLong();
          break;
        }
      }
      if (ppid > 0 && want.contains(ppid) && !want.contains(pid)) {
        want.insert(pid);
        grew = true;
      }
    }
    if (!grew) break;
  }
  out.reserve(want.size());
  for (qint64 p : want) out.push_back(p);
  return out;
}

namespace {

QString jsPidArray(const QList<qint64>& pids) {
  QString s = QStringLiteral("[");
  for (int i = 0; i < pids.size(); ++i) {
    if (i) s += QLatin1Char(',');
    s += QString::number(pids[i]);
  }
  s += QLatin1Char(']');
  return s;
}

QString jsNameArray(const QStringList& names) {
  QString s = QStringLiteral("[");
  for (int i = 0; i < names.size(); ++i) {
    if (i) s += QLatin1Char(',');
    s += QLatin1Char('\'') + jsEscape(names[i].toLower()) + QLatin1Char('\'');
  }
  s += QLatin1Char(']');
  return s;
}

QString jsDesktopPlaceFns() {
  return QStringLiteral(
      "function excluded(id) {\n"
      "  const s = String(id);\n"
      "  for (let i = 0; i < exclude.length; ++i) if (exclude[i] === s) return true;\n"
      "  return false;\n"
      "}\n"
      "function pidHit(w) {\n"
      "  if (!wantPids.length) return false;\n"
      "  let p = 0;\n"
      "  try { p = Number(w.pid) || 0; } catch (eP) { p = 0; }\n"
      "  if (!p) return false;\n"
      "  for (let i = 0; i < wantPids.length; ++i) if (wantPids[i] === p) return true;\n"
      "  return false;\n"
      "}\n"
      "function nameHit(w) {\n"
      "  if (!wantNames.length) return false;\n"
      "  const cls = (w.resourceClass || '').toLowerCase();\n"
      "  const rn = (w.resourceName || '').toLowerCase();\n"
      "  let desk = '';\n"
      "  try { desk = String(w.desktopFileName || '').toLowerCase(); } catch (eDf) {}\n"
      "  const cap = String(w.caption || '').toLowerCase();\n"
      "  for (let i = 0; i < wantNames.length; ++i) {\n"
      "    const n = wantNames[i];\n"
      "    if (!n) continue;\n"
      "    if (cls.indexOf(n) >= 0 || rn.indexOf(n) >= 0 || desk.indexOf(n) >= 0)\n"
      "      return true;\n"
      "    if (n.length >= 5 && cap.indexOf(n) >= 0) return true;\n"
      "  }\n"
      "  return false;\n"
      "}\n"
      "function scoreDesk(w, idx, total) {\n"
      "  if (!isNormalWin(w)) return -1;\n"
      "  const id = String(w.internalId);\n"
      "  if (excluded(id)) return -1;\n"
      "  const hit = pidHit(w);\n"
      "  const byName = nameHit(w);\n"
      "  if (!hit && !byName) return -1;\n"
      "  const fg = w.frameGeometry;\n"
      "  const a = Math.max(1, fg.width) * Math.max(1, fg.height);\n"
      "  const mini = !!w.minimized;\n"
      "  if (!mini && a < 4000 && !hit && !byName) return -1;\n"
      "  let score = (total - idx) + Math.min(a / 1000, 50000);\n"
      "  if (hit) score += 200000;\n"
      "  if (byName) score += 150000;\n"
      "  return score;\n"
      "}\n");
}

}  // namespace

bool KwinWindowPlacer::arm_place_new_window(const QString& output_substr,
                                            const QStringList& exclude_ids,
                                            const QList<qint64>& prefer_pids,
                                            const QStringList& want_names, QString* error_out) {
  if (output_substr.isEmpty()) {
    if (error_out) *error_out = QStringLiteral("empty output");
    return false;
  }
  {
    QMutexLocker lock(&arm().mu);
    disarmLocked(/*keep_fullscreen_rule=*/false);
  }

  if (!placeHub().ok) {
    if (error_out) *error_out = QStringLiteral("D-Bus place reporter failed");
    return false;
  }

  // No wmclass Window Rule — place script only (arbitrary desktop apps).
  placeHub().reporter.last.clear();

  const QString script =
      jsHelpers() +
      QStringLiteral(
          "const outNeedle = '%1'.toLowerCase();\n"
          "const exclude = %2;\n"
          "const wantPids = %3;\n"
          "const wantNames = %4;\n")
          .arg(jsEscape(output_substr.toLower()), jsExcludeArray(exclude_ids),
               jsPidArray(prefer_pids), jsNameArray(want_names)) +
      jsDesktopPlaceFns() +
      QStringLiteral(
          "const targetOut = findOutput(outNeedle);\n"
          "if (!targetOut) { report('fail:no-output'); }\n"
          "else {\n"
          "  let reported = false;\n"
          "  let settleTimer = null;\n"
          "  function stopSettle() {\n"
          "    try { if (settleTimer) settleTimer.stop(); } catch (eS) {}\n"
          "  }\n"
          // One successful place then stop — captionChanged↔place loops froze Plasma.\n"
          // Like Chrome arm: trust placeOnOutput* (do not require immediate output check).\n"
          "  function tryPlace(w, why) {\n"
          "    if (reported) return false;\n"
          "    if (!w || w.deleted) return false;\n"
          "    if (scoreDesk(w, 0, 1) < 0) return false;\n"
          "    if (!placeOnOutputDesktop(w, targetOut)) return false;\n"
          "    reported = true;\n"
          "    stopSettle();\n"
          "    report('ok:' + String(w.internalId) + ':' + targetOut.name + ':' + why + ':desk');\n"
          "    return true;\n"
          "  }\n"
          "  function scan(why) {\n"
          "    if (reported) return;\n"
          "    const wins = (typeof workspace.windowList === 'function')\n"
          "        ? workspace.windowList() : workspace.windowList;\n"
          "    let ranked = [];\n"
          "    for (let i = 0; i < wins.length; ++i) {\n"
          "      const w = wins[i];\n"
          "      const sc = scoreDesk(w, i, wins.length);\n"
          "      if (sc >= 0) ranked.push({w:w, sc:sc});\n"
          "    }\n"
          "    ranked.sort(function(a,b){ return b.sc - a.sc; });\n"
          "    for (let i = 0; i < ranked.length; ++i) {\n"
          "      if (tryPlace(ranked[i].w, why)) break;\n"
          "    }\n"
          "  }\n"
          "  function watch(w) { tryPlace(w, 'added'); }\n"
          "  try { workspace.windowAdded.connect(watch); } catch (eA) {}\n"
          "  try { workspace.clientAdded.connect(watch); } catch (eB) {}\n"
          "  report('armed:' + targetOut.name + ':desktop');\n"
          "  scan('boot');\n"
          "  // Brief settle only — never leave a permanent hot timer in KWin.\n"
          "  try {\n"
          "    settleTimer = new QTimer();\n"
          "    let ticks = 0;\n"
          "    settleTimer.interval = 300;\n"
          "    settleTimer.triggered.connect(function() {\n"
          "      if (reported) { stopSettle(); return; }\n"
          "      scan('timer');\n"
          "      if (++ticks >= 20) stopSettle();  // ~6s\n"
          "    });\n"
          "    settleTimer.start();\n"
          "  } catch (eT) { print('monaSphere timer: ' + eT); }\n"
          "}\n");

  QString err;
  QTemporaryDir* keep = nullptr;
  const int id = loadAndRunScript(script, &err, /*keep_loaded=*/true, &keep);

  QMutexLocker lock(&arm().mu);
  arm().rule_installed = false;
  arm().rule_screen_index = -1;
  arm().exclude_ids = exclude_ids;
  arm().output_substr = output_substr;
  if (id < 0) {
    if (error_out) *error_out = err.isEmpty() ? QStringLiteral("arm failed") : err;
    delete keep;
    arm().script_id = -1;
    arm().script_dir = nullptr;
    arm().armed = false;
    return false;
  }
  arm().armed = true;
  arm().script_id = id;
  arm().script_dir = keep;
  arm().last_report = placeHub().reporter.last;
  VRP_LOG("kwin-place: armed desktop place on '%s' (script=%d pids=%d names=%d)",
          output_substr.toUtf8().constData(), id, prefer_pids.size(), want_names.size());
  return true;
}

bool KwinWindowPlacer::move_new_window_to_output(const QString& output_substr,
                                                 const QStringList& exclude_ids,
                                                 const QList<qint64>& prefer_pids,
                                                 const QStringList& want_names,
                                                 QString* out_window_id, QString* error_out,
                                                 bool* moved_ok) {
  if (output_substr.isEmpty()) {
    if (error_out) *error_out = QStringLiteral("empty output");
    return false;
  }
  BusGuard guard;
  const QString prior = placeHub().reporter.last;
  if (!prior.startsWith(QLatin1String("ok:"))) guard.clearLast();
  const QString script =
      jsHelpers() +
      QStringLiteral(
          "const outNeedle = '%1'.toLowerCase();\n"
          "const exclude = %2;\n"
          "const wantPids = %3;\n"
          "const wantNames = %4;\n")
          .arg(jsEscape(output_substr.toLower()), jsExcludeArray(exclude_ids),
               jsPidArray(prefer_pids), jsNameArray(want_names)) +
      jsDesktopPlaceFns() +
      QStringLiteral(
          "const targetOut = findOutput(outNeedle);\n"
          "if (!targetOut) { report('fail:no-output'); }\n"
          "else {\n"
          "  const wins = (typeof workspace.windowList === 'function')\n"
          "      ? workspace.windowList() : workspace.windowList;\n"
          "  let ranked = [];\n"
          "  for (let i = 0; i < wins.length; ++i) {\n"
          "    const w = wins[i];\n"
          "    const sc = scoreDesk(w, i, wins.length);\n"
          "    if (sc >= 0) ranked.push({w:w, sc:sc});\n"
          "  }\n"
          "  ranked.sort(function(a,b){ return b.sc - a.sc; });\n"
          "  let placed = null;\n"
          "  for (let i = 0; i < ranked.length; ++i) {\n"
          "    if (placeOnOutputDesktop(ranked[i].w, targetOut)) {\n"
          "      if (!placed) placed = ranked[i].w;\n"
          "    }\n"
          "  }\n"
          "  if (!placed) { report('fail:no-window'); }\n"
          "  else report('ok:' + String(placed.internalId) + ':' + targetOut.name);\n"
          "}\n");

  const bool ran = runScript(script, error_out);
  QString rep = guard.last();
  if (!rep.startsWith(QLatin1String("ok:")) && prior.startsWith(QLatin1String("ok:"))) {
    rep = prior;
  }
  if (rep.startsWith(QLatin1String("ok:"))) {
    QMutexLocker lock(&arm().mu);
    arm().last_report = rep;
    placeHub().reporter.last = rep;
  }
  const bool ok_msg = rep.startsWith(QLatin1String("ok:"));
  if (moved_ok) *moved_ok = ok_msg;
  if (ok_msg && out_window_id) {
    const QString rest = rep.mid(3);
    const int colon = rest.indexOf(QLatin1Char(':'));
    *out_window_id = colon >= 0 ? rest.left(colon) : rest;
  }
  if (!ok_msg && error_out && error_out->isEmpty()) {
    *error_out = rep.isEmpty() ? QStringLiteral("no placement report") : rep;
  }
  return ran;
}

bool KwinWindowPlacer::poll_armed_place(QString* out_window_id, QString* error_out) {
  QMutexLocker lock(&arm().mu);
  if (!arm().armed && !arm().rule_installed) {
    if (error_out) *error_out = QStringLiteral("not armed");
    return false;
  }
  const QString rep = placeHub().reporter.last;
  if (!rep.isEmpty()) arm().last_report = rep;
  if (!arm().last_report.startsWith(QLatin1String("ok:"))) {
    if (arm().last_report.startsWith(QLatin1String("fail:")) && error_out)
      *error_out = arm().last_report;
    return false;
  }
  if (out_window_id) {
    const QString rest = arm().last_report.mid(3);
    const int colon = rest.indexOf(QLatin1Char(':'));
    *out_window_id = colon >= 0 ? rest.left(colon) : rest;
  }
  return true;
}

void KwinWindowPlacer::disarm_place_on_open(bool keep_fullscreen_rule) {
  QMutexLocker lock(&arm().mu);
  disarmLocked(keep_fullscreen_rule);
}

bool KwinWindowPlacer::move_youtube_to_output(const QString& output_substr,
                                              const QStringList& exclude_ids,
                                              QString* out_window_id, QString* error_out,
                                              bool* moved_ok) {
  if (output_substr.isEmpty()) {
    if (error_out) *error_out = QStringLiteral("empty output");
    return false;
  }
  BusGuard guard;
  // Preserve an in-flight arm ok: — clearing it raced with windowAdded reports.
  const QString prior = placeHub().reporter.last;
  if (!prior.startsWith(QLatin1String("ok:"))) guard.clearLast();
  const QString script =
      jsHelpers() +
      QStringLiteral(
          "const outNeedle = '%1'.toLowerCase();\n"
          "const exclude = %2;\n")
          .arg(jsEscape(output_substr.toLower()), jsExcludeArray(exclude_ids)) +
      jsExcludedFn() +
      QStringLiteral(
          "const targetOut = findOutput(outNeedle);\n"
          "if (!targetOut) { report('fail:no-output'); }\n"
          "else {\n"
          "  const wins = (typeof workspace.windowList === 'function')\n"
          "      ? workspace.windowList() : workspace.windowList;\n"
          "  let best = null, bestScore = -1;\n"
          "  for (let i = wins.length - 1; i >= 0; --i) {\n"
          "    const w = wins[i];\n"
          "    if (!w || w.deleted) continue;\n"
          "    const sc = scoreWin(w, i, wins.length);\n"
          "    if (sc > bestScore) { bestScore = sc; best = w; }\n"
          "  }\n"
          "  if (!best) { report('fail:no-window'); }\n"
          "  else if (placeOnOutput(best, targetOut))\n"
          "    report('ok:' + String(best.internalId) + ':' + targetOut.name);\n"
          "  else report('fail:exception');\n"
          "}\n");

  const bool ran = runScript(script, error_out);
  QString rep = guard.last();
  if (!rep.startsWith(QLatin1String("ok:")) && prior.startsWith(QLatin1String("ok:"))) {
    rep = prior;  // arm succeeded while one-shot ran
  }
  // Keep arm's last_report in sync when a one-shot move succeeds while armed.
  if (rep.startsWith(QLatin1String("ok:"))) {
    QMutexLocker lock(&arm().mu);
    arm().last_report = rep;
    placeHub().reporter.last = rep;
  }
  const bool ok_msg = rep.startsWith(QLatin1String("ok:"));
  if (moved_ok) *moved_ok = ok_msg;
  if (ok_msg && out_window_id) {
    const QString rest = rep.mid(3);
    const int colon = rest.indexOf(QLatin1Char(':'));
    *out_window_id = colon >= 0 ? rest.left(colon) : rest;
  }
  if (!ok_msg && error_out && error_out->isEmpty()) {
    *error_out = rep.isEmpty() ? QStringLiteral("no placement report") : rep;
  }
  return ran;
}

bool KwinWindowPlacer::close_chrome_except(const QStringList& keep_ids, QString* error_out) {
  BusGuard guard;
  guard.clearLast();
  const QString script =
      jsHelpers() +
      QStringLiteral("const exclude = %1;\n").arg(jsExcludeArray(keep_ids)) +
      QStringLiteral(
          "function excluded(id) {\n"
          "  const s = String(id);\n"
          "  for (let i = 0; i < exclude.length; ++i) if (exclude[i] === s) return true;\n"
          "  return false;\n"
          "}\n"
          "const wins = (typeof workspace.windowList === 'function')\n"
          "    ? workspace.windowList() : workspace.windowList;\n"
          "let n = 0;\n"
          "for (let i = 0; i < wins.length; ++i) {\n"
          "  const w = wins[i];\n"
          "  if (!w || w.deleted || !isChrome(w)) continue;\n"
          "  if (excluded(w.internalId)) continue;\n"
          "  try { w.closeWindow(); ++n; } catch (e) { print(e); }\n"
          "}\n"
          "report('closed-new:' + n);\n");
  if (!runScript(script, error_out)) return false;
  const QString rep = guard.last();
  if (rep.startsWith(QLatin1String("closed-new:"))) {
    const int n = rep.mid(11).toInt();
    VRP_LOG("kwin-place: closed %d new Chrome window(s) (place abort)", n);
    return n > 0;
  }
  return false;
}

bool KwinWindowPlacer::close_window_by_id(const QString& internal_id, QString* error_out) {
  if (internal_id.isEmpty()) {
    if (error_out) *error_out = QStringLiteral("empty id");
    return false;
  }
  BusGuard guard;
  const QString script =
      jsHelpers() +
      QStringLiteral(
          "const want = '%1';\n"
          "const wins = (typeof workspace.windowList === 'function')\n"
          "    ? workspace.windowList() : workspace.windowList;\n"
          "let closed = false;\n"
          "for (let i = 0; i < wins.length; ++i) {\n"
          "  const w = wins[i];\n"
          "  if (!w || w.deleted) continue;\n"
          "  if (String(w.internalId) !== want) continue;\n"
          "  try { w.closeWindow(); closed = true; } catch (e) { print(e); }\n"
          "  break;\n"
          "}\n"
          "report(closed ? 'closed' : 'fail:not-found');\n")
          .arg(jsEscape(internal_id));
  if (!runScript(script, error_out)) return false;
  return guard.last() == QLatin1String("closed");
}

bool KwinWindowPlacer::close_youtube_window(QString* error_out) {
  BusGuard guard;
  // Only close dedicated monaSphere / --app windows — never a normal Chrome
  // that happens to show a video site in the title.
  const QString script = jsHelpers() + QStringLiteral(
      "const wins = (typeof workspace.windowList === 'function')\n"
      "    ? workspace.windowList() : workspace.windowList;\n"
      "let best = null;\n"
      "let bestScore = -1;\n"
      "for (let i = wins.length - 1; i >= 0; --i) {\n"
      "  const w = wins[i];\n"
      "  if (!w || w.deleted || !isOurs(w)) continue;\n"
      "  const rn = (w.resourceName || '').toLowerCase();\n"
      "  const rc = (w.resourceClass || '').toLowerCase();\n"
      "  let score = wins.length - i;\n"
      "  if (rc.indexOf('monasphereyt') >= 0 || rn.indexOf('monasphereyt') >= 0) score += 100000;\n"
      "  if (rn.indexOf('crx_') === 0) score += 80000;\n"
      "  if (score > bestScore) { bestScore = score; best = w; }\n"
      "}\n"
      "if (!best) { report('fail:no-window'); }\n"
      "else {\n"
      "  try { best.closeWindow(); report('closed:' + String(best.internalId)); }\n"
      "  catch (e) { report('fail:exception'); }\n"
      "}\n");
  if (!runScript(script, error_out)) return false;
  return guard.last().startsWith(QLatin1String("closed"));
}

bool KwinWindowPlacer::activate_youtube_window(const QString& internal_id, QString* error_out) {
  BusGuard guard;
  guard.clearLast();
  QString script;
  if (!internal_id.isEmpty()) {
    script = jsHelpers() + QStringLiteral(
                 "const want = '%1';\n"
                 "const wins = (typeof workspace.windowList === 'function')\n"
                 "    ? workspace.windowList() : workspace.windowList;\n"
                 "let ok = false;\n"
                 "for (let i = 0; i < wins.length; ++i) {\n"
                 "  const w = wins[i];\n"
                 "  if (!w || w.deleted) continue;\n"
                 "  if (String(w.internalId) !== want) continue;\n"
                 "  try {\n"
                 "    w.minimized = false;\n"
                 "    try { w.noBorder = true; } catch (eNb) {}\n"
                 "    w.fullScreen = true;\n"
                 "    if (typeof workspace.activateWindow === 'function') workspace.activateWindow(w);\n"
                 "    else workspace.activeWindow = w;\n"
                 "    try { w.noBorder = true; } catch (eNb2) {}\n"
                 "    w.fullScreen = true;\n"
                 "    ok = true;\n"
                 "  } catch (e) { print(e); }\n"
                 "  break;\n"
                 "}\n"
                 "report(ok ? 'activated' : 'fail:not-found');\n")
                 .arg(jsEscape(internal_id));
  } else {
    script = jsHelpers() + QStringLiteral(
                 "const wins = (typeof workspace.windowList === 'function')\n"
                 "    ? workspace.windowList() : workspace.windowList;\n"
                 "let best = null;\n"
                 "let bestScore = -1;\n"
                 "for (let i = wins.length - 1; i >= 0; --i) {\n"
                 "  const w = wins[i];\n"
                 "  if (!w || w.deleted || !isOurs(w)) continue;\n"
                 "  const rn = (w.resourceName || '').toLowerCase();\n"
                 "  const rc = (w.resourceClass || '').toLowerCase();\n"
                 "  let score = wins.length - i;\n"
                 "  if (rc.indexOf('monasphereyt') >= 0 || rn.indexOf('monasphereyt') >= 0) score += 100000;\n"
                 "  if (rn.indexOf('crx_') === 0) score += 80000;\n"
                 "  if (score > bestScore) { bestScore = score; best = w; }\n"
                 "}\n"
                 "if (!best) { report('fail:no-window'); }\n"
                 "else {\n"
                 "  try {\n"
                 "    best.minimized = false;\n"
                 "    try { best.noBorder = true; } catch (eNb) {}\n"
                 "    best.fullScreen = true;\n"
                 "    if (typeof workspace.activateWindow === 'function') workspace.activateWindow(best);\n"
                 "    else workspace.activeWindow = best;\n"
                 "    try { best.noBorder = true; } catch (eNb2) {}\n"
                 "    best.fullScreen = true;\n"
                 "    report('activated:' + String(best.internalId));\n"
                 "  } catch (e) { report('fail:exception'); }\n"
                 "}\n");
  }
  if (!runScript(script, error_out)) return false;
  return guard.last().startsWith(QLatin1String("activated"));
}

bool KwinWindowPlacer::arm_portal_dialog_on_primary(QString* error_out) {
  {
    QMutexLocker lock(&portalArm().mu);
    disarmPortalLocked();
  }
  if (!placeHub().ok) {
    if (error_out) *error_out = QStringLiteral("D-Bus place reporter failed");
    return false;
  }
  placeHub().reporter.last.clear();
  QString err;
  QTemporaryDir* keep = nullptr;
  const int id = loadAndRunScript(portalArmScript(), &err, /*keep_loaded=*/true, &keep,
                                  kScriptNamePortal);
  QMutexLocker lock(&portalArm().mu);
  if (id < 0) {
    if (error_out) *error_out = err.isEmpty() ? QStringLiteral("portal arm failed") : err;
    delete keep;
    portalArm().script_id = -1;
    portalArm().script_dir = nullptr;
    portalArm().armed = false;
    return false;
  }
  portalArm().armed = true;
  portalArm().script_id = id;
  portalArm().script_dir = keep;
  VRP_LOG("kwin-place: armed portal-dialog → primary (script=%d)", id);
  return true;
}

void KwinWindowPlacer::disarm_portal_dialog_on_primary() {
  QMutexLocker lock(&portalArm().mu);
  if (!portalArm().armed && portalArm().script_id < 0 && !portalArm().script_dir) return;
  disarmPortalLocked();
  VRP_LOG("kwin-place: disarmed portal-dialog arm");
}

bool KwinWindowPlacer::move_portal_dialogs_to_primary(QString* error_out) {
  BusGuard guard;
  guard.clearLast();
  QString err;
  const int id = loadAndRunScript(portalMoveOnceScript(), &err, /*keep_loaded=*/false, nullptr,
                                  kScriptNamePortalOnce);
  if (id < 0) {
    if (error_out) *error_out = err.isEmpty() ? QStringLiteral("portal move failed") : err;
    return false;
  }
  const QString rep = guard.last();
  if (rep.startsWith(QLatin1String("portal-moved:"))) {
    VRP_LOG("kwin-place: %s", rep.toUtf8().constData());
    return true;
  }
  // portal-none / portal-armed are not failures — dialog may not exist yet.
  return true;
}

bool KwinWindowPlacer::move_class_to_output(const QString& class_substr, const QString& output_substr,
                                            QString* error_out) {
  return move_to_output(class_substr, output_substr, 0, error_out);
}

bool KwinWindowPlacer::move_to_output(const QString& /*class_substr*/, const QString& output_substr,
                                      qint64 /*pid*/, QString* error_out) {
  bool moved = false;
  QString id;
  return move_youtube_to_output(output_substr, {}, &id, error_out, &moved);
}

bool KwinWindowPlacer::move_youtube_to_output(const QString& output_substr, QString* error_out,
                                              bool* moved_ok) {
  QString id;
  return move_youtube_to_output(output_substr, {}, &id, error_out, moved_ok);
}

bool KwinWindowPlacer::query_cursor_pos(double* x, double* y, QString* error_out) {
  if (!x || !y) {
    if (error_out) *error_out = QStringLiteral("null out");
    return false;
  }
  if (!placeHub().ok) {
    if (error_out) *error_out = QStringLiteral("D-Bus place reporter failed");
    return false;
  }
  BusGuard guard;
  guard.clearLast();
  const QString script = QStringLiteral(
      "function report(msg) {\n"
      "  try {\n"
      "    callDBus('org.monaSphere.Yt', '/monaSphereYtPlace',\n"
      "             'org.monaSphere.YtPlace', 'report', msg);\n"
      "  } catch (e) { print('monaSphere report failed: ' + e); }\n"
      "}\n"
      "try {\n"
      "  const p = workspace.cursorPos;\n"
      "  report('cursor:' + p.x + ',' + p.y);\n"
      "} catch (e) { report('fail:' + e); }\n");
  QString err;
  if (loadAndRunScript(script, &err, /*keep_loaded=*/false, nullptr,
                       "monaSphereYtCursor") < 0) {
    if (error_out) *error_out = err.isEmpty() ? QStringLiteral("cursor script failed") : err;
    return false;
  }
  const QString rep = guard.last();
  if (!rep.startsWith(QLatin1String("cursor:"))) {
    if (error_out) *error_out = rep.isEmpty() ? QStringLiteral("no cursor report") : rep;
    return false;
  }
  const QStringList parts = rep.mid(7).split(QLatin1Char(','));
  if (parts.size() < 2) {
    if (error_out) *error_out = QStringLiteral("bad cursor report");
    return false;
  }
  bool okx = false, oky = false;
  const double px = parts.at(0).toDouble(&okx);
  const double py = parts.at(1).toDouble(&oky);
  if (!okx || !oky) {
    if (error_out) *error_out = QStringLiteral("cursor parse failed");
    return false;
  }
  *x = px;
  *y = py;
  return true;
}

}  // namespace vrp

#include "kwin_window_placer.moc"
