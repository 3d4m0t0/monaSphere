#pragma once

#include "ui/vr_menu.hpp"

#include <QImage>

namespace vrp {

/** Paint HMD menu snapshot to RGBA8 QImage (call on Qt GUI thread). */
QImage paint_vr_menu(const VrMenu::Snapshot& snap, int width = 1280, int height = 720);

}  // namespace vrp
