#include "capture/screencast_portal.hpp"
#include "capture/portal_request.hpp"

#include "common.hpp"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusUnixFileDescriptor>
#include <QDBusVariant>
#include <QUuid>
#include <QVariantList>

#include <unistd.h>

namespace vrp {

struct PortalStream {
  uint node_id = 0;
  QVariantMap props;
};
using PortalStreams = QList<PortalStream>;

QDBusArgument& operator<<(QDBusArgument& arg, const PortalStream& s) {
  arg.beginStructure();
  arg << s.node_id << s.props;
  arg.endStructure();
  return arg;
}

const QDBusArgument& operator>>(const QDBusArgument& arg, PortalStream& s) {
  arg.beginStructure();
  arg >> s.node_id >> s.props;
  arg.endStructure();
  return arg;
}

}  // namespace vrp

Q_DECLARE_METATYPE(vrp::PortalStream)
Q_DECLARE_METATYPE(vrp::PortalStreams)

namespace vrp {
namespace {

constexpr const char* kPortal = "org.freedesktop.portal.Desktop";
constexpr const char* kPath = "/org/freedesktop/portal/desktop";
constexpr const char* kScreenCast = "org.freedesktop.portal.ScreenCast";
constexpr const char* kRemoteDesktop = "org.freedesktop.portal.RemoteDesktop";
constexpr uint32_t kSourceMonitor = 0x1;
constexpr uint32_t kSourceWindow = 0x2;
constexpr uint32_t kCursorHidden = 0x1;
constexpr uint32_t kCursorEmbedded = 0x2;
constexpr uint32_t kDeviceKeyboard = 0x1;
constexpr uint32_t kDevicePointer = 0x2;

QString makeToken(const char* prefix) {
  return QString::fromLatin1(prefix) + QUuid::createUuid().toString(QUuid::Id128);
}

QString portalSenderToken() {
  QString unique = QDBusConnection::sessionBus().baseService();
  if (unique.startsWith(QLatin1Char(':'))) unique.remove(0, 1);
  unique.replace(QLatin1Char('.'), QLatin1Char('_'));
  return unique;
}

QString portalRequestPath(const QString& handle_token) {
  return QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2")
      .arg(portalSenderToken(), handle_token);
}

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

uint32_t readPortalUintProp(const char* iface, const char* name, uint32_t fallback) {
  QDBusMessage msg = QDBusMessage::createMethodCall(
      QString::fromLatin1(kPortal), QString::fromLatin1(kPath),
      QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
  msg << QString::fromLatin1(iface) << QString::fromLatin1(name);
  const QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 3000);
  if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) return fallback;
  const QVariant v = unwrapDbus(reply.arguments().at(0));
  bool ok = false;
  const uint32_t n = v.toUInt(&ok);
  return ok ? n : fallback;
}

QString portalMapString(const QVariantMap& m, const QString& key) {
  if (!m.contains(key)) return {};
  const QVariant v = unwrapDbus(m.value(key));
  if (v.canConvert<QDBusObjectPath>()) {
    const QDBusObjectPath op = qvariant_cast<QDBusObjectPath>(v);
    if (!op.path().isEmpty()) return op.path();
  }
  if (v.typeId() == QMetaType::QString || v.canConvert<QString>()) {
    const QString s = v.toString();
    if (!s.isEmpty()) return s;
  }
  if (v.canConvert<QByteArray>()) {
    const QByteArray ba = v.toByteArray();
    if (!ba.isEmpty()) {
      const QString as_utf8 = QString::fromUtf8(ba);
      if (!as_utf8.isEmpty() && !as_utf8.contains(QChar('\0'))) return as_utf8;
      return QString::fromLatin1(ba.toBase64());
    }
  }
  return {};
}

bool parseStreams(const QVariant& streams_v, uint32_t& node_id, QVariantMap& props, QString& err) {
  {
    const PortalStreams streams = qdbus_cast<PortalStreams>(streams_v);
    if (!streams.isEmpty()) {
      node_id = streams.first().node_id;
      props = streams.first().props;
      return true;
    }
  }

  if (streams_v.canConvert<QDBusArgument>()) {
    QDBusArgument arg = qvariant_cast<QDBusArgument>(streams_v);
    if (arg.currentType() == QDBusArgument::ArrayType) {
      arg.beginArray();
      if (arg.atEnd()) {
        err = QStringLiteral("Start: empty streams");
        return false;
      }
      PortalStream s;
      arg >> s;
      arg.endArray();
      node_id = s.node_id;
      props = s.props;
      return true;
    }
  }

  if (streams_v.canConvert<QVariantList>()) {
    const QVariantList list = streams_v.toList();
    if (list.isEmpty()) {
      err = QStringLiteral("Start: empty streams");
      return false;
    }
    const QVariant first = list.first();
    if (first.canConvert<QVariantList>()) {
      const QVariantList tup = first.toList();
      if (tup.size() >= 2) {
        node_id = tup.at(0).toUInt();
        props = tup.at(1).toMap();
        return true;
      }
    }
  }

  err = QStringLiteral("Start: streams not an array (type=%1)")
            .arg(QString::fromLatin1(streams_v.typeName()));
  return false;
}

void parseSizeProp(const QVariantMap& props, int& width, int& height) {
  if (!props.contains(QStringLiteral("size"))) return;
  const QVariant sz_v = props.value(QStringLiteral("size"));
  if (sz_v.canConvert<QDBusArgument>()) {
    const QDBusArgument sz = qvariant_cast<QDBusArgument>(sz_v);
    if (sz.currentType() == QDBusArgument::StructureType) {
      sz.beginStructure();
      sz >> width >> height;
      sz.endStructure();
      return;
    }
  }
  if (sz_v.canConvert<QVariantList>()) {
    const QVariantList l = sz_v.toList();
    if (l.size() >= 2) {
      width = l.at(0).toInt();
      height = l.at(1).toInt();
    }
  }
}

}  // namespace

