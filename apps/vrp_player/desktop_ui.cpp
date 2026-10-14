#include "desktop_ui.hpp"

#include <QApplication>
#include <QColor>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QFile>
#include <QGuiApplication>
#include <QPalette>
#include <QStandardPaths>
#include <QStyleHints>

#include <QtGlobal>

namespace {

struct DesktopAppearance {
  QFont ui;
  QFont fixed;
  bool have_ui = false;
  bool have_fixed = false;
  // 1 = prefer dark, 2 = prefer light, 0 = unset.
  int color_scheme = 0;
};

DesktopAppearance g_desktop;

QString iniValue(const QString& path, const QString& group, const QString& key) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
  QString current;
  while (!file.atEnd()) {
    const QString line = QString::fromUtf8(file.readLine()).trimmed();
    if (line.isEmpty() || line.startsWith(QLatin1Char('#')) || line.startsWith(QLatin1Char(';'))) continue;
    if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
      current = line.mid(1, line.size() - 2).trimmed();
      continue;
    }
    if (current.compare(group, Qt::CaseInsensitive) != 0) continue;
    const int eq = line.indexOf(QLatin1Char('='));
    if (eq <= 0) continue;
    if (line.left(eq).trimmed() != key) continue;
    return line.mid(eq + 1).trimmed();
  }
  return {};
}

QFont fontFromPango(const QString& desc) {
  const QStringList parts = desc.split(QLatin1Char(' '), Qt::SkipEmptyParts);
  if (parts.isEmpty()) return {};
  int family_end = parts.size();
  bool have_size = false;
  double size = 0;
  bool size_ok = false;
  size = parts.last().toDouble(&size_ok);
  if (size_ok && size > 0) {
    have_size = true;
    family_end = parts.size() - 1;
  }
  QStringList family;
  QFont::Weight weight = QFont::Normal;
  bool italic = false;
  for (int i = 0; i < family_end; ++i) {
    const QString& word = parts.at(i);
    if (word.compare(QLatin1String("Bold"), Qt::CaseInsensitive) == 0) {
      weight = QFont::Bold;
    } else if (word.compare(QLatin1String("Italic"), Qt::CaseInsensitive) == 0 ||
               word.compare(QLatin1String("Oblique"), Qt::CaseInsensitive) == 0) {
      italic = true;
    } else if (word.compare(QLatin1String("Medium"), Qt::CaseInsensitive) == 0) {
      weight = QFont::Medium;
    } else if (word.compare(QLatin1String("Light"), Qt::CaseInsensitive) == 0) {
      weight = QFont::Light;
    } else if (word.compare(QLatin1String("Semibold"), Qt::CaseInsensitive) == 0 ||
               word.compare(QLatin1String("DemiBold"), Qt::CaseInsensitive) == 0) {
      weight = QFont::DemiBold;
    } else if (word.compare(QLatin1String("Regular"), Qt::CaseInsensitive) == 0 ||
               word.compare(QLatin1String("Normal"), Qt::CaseInsensitive) == 0 ||
               word.compare(QLatin1String("Book"), Qt::CaseInsensitive) == 0) {
    } else {
      family << word;
    }
  }
  if (family.isEmpty()) return {};
  QFont font(family.join(QLatin1Char(' ')));
  if (have_size) font.setPointSizeF(size);
  font.setWeight(weight);
  font.setItalic(italic);
  return font;
}

QFont fontFromQtString(const QString& spec) {
  if (spec.isEmpty()) return {};
  QFont font;
  if (!font.fromString(spec) || font.family().isEmpty()) return {};
  return font;
}

QVariant unwrapDbus(const QVariant& in, int depth = 0) {
  if (depth > 4) return in;
  if (in.canConvert<QDBusVariant>()) return unwrapDbus(in.value<QDBusVariant>().variant(), depth + 1);
  if (in.userType() == qMetaTypeId<QDBusArgument>()) {
    QDBusArgument arg = in.value<QDBusArgument>();
    if (arg.currentType() == QDBusArgument::VariantType) {
      QVariant inner;
      arg >> inner;
      return unwrapDbus(inner, depth + 1);
    }
  }
  return in;
}

