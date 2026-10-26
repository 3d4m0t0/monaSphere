#pragma once

#include <QDBusConnection>
#include <QObject>
#include <QString>
#include <QVariantMap>
#include <functional>

namespace vrp {

/** One-shot listener for org.freedesktop.portal.Request::Response. */
class PortalRequestWatcher : public QObject {
  Q_OBJECT
 public:
  using Callback = std::function<void(uint, const QVariantMap&)>;
  PortalRequestWatcher(const QString& path, Callback cb, QObject* parent = nullptr)
      : QObject(parent), path_(path), cb_(std::move(cb)) {
    QDBusConnection::sessionBus().connect(
        QStringLiteral("org.freedesktop.portal.Desktop"), path_,
        QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"), this,
        SLOT(onResponse(uint, QVariantMap)));
  }
  ~PortalRequestWatcher() override {
    QDBusConnection::sessionBus().disconnect(
        QStringLiteral("org.freedesktop.portal.Desktop"), path_,
        QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"), this,
        SLOT(onResponse(uint, QVariantMap)));
  }

 private slots:
  void onResponse(uint response, const QVariantMap& results) {
    Callback cb = std::move(cb_);
    deleteLater();
    if (cb) cb(response, results);
  }

 private:
  QString path_;
  Callback cb_;
};

}  // namespace vrp