ScreencastPortal::ScreencastPortal(QObject* parent) : QObject(parent) {
  qDBusRegisterMetaType<PortalStream>();
  qDBusRegisterMetaType<PortalStreams>();
  qDBusRegisterMetaType<QVariantMap>();
  qRegisterMetaType<vrp::ScreencastPortal::Result>("vrp::ScreencastPortal::Result");
}

ScreencastPortal::~ScreencastPortal() { stop(); }

void ScreencastPortal::fail(const QString& err) {
  VRP_LOG("screencast: FAIL %s", err.toUtf8().constData());
  if (eis_fd_ >= 0) {
    ::close(eis_fd_);
    eis_fd_ = -1;
  }
  busy_ = false;
  emit failed(err);
}

void ScreencastPortal::start(const QString& parent_window, const QString& restore_token,
                             uint32_t persist_mode, SourceKind kind, SessionMode mode) {
  if (busy_) {
    fail(QStringLiteral("ScreenCast already starting"));
    return;
  }
  if (!session_path_.isEmpty()) {
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QString::fromLatin1(kPortal), session_path_, QStringLiteral("org.freedesktop.portal.Session"),
        QStringLiteral("Close"));
    QDBusConnection::sessionBus().asyncCall(msg);
    session_path_.clear();
  }
  if (eis_fd_ >= 0) {
    ::close(eis_fd_);
    eis_fd_ = -1;
  }
  busy_ = true;
  parent_window_ = parent_window;
  restore_token_in_ = restore_token;
  persist_mode_ = persist_mode;
  kind_ = kind;
  mode_ = mode;
  node_id_ = 0;
  pw_serial_ = 0;
  width_ = height_ = 0;
  restore_token_out_.clear();
  mapping_id_.clear();

  source_types_ =
      readPortalUintProp(kScreenCast, "AvailableSourceTypes", kSourceMonitor | kSourceWindow);
  cursor_modes_ = readPortalUintProp(kScreenCast, "AvailableCursorModes", kCursorHidden);
  portal_version_ = readPortalUintProp(kScreenCast, "version", 1);
  remote_desktop_version_ = readPortalUintProp(kRemoteDesktop, "version", 1);

  if (mode_ == SessionMode::RemoteDesktopWithVideo && remote_desktop_version_ < 2) {
    fail(QStringLiteral("RemoteDesktop portal v%1 < 2 (ConnectToEIS required for GNOME input)")
             .arg(remote_desktop_version_));
    return;
  }

  const uint32_t want = static_cast<uint32_t>(kind_);
  if ((source_types_ & want) == 0) {
    if (kind_ == SourceKind::Monitor && (source_types_ & kSourceWindow)) {
      kind_ = SourceKind::Window;
      VRP_LOG("screencast: MONITOR unavailable — falling back to WINDOW");
    } else if (kind_ == SourceKind::Window && (source_types_ & kSourceMonitor)) {
      kind_ = SourceKind::Monitor;
      VRP_LOG("screencast: WINDOW unavailable — falling back to MONITOR");
    } else {
      fail(QStringLiteral("Portal ScreenCast has no usable source (AvailableSourceTypes=0x%1)")
               .arg(source_types_, 0, 16));
      return;
    }
  }

  VRP_LOG("screencast: mode=%s ScreenCast v%u RemoteDesktop v%u sources=0x%x cursors=0x%x "
          "kind=%s restore=%s parent=%s",
          mode_ == SessionMode::RemoteDesktopWithVideo ? "RD+SC" : "SC", portal_version_,
          remote_desktop_version_, source_types_, cursor_modes_,
          kind_ == SourceKind::Monitor ? "MONITOR" : "WINDOW",
          restore_token.isEmpty() ? "no" : "yes",
          parent_window.isEmpty() ? "(none)" : parent_window.toUtf8().constData());
  const QString st =
      restore_token.isEmpty()
          ? (mode_ == SessionMode::RemoteDesktopWithVideo
                 ? QStringLiteral("ScreenCast+input: opening picker (host desktop)…")
                 : QStringLiteral("ScreenCast: opening picker (host desktop)…"))
          : (mode_ == SessionMode::RemoteDesktopWithVideo
                 ? QStringLiteral("ScreenCast+input: restoring previous share…")
                 : QStringLiteral("ScreenCast: restoring previous share…"));
  VRP_LOG("screencast: %s", st.toUtf8().constData());
  emit status(st);
  callCreateSession();
}

