#pragma once

#include <QObject>
#include <QString>

#include <cstdint>

class QDBusPendingCallWatcher;

namespace vrp {

/**
 * GNOME: Mutter ScreenCast.RecordVirtual with is-platform=true.
 *
 * xdg-desktop-portal-gnome omits is-platform (defaults false). Non-platform Meta-*
 * shows up in XRandR and accepts X11 coords, but XWayland/Chrome often never
 * composites into the RecordVirtual framebuffer — HMD stays on wallpaper.
 */
class MutterVirtualScreencast : public QObject {
  Q_OBJECT
 public:
  struct Result {
    uint32_t node_id = 0;
    int eis_fd = -1;  // from Mutter RemoteDesktop.ConnectToEIS (-1 if unavailable)
    QString mapping_id;
  };

  explicit MutterVirtualScreencast(QObject* parent = nullptr);
  ~MutterVirtualScreencast() override;

  void start();
  void stop();
  bool busy() const { return busy_; }

 signals:
  void started(vrp::MutterVirtualScreencast::Result result);
  void failed(const QString& error);
  void status(const QString& message);

 private:
  void fail(const QString& err);
  void callScreenCastCreate();
  void callRecordVirtual();
  void callScreenCastStart();
  void callRemoteDesktopCreate();
  void callRemoteDesktopStart();
  void callConnectToEis(int attempt = 0);
  void emitSuccess();

 private slots:
  void onPipeWireStreamAdded(uint node_id);

 private:
  bool busy_ = false;
  QString sc_session_;
  QString sc_stream_;
  QString rd_session_;
  uint32_t node_id_ = 0;
  int eis_fd_ = -1;
  QString mapping_id_;
  bool have_node_ = false;
  bool rd_done_ = false;
  int eis_attempts_ = 0;
};

}  // namespace vrp

Q_DECLARE_METATYPE(vrp::MutterVirtualScreencast::Result)
