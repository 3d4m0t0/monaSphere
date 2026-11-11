#include "capture/mutter_virtual_screencast.hpp"

#include "common.hpp"

// VRP_TEARDOWN_LOG

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusUnixFileDescriptor>
#include <QDBusVariant>
#include <QTimer>
#include <QVariantMap>

#include <unistd.h>

namespace vrp {
namespace {

constexpr const char* kScDest = "org.gnome.Mutter.ScreenCast";
constexpr const char* kScPath = "/org/gnome/Mutter/ScreenCast";
constexpr const char* kScIface = "org.gnome.Mutter.ScreenCast";
constexpr const char* kScSessionIface = "org.gnome.Mutter.ScreenCast.Session";
constexpr const char* kScStreamIface = "org.gnome.Mutter.ScreenCast.Stream";

constexpr const char* kRdDest = "org.gnome.Mutter.RemoteDesktop";
constexpr const char* kRdPath = "/org/gnome/Mutter/RemoteDesktop";
constexpr const char* kRdIface = "org.gnome.Mutter.RemoteDesktop";
constexpr const char* kRdSessionIface = "org.gnome.Mutter.RemoteDesktop.Session";

QDBusMessage scCall(const QString& path, const QString& iface, const QString& method) {
  return QDBusMessage::createMethodCall(QString::fromLatin1(kScDest), path, iface, method);
}
QDBusMessage rdCall(const QString& path, const QString& iface, const QString& method) {
  return QDBusMessage::createMethodCall(QString::fromLatin1(kRdDest), path, iface, method);
}

}  // namespace

MutterVirtualScreencast::MutterVirtualScreencast(QObject* parent) : QObject(parent) {
  qRegisterMetaType<vrp::MutterVirtualScreencast::Result>();
}

MutterVirtualScreencast::~MutterVirtualScreencast() { stop(); }

void MutterVirtualScreencast::fail(const QString& err) {
  VRP_LOG("mutter-virtual: fail: %s", err.toUtf8().constData());
  stop();
  busy_ = false;
  emit failed(err);
}

void MutterVirtualScreencast::stop() {
  QDBusConnection bus = QDBusConnection::sessionBus();
  const bool had =
      !sc_session_.isEmpty() || !rd_session_.isEmpty() || !sc_stream_.isEmpty() || node_id_ != 0;
  if (had) {
    VRP_TEARDOWN_LOG("mutter-virtual: stop BEGIN sc=%s rd=%s node=%u",
                     sc_session_.toUtf8().constData(), rd_session_.toUtf8().constData(), node_id_);
  }
  if (!sc_stream_.isEmpty()) {
    bus.disconnect(QString::fromLatin1(kScDest), sc_stream_, QString::fromLatin1(kScStreamIface),
                   QStringLiteral("PipeWireStreamAdded"), this,
                   SLOT(onPipeWireStreamAdded(uint)));
  }
  if (!sc_session_.isEmpty()) {
    VRP_TEARDOWN_LOG("mutter-virtual: ScreenCast.Session.Stop…");
    QDBusMessage msg = scCall(sc_session_, QString::fromLatin1(kScSessionIface),
                              QStringLiteral("Stop"));
    const QDBusMessage reply = bus.call(msg, QDBus::Block, 2000);
    VRP_TEARDOWN_LOG("mutter-virtual: ScreenCast.Session.Stop done err=%s",
                     reply.type() == QDBusMessage::ErrorMessage
                         ? reply.errorMessage().toUtf8().constData()
                         : "(none)");
  }
  if (!rd_session_.isEmpty()) {
    VRP_TEARDOWN_LOG("mutter-virtual: RemoteDesktop.Session.Stop…");
    QDBusMessage msg = rdCall(rd_session_, QString::fromLatin1(kRdSessionIface),
                              QStringLiteral("Stop"));
    const QDBusMessage reply = bus.call(msg, QDBus::Block, 2000);
    VRP_TEARDOWN_LOG("mutter-virtual: RemoteDesktop.Session.Stop done err=%s",
                     reply.type() == QDBusMessage::ErrorMessage
                         ? reply.errorMessage().toUtf8().constData()
                         : "(none)");
  }
  sc_session_.clear();
  sc_stream_.clear();
  rd_session_.clear();
  node_id_ = 0;
  have_node_ = false;
  rd_done_ = false;
  eis_attempts_ = 0;
  mapping_id_.clear();
  if (eis_fd_ >= 0) {
    ::close(eis_fd_);
    eis_fd_ = -1;
  }
  busy_ = false;
  if (had) VRP_TEARDOWN_LOG("mutter-virtual: stop END (Meta-* should be gone)");
}

void MutterVirtualScreencast::start() {
  if (busy_) {
    emit failed(QStringLiteral("Mutter virtual already starting"));
    return;
  }
  stop();
  busy_ = true;
  emit status(QStringLiteral("Mutter: creating platform virtual MONITOR…"));
  callScreenCastCreate();
}

void MutterVirtualScreencast::callScreenCastCreate() {
  VRP_LOG("mutter-virtual: ScreenCast.CreateSession…");
  QDBusMessage msg =
      scCall(QString::fromLatin1(kScPath), QString::fromLatin1(kScIface), QStringLiteral("CreateSession"));
  QVariantMap props;
  msg << props;
  auto* w = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(msg), this);
  QObject::connect(w, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher* w) {
    w->deleteLater();
    QDBusPendingReply<QDBusObjectPath> reply = *w;
    if (reply.isError()) {
      fail(QStringLiteral("Mutter ScreenCast.CreateSession: %1").arg(reply.error().message()));
      return;
    }
    sc_session_ = reply.value().path();
    VRP_LOG("mutter-virtual: session=%s", sc_session_.toUtf8().constData());
    callRecordVirtual();
  });
}