void ScreencastPortal::stop() {
  closing_session_ = true;
  unwatchSessionClosed();
  if (eis_fd_ >= 0) {
    ::close(eis_fd_);
    eis_fd_ = -1;
  }
  if (!session_path_.isEmpty()) {
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QString::fromLatin1(kPortal), session_path_, QStringLiteral("org.freedesktop.portal.Session"),
        QStringLiteral("Close"));
    QDBusConnection::sessionBus().asyncCall(msg);
    session_path_.clear();
  }
  const bool was_busy = busy_;
  busy_ = false;
  closing_session_ = false;
  if (was_busy) emit stopped();
}

void ScreencastPortal::watchSessionClosed() {
  if (session_path_.isEmpty() || watching_session_closed_) return;
  watching_session_closed_ = QDBusConnection::sessionBus().connect(
      QString::fromLatin1(kPortal), session_path_,
      QStringLiteral("org.freedesktop.portal.Session"), QStringLiteral("Closed"), this,
      SLOT(onSessionClosed()));
  if (!watching_session_closed_) {
    VRP_LOG("screencast: could not watch Session.Closed on %s",
            session_path_.toUtf8().constData());
  }
}

void ScreencastPortal::unwatchSessionClosed() {
  if (!watching_session_closed_ || session_path_.isEmpty()) {
    watching_session_closed_ = false;
    return;
  }
  QDBusConnection::sessionBus().disconnect(
      QString::fromLatin1(kPortal), session_path_,
      QStringLiteral("org.freedesktop.portal.Session"), QStringLiteral("Closed"), this,
      SLOT(onSessionClosed()));
  watching_session_closed_ = false;
}

