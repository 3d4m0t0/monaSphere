#include "capture/gnome_virtual_display.hpp"

#include "common.hpp"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QVariant>

namespace vrp {
namespace {

constexpr const char* kPortal = "org.freedesktop.portal.Desktop";
constexpr const char* kPortalPath = "/org/freedesktop/portal/desktop";
constexpr const char* kScreenCastIface = "org.freedesktop.portal.ScreenCast";
constexpr const char* kMutterScreenCast = "org.gnome.Mutter.ScreenCast";
constexpr uint32_t kSourceVirtual = 0x4;

QVariant unwrapDbus(QVariant v) {
  for (int i = 0; i < 4; ++i) {
    if (!v.canConvert<QDBusVariant>()) break;
    const QDBusVariant dv = qvariant_cast<QDBusVariant>(v);
    const QVariant inner = dv.variant();
    if (!inner.isValid() || inner == v) break;
    v = inner;
  }
  return v;
}

uint32_t readPortalSourceTypes() {
  QDBusMessage msg = QDBusMessage::createMethodCall(
      QString::fromLatin1(kPortal), QString::fromLatin1(kPortalPath),
      QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
  msg << QString::fromLatin1(kScreenCastIface) << QStringLiteral("AvailableSourceTypes");
  const QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 3000);
  if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) return 0;
  bool ok = false;
  const uint32_t n = unwrapDbus(reply.arguments().at(0)).toUInt(&ok);
  return ok ? n : 0;
}

}  // namespace

bool GnomeVirtualDisplay::portalSupportsVirtual() {
  const uint32_t types = readPortalSourceTypes();
  VRP_LOG("gnome-virtual: portal AvailableSourceTypes=0x%x virtual=%d", types,
          (types & kSourceVirtual) ? 1 : 0);
  return (types & kSourceVirtual) != 0;
}

bool GnomeVirtualDisplay::mutterScreenCastAvailable() {
  QDBusConnectionInterface* iface = QDBusConnection::sessionBus().interface();
  if (!iface) return false;
  const bool on = iface->isServiceRegistered(QString::fromLatin1(kMutterScreenCast));
  VRP_LOG("gnome-virtual: Mutter.ScreenCast registered=%d", on ? 1 : 0);
  return on;
}

bool GnomeVirtualDisplay::managedVirtualAvailable() {
  return portalSupportsVirtual() || mutterScreenCastAvailable();
}

QString GnomeVirtualDisplay::outputNameHint() {
  return QStringLiteral("Meta-");
}

bool GnomeVirtualDisplay::queryVirtualRect(QRect* /*geo*/, QString* /*matched_connector*/) {
  // Intentionally unused: demarshalling GetCurrentState via QDBusArgument aborted
  // (signature varies by Mutter; "write from a read-only object" / libdbus abort).
  // Placement uses portal EIS region (region_x/y/w/h) instead — see resolveGnomeVirtualRect.
  return false;
}

}  // namespace vrp
