#include "common.hpp"
#include "host_window.hpp"
#include "i18n.hpp"
#include "options.hpp"

#include <QApplication>
#include <QFileInfo>
#include <QIcon>

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

  QApplication app(argc, argv);
  MonasphereInstallTranslations(app);
  QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/monasphere.png")));
  QApplication::setOrganizationName(QStringLiteral("monaSphere"));
  QApplication::setApplicationName(QStringLiteral("monaSphere"));
  QApplication::setApplicationDisplayName(QStringLiteral("monaSphere"));
  if (!show_ui) {
    // Still use the host window but auto-connect; window remains the control surface.
    show_ui = true;
  }
  HostWindow window(opt, find_shader_dir());
  window.show();
  // Do not force --no-ui auto-connect here; the host UI auto-connects when Monado is ready
  // and exactly one known HMD is present.
  return app.exec();
}