void ScreencastPortal::onSessionClosed() {
  VRP_LOG("screencast: Session.Closed%s", closing_session_ ? " (ours)" : " (host/tray stop)");
  watching_session_closed_ = false;
  session_path_.clear();
  if (eis_fd_ >= 0) {
    ::close(eis_fd_);
    eis_fd_ = -1;
  }
  busy_ = false;
  if (!closing_session_) emit stopped();
}

void ScreencastPortal::watchRequest(const QString& path,
                                    std::function<void(uint, const QVariantMap&)> cb) {
  VRP_LOG("screencast: watching Request %s (before method call)", path.toUtf8().constData());
  new PortalRequestWatcher(path, std::move(cb), this);
}

void ScreencastPortal::callCreateSession() {
  const bool rd = mode_ == SessionMode::RemoteDesktopWithVideo;
  VRP_LOG("screencast: %s.CreateSession…", rd ? "RemoteDesktop" : "ScreenCast");
  const QString handle = makeToken("monasphere_cs");
  const QString sess = makeToken("monasphere_sess");
  const QString req_path = portalRequestPath(handle);
  watchRequest(req_path, [this](uint response, const QVariantMap& results) {
    VRP_LOG("screencast: CreateSession Response=%u", response);
    if (response != 0) {
      fail(QStringLiteral("CreateSession cancelled (%1)").arg(response));
      return;
    }
    session_path_ = portalMapString(results, QStringLiteral("session_handle"));
    if (session_path_.isEmpty()) {
      fail(QStringLiteral("CreateSession: no session_handle"));
      return;
    }
    watchSessionClosed();
    VRP_LOG("screencast: session=%s", session_path_.toUtf8().constData());
    if (mode_ == SessionMode::RemoteDesktopWithVideo) {
      emit status(QStringLiteral("ScreenCast+input: selecting pointer devices…"));
      callSelectDevices();
    } else {
      emit status(QStringLiteral("ScreenCast: selecting Virtual MONITOR…"));
      callSelectSources();
    }
  });

  QVariantMap opts;
  opts.insert(QStringLiteral("handle_token"), handle);
  opts.insert(QStringLiteral("session_handle_token"), sess);
  QDBusMessage msg = QDBusMessage::createMethodCall(
      QString::fromLatin1(kPortal), QString::fromLatin1(kPath),
      QString::fromLatin1(rd ? kRemoteDesktop : kScreenCast), QStringLiteral("CreateSession"));
  msg << opts;
  auto* w = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(msg), this);
  QObject::connect(w, &QDBusPendingCallWatcher::finished, this, &ScreencastPortal::onCreateSessionDone);
}

void ScreencastPortal::onCreateSessionDone(QDBusPendingCallWatcher* w) {
  w->deleteLater();
  QDBusPendingReply<QDBusObjectPath> reply = *w;
  if (reply.isError()) {
    fail(reply.error().message());
    return;
  }
  VRP_LOG("screencast: CreateSession reply path=%s", reply.value().path().toUtf8().constData());
}

