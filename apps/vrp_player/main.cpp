#include "common.hpp"
#include "host_window.hpp"
#include "i18n.hpp"
#include "options.hpp"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void print_usage(const char* argv0) {
  std::cout
      << "Usage: " << argv0 << " [options] [video]\n"
      << "  Opens the host UI (Monado launch, log, HMD status).\n"
      << "  -p flat|180|360     Projection (default flat)\n"
      << "  -l mono|sbs|ou      Stereo layout (default mono)\n"
      << "  -f <deg>            Flat FOV degrees (default 70)\n"
      << "  -d <meters>         Screen distance (default 4)\n"
      << "  --clear-only        Skip video decode\n"
      << "  --no-ui             Headless OpenXR loop (no host window)\n"
      << "  -h                  Help\n";
}

vrp::PlayerOptions parse_args(int argc, char** argv, bool& show_ui) {
  vrp::PlayerOptions opt;
  show_ui = true;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      print_usage(argv[0]);
      std::exit(0);
    } else if (a == "--no-ui") {
      show_ui = false;
    } else if (a == "-p" && i + 1 < argc) {
      std::string v = argv[++i];
      if (v == "flat") opt.projection = vrp::ProjectionMode::Flat;
      else if (v == "180") opt.projection = vrp::ProjectionMode::Deg180;
      else if (v == "360") opt.projection = vrp::ProjectionMode::Deg360;
      else vrp_fatal("Unknown -p value");
    } else if (a == "-l" && i + 1 < argc) {
      std::string v = argv[++i];
      if (v == "mono") opt.stereo = vrp::StereoLayout::Mono;
      else if (v == "sbs") opt.stereo = vrp::StereoLayout::Sbs;
      else if (v == "ou") opt.stereo = vrp::StereoLayout::OverUnder;
      else vrp_fatal("Unknown -l value");
    } else if (a == "-f" && i + 1 < argc) {
      opt.flat_fov_deg = std::strtof(argv[++i], nullptr);
    } else if (a == "-d" && i + 1 < argc) {
      opt.screen_distance = std::strtof(argv[++i], nullptr);
    } else if (a == "--clear-only") {
      opt.clear_only = true;
    } else if (!a.empty() && a[0] != '-') {
      opt.video_path = a;
    } else {
      vrp_fatal("Unknown arg: " + a);
    }
  }
  return opt;
}

QString find_shader_dir() {
  const char* candidates[] = {
      "shaders",
      "../shaders",
      "build/shaders",
      "/usr/share/monasphere/shaders",
      "/usr/local/share/monasphere/shaders",
      nullptr,
  };
  for (int i = 0; candidates[i]; ++i) {
    QString p = QString(candidates[i]) + "/scene.vert.spv";
    if (QFileInfo::exists(p)) return candidates[i];
  }
  return "shaders";
}

QString instanceSocketName() {
  QString dir = qEnvironmentVariable("XDG_RUNTIME_DIR");
  if (dir.isEmpty()) dir = QDir::temp().absolutePath();
  return dir + QStringLiteral("/monasphere.sock");
}

QByteArray startupActivationToken() {
  const char* token = std::getenv("XDG_ACTIVATION_TOKEN");
  return token ? QByteArray(token) : QByteArray();
}

bool notifyPrimary(const QString& name, const QByteArray& activation_token) {
  QLocalSocket sock;
  sock.connectToServer(name);
  if (!sock.waitForConnected(400)) return false;
  QByteArray msg = QByteArrayLiteral("raise");
  if (!activation_token.isEmpty()) {
    msg += ' ';
    msg += activation_token;
  }
  msg += '\n';
  sock.write(msg);
  sock.flush();
  sock.waitForBytesWritten(400);
  return true;
}

/**
 * True when this process should open the window.
 * *server is set when this process owns the single-instance socket.
 */
bool claimInstance(QApplication& app, QLocalServer** server, const QByteArray& activation_token) {
  *server = nullptr;
  const QString name = instanceSocketName();
  if (notifyPrimary(name, activation_token)) return false;
  QLocalServer::removeServer(name);
  auto* created = new QLocalServer(&app);
  created->setSocketOptions(QLocalServer::UserAccessOption);
  if (!created->listen(name)) {
    if (notifyPrimary(name, activation_token)) {
      delete created;
      return false;
    }
    VRP_ERR("single-instance socket: %s", created->errorString().toUtf8().constData());
    delete created;
    return true;
  }
  *server = created;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  bool show_ui = true;
  vrp::PlayerOptions opt;
  try {
    opt = parse_args(argc, argv, show_ui);
  } catch (const std::exception& e) {
    VRP_ERR("%s", e.what());
    return 1;
  }

  const QByteArray activation_token = startupActivationToken();

  QApplication app(argc, argv);
  QLocalServer* instance = nullptr;
  if (!claimInstance(app, &instance, activation_token)) return 0;
  MonasphereInstallTranslations(app);
  QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/monasphere.png")));
  QApplication::setOrganizationName(QStringLiteral("monaSphere"));
  QApplication::setApplicationName(QStringLiteral("monaSphere"));
  QApplication::setApplicationDisplayName(QStringLiteral("monaSphere"));
  QGuiApplication::setDesktopFileName(QStringLiteral("monasphere"));
  if (!show_ui) {
    // Still use the host window but auto-connect; window remains the control surface.
    show_ui = true;
  }
  HostWindow window(opt, find_shader_dir());
  if (instance) {
    QObject::connect(instance, &QLocalServer::newConnection, &window, [instance, &window] {
      while (QLocalSocket* sock = instance->nextPendingConnection()) {
        if (sock->bytesAvailable() == 0) sock->waitForReadyRead(100);
        const QByteArray line = sock->readAll();
        QString token;
        const int space = line.indexOf(' ');
        const int nl = line.indexOf('\n');
        if (space >= 0 && (nl < 0 || space < nl)) {
          const int end = nl < 0 ? line.size() : nl;
          token = QString::fromUtf8(line.mid(space + 1, end - space - 1)).trimmed();
        }
        window.presentToFront(token);
        sock->disconnectFromServer();
        sock->deleteLater();
      }
    });
  }
  window.show();
  // Do not force --no-ui auto-connect here; the host UI auto-connects when Monado is ready
  // and exactly one known HMD is present.
  return app.exec();
}
