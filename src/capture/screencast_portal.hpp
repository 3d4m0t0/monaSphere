#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <cstdint>
#include <functional>
#include <string>

class QDBusPendingCallWatcher;

namespace vrp {

/**
 * xdg-desktop-portal ScreenCast, optionally combined with RemoteDesktop.
 *
 * VideoOnly (KDE): ScreenCast session alone so Plasma shows the MONITOR picker.
 * RemoteDesktopWithVideo (GNOME): RD session + SelectSources + ConnectToEIS.
 *
 * Must live on the Qt GUI / session-bus thread.
 */
class ScreencastPortal : public QObject {
  Q_OBJECT
 public:
  enum class SourceKind : uint32_t {
    Monitor = 0x1,
    Window = 0x2,
  };

  enum class SessionMode {
    VideoOnly,
    RemoteDesktopWithVideo,
  };

  explicit ScreencastPortal(QObject* parent = nullptr);
  ~ScreencastPortal() override;

  struct Result {
    uint32_t node_id = 0;
    uint64_t pipewire_serial = 0;
    int pw_fd = -1;   // owned by caller after success (dup'd)
    int eis_fd = -1;  // owned by caller; only RemoteDesktopWithVideo (ConnectToEIS)
    int width = 0;
    int height = 0;
    QString restore_token;
    QString mapping_id;  // ScreenCast stream prop for libei region pairing
  };

  /**
   * persist_mode 2 = until revoked. Empty restore_token → interactive picker.
   * mode: VideoOnly for KDE; RemoteDesktopWithVideo for GNOME pointer+video.
   */
  void start(const QString& parent_window, const QString& restore_token = {},
             uint32_t persist_mode = 2, SourceKind kind = SourceKind::Monitor,
             SessionMode mode = SessionMode::VideoOnly);
  void stop();

  bool busy() const { return busy_; }
  SourceKind active_kind() const { return kind_; }
  SessionMode session_mode() const { return mode_; }
  /** True if the in-flight / last start used a restore_token. */
  bool used_restore_token() const { return !restore_token_in_.isEmpty(); }

 signals:
  void started(vrp::ScreencastPortal::Result result);
  void failed(const QString& error);
  void stopped();
  /** Progress for the host log / HMD toast. */
  void status(const QString& message);

 private:
  void fail(const QString& err);
  void callCreateSession();
  void onCreateSessionDone(QDBusPendingCallWatcher* w);
  void callSelectDevices();
  void onSelectDevicesDone(QDBusPendingCallWatcher* w);
  void callSelectSources();
  void onSelectSourcesDone(QDBusPendingCallWatcher* w);
  void callStart();
  void onStartDone(QDBusPendingCallWatcher* w);
  void watchRequest(const QString& path, std::function<void(uint, const QVariantMap&)> cb);
  void openPipeWireRemote();
  void connectToEis();
  void watchSessionClosed();
  void unwatchSessionClosed();

 private slots:
  void onSessionClosed();

 private:
  bool busy_ = false;
  bool closing_session_ = false;
  bool watching_session_closed_ = false;
  QString parent_window_;
  QString restore_token_in_;
  uint32_t persist_mode_ = 2;
  SourceKind kind_ = SourceKind::Monitor;
  SessionMode mode_ = SessionMode::VideoOnly;
  QString session_path_;
  uint32_t source_types_ = 0;
  uint32_t cursor_modes_ = 0;
  uint32_t portal_version_ = 1;          // ScreenCast
  uint32_t remote_desktop_version_ = 1;  // RemoteDesktop
  uint32_t node_id_ = 0;
  uint64_t pw_serial_ = 0;
  int width_ = 0;
  int height_ = 0;
  int eis_fd_ = -1;
  QString restore_token_out_;
  QString mapping_id_;
};

}  // namespace vrp

Q_DECLARE_METATYPE(vrp::ScreencastPortal::Result)