void ScreencastPortal::callSelectDevices() {
  VRP_LOG("screencast: SelectDevices (pointer+keyboard restore=%s)…",
          restore_token_in_.isEmpty() ? "no" : "yes");
  const QString handle = makeToken("monasphere_sd");
  const QString req_path = portalRequestPath(handle);
  watchRequest(req_path, [this](uint response, const QVariantMap&) {
    VRP_LOG("screencast: SelectDevices Response=%u", response);
    if (response != 0) {
      fail(restore_token_in_.isEmpty()
               ? QStringLiteral("SelectDevices cancelled (%1)").arg(response)
               : QStringLiteral("SelectDevices restore rejected (%1)").arg(response));
      return;
    }
    emit status(QStringLiteral("ScreenCast+input: selecting MONITOR…"));
    callSelectSources();
  });

  QVariantMap opts;
  opts.insert(QStringLiteral("handle_token"), handle);
  opts.insert(QStringLiteral("types"),
              QVariant::fromValue<uint>(kDeviceKeyboard | kDevicePointer));
  if (remote_desktop_version_ >= 2) {
    opts.insert(QStringLiteral("persist_mode"), QVariant::fromValue<uint>(persist_mode_));
    if (!restore_token_in_.isEmpty()) {
      opts.insert(QStringLiteral("restore_token"), restore_token_in_);
    }
  }

  QDBusMessage msg = QDBusMessage::createMethodCall(
      QString::fromLatin1(kPortal), QString::fromLatin1(kPath), QString::fromLatin1(kRemoteDesktop),
      QStringLiteral("SelectDevices"));
  msg << QVariant::fromValue(QDBusObjectPath(session_path_)) << opts;
  auto* w = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(msg), this);
  QObject::connect(w, &QDBusPendingCallWatcher::finished, this, &ScreencastPortal::onSelectDevicesDone);
}

void ScreencastPortal::onSelectDevicesDone(QDBusPendingCallWatcher* w) {
  w->deleteLater();
  QDBusPendingReply<QDBusObjectPath> reply = *w;
  if (reply.isError()) {
    fail(reply.error().message());
    return;
  }
  VRP_LOG("screencast: SelectDevices reply path=%s", reply.value().path().toUtf8().constData());
}

void ScreencastPortal::callSelectSources() {
  VRP_LOG("screencast: SelectSources (kind=%s)…",
          kind_ == SourceKind::Monitor ? "MONITOR" : "WINDOW");
  const QString handle = makeToken("monasphere_ss");
  const QString req_path = portalRequestPath(handle);
  watchRequest(req_path, [this](uint response, const QVariantMap&) {
    VRP_LOG("screencast: SelectSources Response=%u", response);
    if (response != 0) {
      fail(QStringLiteral("SelectSources cancelled (%1)").arg(response));
      return;
    }
    const bool rd = mode_ == SessionMode::RemoteDesktopWithVideo;
    const QString st =
        restore_token_in_.isEmpty()
            ? (rd ? QStringLiteral("ScreenCast+input: waiting for host approval…")
                  : QStringLiteral(
                        "ScreenCast: waiting for host approval (pick Virtual-monaSphere)…"))
            : (rd ? QStringLiteral("ScreenCast+input: starting restored share…")
                  : QStringLiteral("ScreenCast: starting restored share…"));
    VRP_LOG("screencast: %s", st.toUtf8().constData());
    emit status(st);
    callStart();
  });

  QVariantMap opts;
  opts.insert(QStringLiteral("handle_token"), handle);
  opts.insert(QStringLiteral("types"), QVariant::fromValue<uint>(static_cast<uint>(kind_)));
  opts.insert(QStringLiteral("multiple"), false);

  if (cursor_modes_ & kCursorEmbedded) {
    opts.insert(QStringLiteral("cursor_mode"), QVariant::fromValue<uint>(kCursorEmbedded));
    VRP_LOG("screencast: cursor_mode=Embedded (DE cursor in stream)");
  } else {
    VRP_LOG("screencast: WARNING portal has no Embedded cursor (AvailableCursorModes=0x%x)",
            cursor_modes_);
  }

  // Persist on ScreenCast only for VideoOnly. RD path persists via SelectDevices.
  if (mode_ == SessionMode::VideoOnly && portal_version_ >= 4) {
    opts.insert(QStringLiteral("persist_mode"), QVariant::fromValue<uint>(persist_mode_));
    if (!restore_token_in_.isEmpty()) {
      opts.insert(QStringLiteral("restore_token"), restore_token_in_);
    }
    VRP_LOG("screencast: SelectSources persist_mode=%u restore_in=%s", persist_mode_,
            restore_token_in_.isEmpty() ? "no" : "yes");
  }

  QDBusMessage msg =
      QDBusMessage::createMethodCall(QString::fromLatin1(kPortal), QString::fromLatin1(kPath),
                                     QString::fromLatin1(kScreenCast), QStringLiteral("SelectSources"));
  msg << QVariant::fromValue(QDBusObjectPath(session_path_)) << opts;
  auto* w = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(msg), this);
  QObject::connect(w, &QDBusPendingCallWatcher::finished, this, &ScreencastPortal::onSelectSourcesDone);
}

