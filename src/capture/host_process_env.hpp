#pragma once

#include <QCoreApplication>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

namespace vrp {
namespace {

inline void strip_appdir_from_path_var(QProcessEnvironment& env, const QString& key,
                                       const QString& appdir) {
  if (!env.contains(key) || appdir.isEmpty()) return;
  const QString prefix = appdir.endsWith(QLatin1Char('/')) ? appdir : (appdir + QLatin1Char('/'));
  const QStringList parts = env.value(key).split(QLatin1Char(':'), Qt::SkipEmptyParts);
  QStringList kept;
  kept.reserve(parts.size());
  for (const QString& part : parts) {
    const QString trimmed = part.endsWith(QLatin1Char('/')) ? part.chopped(1) : part;
    if (trimmed == appdir || trimmed.startsWith(prefix)) continue;
    kept.push_back(part);
  }
  if (kept.isEmpty()) {
    env.remove(key);
  } else {
    env.insert(key, kept.join(QLatin1Char(':')));
  }
}

inline bool path_under_appdir(const QString& path, const QString& appdir) {
  if (appdir.isEmpty() || path.isEmpty()) return false;
  const QString prefix = appdir.endsWith(QLatin1Char('/')) ? appdir : (appdir + QLatin1Char('/'));
  const QString trimmed = path.endsWith(QLatin1Char('/')) ? path.chopped(1) : path;
  return trimmed == appdir || trimmed.startsWith(prefix);
}

}  // namespace

/**
 * True when *this* binary is running from an AppImage AppDir.
 * Do not trust APPDIR/APPIMAGE alone — Cursor (and other AppImages) leak those
 * into child processes; only match when applicationFilePath() is under APPDIR.
 */
inline bool running_in_appimage() {
  const QByteArray appdir_ba = qgetenv("APPDIR");
  if (appdir_ba.isEmpty()) return false;
  const QString self = QCoreApplication::applicationFilePath();
  if (self.isEmpty()) return false;
  const QString appdir = QString::fromLocal8Bit(appdir_ba);
  const QString prefix = appdir.endsWith(QLatin1Char('/')) ? appdir : (appdir + QLatin1Char('/'));
  return self.startsWith(prefix);
}

/**
 * Environment for host binaries (krfb, kscreen-doctor, Chrome, Monado, …).
 * AppRun sets $APPDIR/usr/lib and Qt plugin paths for the bundled AppImage Qt.
 * Those must not leak into Plasma/KDE host tools — krfb then loads the broken
 * AppImage wayland plugin and exits with "Virtual Monitors are only supported
 * on Wayland" even though the session is Wayland.
 */
inline QProcessEnvironment host_tool_environment() {
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  const QByteArray appdir_ba = qgetenv("APPDIR");
  if (appdir_ba.isEmpty()) return env;
  const QString appdir = QString::fromLocal8Bit(appdir_ba);

  strip_appdir_from_path_var(env, QStringLiteral("LD_LIBRARY_PATH"), appdir);
  strip_appdir_from_path_var(env, QStringLiteral("PATH"), appdir);
  strip_appdir_from_path_var(env, QStringLiteral("QT_PLUGIN_PATH"), appdir);
  strip_appdir_from_path_var(env, QStringLiteral("QML2_IMPORT_PATH"), appdir);
  strip_appdir_from_path_var(env, QStringLiteral("QML_IMPORT_PATH"), appdir);
  strip_appdir_from_path_var(env, QStringLiteral("XDG_DATA_DIRS"), appdir);
  strip_appdir_from_path_var(env, QStringLiteral("VK_ICD_FILENAMES"), appdir);
  strip_appdir_from_path_var(env, QStringLiteral("VK_DRIVER_FILES"), appdir);

  // Always drop AppImage Qt plugin overrides — host Qt must use system plugins.
  env.remove(QStringLiteral("QT_PLUGIN_PATH"));
  env.remove(QStringLiteral("QT_QPA_PLATFORM_PLUGIN_PATH"));
  env.remove(QStringLiteral("QTWEBENGINEPROCESS_PATH"));

  // Let host Qt pick wayland/xcb from the session; do not inherit a forced
  // platform that only exists inside the AppImage plugin tree.
  const QString plat = env.value(QStringLiteral("QT_QPA_PLATFORM"));
  if (!plat.isEmpty() &&
      (plat.contains(QLatin1String("wayland"), Qt::CaseInsensitive) ||
       plat.contains(QLatin1String("xcb"), Qt::CaseInsensitive))) {
    env.remove(QStringLiteral("QT_QPA_PLATFORM"));
  }

  // Chrome/host Vulkan must use system ICDs (NVIDIA), not an empty AppDir tree.
  const QString vk_icd = env.value(QStringLiteral("VK_ICD_FILENAMES"));
  if (!vk_icd.isEmpty() && path_under_appdir(vk_icd.split(QLatin1Char(':')).value(0), appdir)) {
    env.remove(QStringLiteral("VK_ICD_FILENAMES"));
  }
  const QString vk_drv = env.value(QStringLiteral("VK_DRIVER_FILES"));
  if (!vk_drv.isEmpty() && path_under_appdir(vk_drv.split(QLatin1Char(':')).value(0), appdir)) {
    env.remove(QStringLiteral("VK_DRIVER_FILES"));
  }

  // Chrome/Chromium treat APPDIR as a packaging root and break Vulkan ICD /
  // sandbox path resolution (relative nvidia_icd JSON → Dawn "Found no drivers",
  // then Wayland+Vulkan abort). Host tools must look like a normal session.
  env.remove(QStringLiteral("APPDIR"));
  env.remove(QStringLiteral("APPIMAGE"));
  env.remove(QStringLiteral("ARGV0"));
  env.remove(QStringLiteral("OWD"));
  env.remove(QStringLiteral("APPIMAGE_EXTRACT_AND_RUN"));

  return env;
}

/**
 * Absolute path of libmonasphere_padhook.so for LD_PRELOAD.
 * Dev builds copy next to the binary; AppImage/install puts it in ../lib.
 */
inline QString padhook_library_path() {
  const QString name = QStringLiteral("libmonasphere_padhook.so");
  const QString app_dir = QCoreApplication::applicationDirPath();
  QStringList candidates;
  candidates << (app_dir + QLatin1Char('/') + name);
  candidates << QFileInfo(app_dir + QStringLiteral("/../lib/") + name).absoluteFilePath();
  const QByteArray appdir = qgetenv("APPDIR");
  if (!appdir.isEmpty()) {
    candidates << (QString::fromLocal8Bit(appdir) + QStringLiteral("/usr/lib/") + name);
  }
  for (const QString& c : candidates) {
    const QFileInfo fi(c);
    if (fi.exists() && fi.isFile()) return fi.canonicalFilePath();
  }
  return {};
}

}  // namespace vrp
