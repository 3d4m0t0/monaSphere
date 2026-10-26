#pragma once

#include <QString>
#include <QStringList>
#include <cstdlib>

namespace vrp {

enum class DesktopEnv {
  Kde,
  Gnome,
  Other,
};

/** Best-effort from XDG_CURRENT_DESKTOP / XDG_SESSION_DESKTOP (colon-separated). */
inline DesktopEnv detect_desktop_env() {
  auto match = [](const char* env) -> DesktopEnv {
    if (!env || !*env) return DesktopEnv::Other;
    const QString s = QString::fromLocal8Bit(env).toLower();
    const QStringList parts = s.split(QLatin1Char(':'), Qt::SkipEmptyParts);
    for (const QString& p : parts) {
      const QString t = p.trimmed();
      if (t.contains(QLatin1String("kde")) || t.contains(QLatin1String("plasma"))) {
        return DesktopEnv::Kde;
      }
      if (t.contains(QLatin1String("gnome")) || t.contains(QLatin1String("unity"))) {
        return DesktopEnv::Gnome;
      }
    }
    return DesktopEnv::Other;
  };
  const DesktopEnv from_current = match(std::getenv("XDG_CURRENT_DESKTOP"));
  if (from_current != DesktopEnv::Other) return from_current;
  return match(std::getenv("XDG_SESSION_DESKTOP"));
}

inline const char* desktop_env_name(DesktopEnv e) {
  switch (e) {
    case DesktopEnv::Kde:
      return "KDE";
    case DesktopEnv::Gnome:
      return "GNOME";
    default:
      return "Other";
  }
}

}  // namespace vrp