void ScreencastPortal::onSelectSourcesDone(QDBusPendingCallWatcher* w) {
  w->deleteLater();
  QDBusPendingReply<QDBusObjectPath> reply = *w;
  if (reply.isError()) {
    fail(reply.error().message());
    return;
  }
  VRP_LOG("screencast: SelectSources reply path=%s", reply.value().path().toUtf8().constData());
}

void ScreencastPortal::callStart() {
  const bool rd = mode_ == SessionMode::RemoteDesktopWithVideo;
  VRP_LOG("screencast: %s.Start (parent=%s)…", rd ? "RemoteDesktop" : "ScreenCast",
          parent_window_.isEmpty() ? "(none)" : parent_window_.toUtf8().constData());
  const QString handle = makeToken("monasphere_st");
  const QString req_path = portalRequestPath(handle);
  watchRequest(req_path, [this](uint response, const QVariantMap& results) {
    VRP_LOG("screencast: Start Response=%u", response);
    if (response != 0) {
      if (!restore_token_in_.isEmpty()) {
        fail(QStringLiteral("Start restore rejected (%1)").arg(response));
      } else if (kind_ == SourceKind::Monitor) {
        fail(QStringLiteral("Start cancelled (%1) — on the host, pick the live Virtual MONITOR")
                 .arg(response));
      } else {
        fail(QStringLiteral("Start cancelled (%1) — on the host, pick window class monaSphereYT")
                 .arg(response));
      }
      return;
    }
    restore_token_out_ = portalMapString(results, QStringLiteral("restore_token"));
    if (restore_token_out_.isEmpty() && !restore_token_in_.isEmpty()) {
      restore_token_out_ = restore_token_in_;
      VRP_LOG("screencast: Start returned no restore_token — keeping previous");
    }
    QVariantMap props;
    QString perr;
    if (!parseStreams(unwrapDbus(results.value(QStringLiteral("streams"))), node_id_, props,
                      perr)) {
      fail(perr);
      return;
    }
    parseSizeProp(props, width_, height_);
    if (props.contains(QStringLiteral("pipewire-serial"))) {
      pw_serial_ = unwrapDbus(props.value(QStringLiteral("pipewire-serial"))).toULongLong();
    }
    mapping_id_ = portalMapString(props, QStringLiteral("mapping_id"));
    if (!mapping_id_.isEmpty()) {
      VRP_LOG("screencast: stream mapping_id=%s", mapping_id_.toUtf8().constData());
    }
    VRP_LOG("screencast: node=%u serial=%llu size=%dx%d token=%s (len=%d)", node_id_,
            static_cast<unsigned long long>(pw_serial_), width_, height_,
            restore_token_out_.isEmpty() ? "(none)" : "yes", restore_token_out_.size());
    openPipeWireRemote();
  });

  QVariantMap opts;
  opts.insert(QStringLiteral("handle_token"), handle);
  QDBusMessage msg = QDBusMessage::createMethodCall(
      QString::fromLatin1(kPortal), QString::fromLatin1(kPath),
      QString::fromLatin1(rd ? kRemoteDesktop : kScreenCast), QStringLiteral("Start"));
  msg << QVariant::fromValue(QDBusObjectPath(session_path_)) << parent_window_ << opts;
  auto* w = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(msg), this);
  QObject::connect(w, &QDBusPendingCallWatcher::finished, this, &ScreencastPortal::onStartDone);
}

