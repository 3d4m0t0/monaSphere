#include "capture/kde_eis_pointer.hpp"

#include "common.hpp"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusUnixFileDescriptor>

#include <unistd.h>

namespace vrp {
namespace {

constexpr int kDeviceTypes = 0x7;  // keyboard | pointer | touch

}  // namespace

KdeEisPointer::~KdeEisPointer() { stop(); }

std::string KdeEisPointer::last_error() const {
  if (!last_error_.empty()) return last_error_;
  return core_.last_error();
}

bool KdeEisPointer::start(const std::string& mapping_id_substr) {
  stop();
  last_error_.clear();

  QDBusMessage msg = QDBusMessage::createMethodCall(
      QStringLiteral("org.kde.KWin"), QStringLiteral("/org/kde/KWin/EIS/RemoteDesktop"),
      QStringLiteral("org.kde.KWin.EIS.RemoteDesktop"), QStringLiteral("connectToEIS"));
  msg << kDeviceTypes;
  const QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 3000);
  if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() < 2) {
    last_error_ = std::string("KWin connectToEIS failed: ") + reply.errorMessage().toStdString();
    VRP_LOG("eis-pointer: %s", last_error_.c_str());
    return false;
  }
  const QDBusUnixFileDescriptor ufd =
      qvariant_cast<QDBusUnixFileDescriptor>(reply.arguments().at(0));
  cookie_ = reply.arguments().at(1).toInt();
  if (!ufd.isValid()) {
    last_error_ = "KWin connectToEIS: invalid fd";
    cookie_ = -1;
    VRP_LOG("eis-pointer: %s", last_error_.c_str());
    return false;
  }
  const int fd = ::dup(ufd.fileDescriptor());
  if (fd < 0) {
    last_error_ = "dup EIS fd failed";
    cookie_ = -1;
    VRP_LOG("eis-pointer: %s", last_error_.c_str());
    return false;
  }
  if (!core_.start_from_fd(fd, mapping_id_substr)) {
    last_error_ = core_.last_error();
    stop();
    return false;
  }
  VRP_LOG("eis-pointer: KWin EIS ready (cookie=%d mapping=%s region_local=%s)", cookie_,
          mapping_id_substr.empty() ? "(none)" : mapping_id_substr.c_str(),
          core_.region_local() ? "yes" : "no");
  return true;
}

void KdeEisPointer::stop() {
  core_.stop();
  if (cookie_ >= 0) {
    QDBusMessage disc = QDBusMessage::createMethodCall(
        QStringLiteral("org.kde.KWin"), QStringLiteral("/org/kde/KWin/EIS/RemoteDesktop"),
        QStringLiteral("org.kde.KWin.EIS.RemoteDesktop"), QStringLiteral("disconnect"));
    disc << cookie_;
    (void)QDBusConnection::sessionBus().call(disc, QDBus::Block, 1000);
    cookie_ = -1;
  }
}

}  // namespace vrp