void MutterVirtualScreencast::callRecordVirtual() {
  // cursor-mode: 0=hidden 1=embedded 2=metadata (Mutter MetaScreenCastCursorMode).
  // Hidden only emits on stage damage (long idle / pool-recycle freeze).
  // Embedded keeps the stream alive via seat cursor; CURSOR_ONLY frames may still
  // queue unpainted BOs — capture drops those (empty damage / corrupted chunk).
  VRP_LOG("mutter-virtual: RecordVirtual (is-platform=true, cursor=embedded)…");
  QDBusMessage msg = scCall(sc_session_, QString::fromLatin1(kScSessionIface),
                            QStringLiteral("RecordVirtual"));
  QVariantMap props;
  // Platform Meta: real monitor for window placement / XWayland compositing.
  props.insert(QStringLiteral("is-platform"), true);
  props.insert(QStringLiteral("cursor-mode"), QVariant::fromValue<uint>(1));  // embedded
  msg << props;
  auto* w = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(msg), this);
  QObject::connect(w, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher* w) {
    w->deleteLater();
    QDBusPendingReply<QDBusObjectPath> reply = *w;
    if (reply.isError()) {
      fail(QStringLiteral("Mutter RecordVirtual: %1").arg(reply.error().message()));
      return;
    }
    sc_stream_ = reply.value().path();
    VRP_LOG("mutter-virtual: stream=%s", sc_stream_.toUtf8().constData());

    QDBusConnection bus = QDBusConnection::sessionBus();
    const bool ok = bus.connect(
        QString::fromLatin1(kScDest), sc_stream_, QString::fromLatin1(kScStreamIface),
        QStringLiteral("PipeWireStreamAdded"), this, SLOT(onPipeWireStreamAdded(uint)));
    if (!ok) {
      fail(QStringLiteral("Mutter: cannot subscribe PipeWireStreamAdded"));
      return;
    }
    callScreenCastStart();
  });
}

void MutterVirtualScreencast::callScreenCastStart() {
  VRP_LOG("mutter-virtual: ScreenCast.Start…");
  QDBusMessage msg =
      scCall(sc_session_, QString::fromLatin1(kScSessionIface), QStringLiteral("Start"));
  auto* w = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(msg), this);
  QObject::connect(w, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher* w) {
    w->deleteLater();
    QDBusPendingReply<> reply = *w;
    if (reply.isError()) {
      fail(QStringLiteral("Mutter ScreenCast.Start: %1").arg(reply.error().message()));
      return;
    }
    emit status(QStringLiteral("Mutter: virtual MONITOR started — connecting input…"));
    callRemoteDesktopCreate();
  });
}