void ScreencastPortal::onStartDone(QDBusPendingCallWatcher* w) {
  w->deleteLater();
  QDBusPendingReply<QDBusObjectPath> reply = *w;
  if (reply.isError()) {
    fail(reply.error().message());
    return;
  }
  VRP_LOG("screencast: Start reply path=%s", reply.value().path().toUtf8().constData());
}

void ScreencastPortal::openPipeWireRemote() {
  QDBusMessage msg =
      QDBusMessage::createMethodCall(QString::fromLatin1(kPortal), QString::fromLatin1(kPath),
                                     QString::fromLatin1(kScreenCast), QStringLiteral("OpenPipeWireRemote"));
  QVariantMap opts;
  msg << QVariant::fromValue(QDBusObjectPath(session_path_)) << opts;
  QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 5000);
  if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
    fail(QStringLiteral("OpenPipeWireRemote failed: %1").arg(reply.errorMessage()));
    return;
  }
  const QDBusUnixFileDescriptor ufd = qvariant_cast<QDBusUnixFileDescriptor>(reply.arguments().at(0));
  if (!ufd.isValid()) {
    fail(QStringLiteral("OpenPipeWireRemote: invalid fd"));
    return;
  }
  // Stash pw fd in Result via emitStarted after optional ConnectToEIS.
  // Temporarily keep dup in a local until emitStarted.
  const int pw = ::dup(ufd.fileDescriptor());
  if (pw < 0) {
    fail(QStringLiteral("OpenPipeWireRemote: dup failed"));
    return;
  }

  if (mode_ == SessionMode::RemoteDesktopWithVideo) {
    connectToEis();
    if (eis_fd_ < 0) {
      ::close(pw);
      // connectToEis already failed()
      return;
    }
  }

  Result r;
  r.node_id = node_id_;
  r.pipewire_serial = pw_serial_;
  r.pw_fd = pw;
  r.eis_fd = eis_fd_;
  eis_fd_ = -1;  // transferred to Result
  r.width = width_;
  r.height = height_;
  r.restore_token = restore_token_out_;
  r.mapping_id = mapping_id_;
  busy_ = false;
  emit started(r);
}

void ScreencastPortal::connectToEis() {
  VRP_LOG("screencast: RemoteDesktop.ConnectToEIS…");
  QDBusMessage msg = QDBusMessage::createMethodCall(
      QString::fromLatin1(kPortal), QString::fromLatin1(kPath), QString::fromLatin1(kRemoteDesktop),
      QStringLiteral("ConnectToEIS"));
  QVariantMap opts;
  msg << QVariant::fromValue(QDBusObjectPath(session_path_)) << opts;
  const QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 5000);
  if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
    fail(QStringLiteral("ConnectToEIS failed: %1").arg(reply.errorMessage()));
    return;
  }
  const QDBusUnixFileDescriptor ufd = qvariant_cast<QDBusUnixFileDescriptor>(reply.arguments().at(0));
  if (!ufd.isValid()) {
    fail(QStringLiteral("ConnectToEIS: invalid fd"));
    return;
  }
  eis_fd_ = ::dup(ufd.fileDescriptor());
  if (eis_fd_ < 0) {
    fail(QStringLiteral("ConnectToEIS: dup failed"));
    return;
  }
  VRP_LOG("screencast: ConnectToEIS ok (fd=%d)", eis_fd_);
}

}  // namespace vrp