QVariant portalValue(const QString& key) {
  const QDBusConnection bus = QDBusConnection::sessionBus();
  if (!bus.isConnected()) return {};
  QDBusMessage msg = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.portal.Desktop"),
                                                    QStringLiteral("/org/freedesktop/portal/desktop"),
                                                    QStringLiteral("org.freedesktop.portal.Settings"),
                                                    QStringLiteral("Read"));
  msg << QStringLiteral("org.freedesktop.appearance") << key;
  const QDBusMessage reply = bus.call(msg, QDBus::Block, 300);
  if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) return {};
  return unwrapDbus(reply.arguments().at(0));
}

QString firstIni(const QString& file_name, const QString& group, const QString& key) {
  const QStringList dirs = QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation);
  for (const QString& dir : dirs) {
    const QString value = iniValue(dir + QLatin1Char('/') + file_name, group, key);
    if (!value.isEmpty()) return value;
  }
  return {};
}

void applyColorScheme(QApplication& app, int scheme) {
  if (scheme != 1 && scheme != 2) return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
  Q_UNUSED(app);
  QGuiApplication::styleHints()->setColorScheme(scheme == 1 ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
#else
  if (scheme != 1) return;
  QPalette palette;
  const QColor window(0x2e, 0x2e, 0x2e);
  const QColor text(0xee, 0xee, 0xee);
  const QColor base(0x25, 0x25, 0x25);
  const QColor button(0x3a, 0x3a, 0x3a);
  palette.setColor(QPalette::Window, window);
  palette.setColor(QPalette::WindowText, text);
  palette.setColor(QPalette::Base, base);
  palette.setColor(QPalette::AlternateBase, window);
  palette.setColor(QPalette::Text, text);
  palette.setColor(QPalette::Button, button);
  palette.setColor(QPalette::ButtonText, text);
  palette.setColor(QPalette::Highlight, QColor(0x3d, 0x6e, 0xaa));
  palette.setColor(QPalette::HighlightedText, Qt::white);
  palette.setColor(QPalette::ToolTipBase, base);
  palette.setColor(QPalette::ToolTipText, text);
  palette.setColor(QPalette::PlaceholderText, QColor(0xaa, 0xaa, 0xaa));
  palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0x9a, 0x9a, 0x9a));
  palette.setColor(QPalette::Disabled, QPalette::Text, QColor(0x9a, 0x9a, 0x9a));
  palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x9a, 0x9a, 0x9a));
  app.setStyle(QStringLiteral("Fusion"));
  app.setPalette(palette);
#endif
}

}  // namespace

void applyDesktopAppearance() {
  DesktopAppearance found;

  const QVariant portal_font = portalValue(QStringLiteral("font-name"));
  if (portal_font.canConvert<QString>()) found.ui = fontFromPango(portal_font.toString());
  if (found.ui.family().isEmpty()) found.ui = fontFromQtString(firstIni(QStringLiteral("kdeglobals"), QStringLiteral("General"), QStringLiteral("font")));
  if (found.ui.family().isEmpty()) {
    QString gtk = firstIni(QStringLiteral("gtk-3.0/settings.ini"), QStringLiteral("Settings"), QStringLiteral("gtk-font-name"));
    if (gtk.isEmpty()) {
      gtk = firstIni(QStringLiteral("gtk-4.0/settings.ini"), QStringLiteral("Settings"), QStringLiteral("gtk-font-name"));
    }
    found.ui = fontFromPango(gtk);
  }
  found.have_ui = !found.ui.family().isEmpty();

  found.fixed = fontFromQtString(firstIni(QStringLiteral("kdeglobals"), QStringLiteral("General"), QStringLiteral("fixed")));
  found.have_fixed = !found.fixed.family().isEmpty();

  const QVariant portal_scheme = portalValue(QStringLiteral("color-scheme"));
  bool scheme_ok = false;
  const int scheme = portal_scheme.toInt(&scheme_ok);
  if (scheme_ok && (scheme == 1 || scheme == 2)) {
    found.color_scheme = scheme;
  } else if (firstIni(QStringLiteral("kdeglobals"), QStringLiteral("General"), QStringLiteral("ColorScheme"))
                 .contains(QLatin1String("dark"), Qt::CaseInsensitive)) {
    found.color_scheme = 1;
  }

  g_desktop = found;
  if (found.have_ui) QApplication::setFont(found.ui);
  if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) applyColorScheme(*app, found.color_scheme);
}

QFont desktopFixedFont() { return g_desktop.have_fixed ? g_desktop.fixed : QFont(); }

QFont desktopUiFont() { return g_desktop.have_ui ? g_desktop.ui : QFont(); }