void MutterVirtualScreencast::onPipeWireStreamAdded(uint node_id) {
  node_id_ = static_cast<uint32_t>(node_id);
  have_node_ = true;
  VRP_LOG("mutter-virtual: PipeWireStreamAdded node=%u (is-platform Meta)", node_id_);
  emitSuccess();
}

void MutterVirtualScreencast::callRemoteDesktopCreate() {
  VRP_LOG("mutter-virtual: RemoteDesktop.CreateSession…");
  // Mutter RD CreateSession takes no args (unlike ScreenCast / the portal wrapper).
  QDBusMessage msg =
      rdCall(QString::fromLatin1(kRdPath), QString::fromLatin1(kRdIface), QStringLiteral("CreateSession"));
  auto* w = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(msg), this);
  QObject::connect(w, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher* w) {
    w->deleteLater();
    QDBusPendingReply<QDBusObjectPath> reply = *w;
    if (reply.isError()) {
      // Video-only still useful; EIS falls back later.
      VRP_LOG("mutter-virtual: RemoteDesktop.CreateSession failed (%s) — video only",
              reply.error().message().toUtf8().constData());
      rd_done_ = true;
      emitSuccess();
      return;
    }
    rd_session_ = reply.value().path();
    callRemoteDesktopStart();
  });
}

void MutterVirtualScreencast::callRemoteDesktopStart() {
  VRP_LOG("mutter-virtual: RemoteDesktop.Start…");
  QDBusMessage msg =
      rdCall(rd_session_, QString::fromLatin1(kRdSessionIface), QStringLiteral("Start"));
  auto* w = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(msg), this);
  QObject::connect(w, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher* w) {
    w->deleteLater();
    QDBusPendingReply<> reply = *w;
    if (reply.isError()) {
      VRP_LOG("mutter-virtual: RemoteDesktop.Start failed (%s)",
              reply.error().message().toUtf8().constData());
      rd_done_ = true;
      emitSuccess();
      return;
    }
    callConnectToEis();
  });
}

void MutterVirtualScreencast::callConnectToEis(int attempt) {
  eis_attempts_ = attempt;
  VRP_LOG("mutter-virtual: RemoteDesktop.ConnectToEIS (attempt %d)…", attempt + 1);
  // Mutter expects ConnectToEIS(a{sv}) — empty options is fine. Host may still be
  // approving the remote-control dialog; retry a while before giving up.
  QDBusMessage msg =
      rdCall(rd_session_, QString::fromLatin1(kRdSessionIface), QStringLiteral("ConnectToEIS"));
  QVariantMap opts;
  msg << opts;
  const QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 5000);
  if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
    VRP_LOG("mutter-virtual: ConnectToEIS failed (%s)", reply.errorMessage().toUtf8().constData());
    if (busy_ && attempt < 40) {
      emit status(QStringLiteral("Mutter: approve remote control on the host…"));
      QTimer::singleShot(500, this, [this, attempt] {
        if (!busy_ || rd_session_.isEmpty()) return;
        callConnectToEis(attempt + 1);
      });
      return;
    }
    rd_done_ = true;
    emitSuccess();
    return;
  }
  const QDBusUnixFileDescriptor ufd =
      qvariant_cast<QDBusUnixFileDescriptor>(reply.arguments().at(0));
  if (!ufd.isValid()) {
    VRP_LOG("mutter-virtual: ConnectToEIS invalid fd");
    if (busy_ && attempt < 40) {
      QTimer::singleShot(500, this, [this, attempt] {
        if (!busy_ || rd_session_.isEmpty()) return;
        callConnectToEis(attempt + 1);
      });
      return;
    }
    rd_done_ = true;
    emitSuccess();
    return;
  }
  eis_fd_ = ::dup(ufd.fileDescriptor());
  VRP_LOG("mutter-virtual: ConnectToEIS ok (fd=%d)", eis_fd_);
  rd_done_ = true;
  emitSuccess();
}

void MutterVirtualScreencast::emitSuccess() {
  if (!busy_) return;
  if (!have_node_ || !rd_done_) return;
  Result r;
  r.node_id = node_id_;
  r.eis_fd = eis_fd_;
  eis_fd_ = -1;
  r.mapping_id = mapping_id_;
  busy_ = false;
  emit status(QStringLiteral("Mutter: platform Meta-* ready (node %1)").arg(r.node_id));
  emit started(r);
}

}  // namespace vrp
