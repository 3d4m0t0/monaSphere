#pragma once

#include "ui/vr_menu.hpp"

#include <QImage>

namespace vrp {

/** Paint HMD menu snapshot to RGBA8 QImage (call on Qt GUI thread). */
QImage paint_vr_menu(const VrMenu::Snapshot& snap, int width = 1280, int height = 720,
                     bool sony_face = false);

/** Centered control-guide card. Surrounding pixels stay transparent. */
QImage paint_controls_help(int width = 1280, int height = 720, int seconds_left = 0,
                           bool sony_face = false);

}  // namespace vrp
