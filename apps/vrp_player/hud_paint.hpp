#pragma once

#include "hud_cache.hpp"
#include "ui/vr_menu.hpp"

#include <QImage>

namespace vrp {

/**
 * Build the HMD menu from cached sprites (call on the Qt GUI thread).
 * New text and icons are rasterized once; the returned quads only place them.
 */
HudPaintResult paint_vr_menu(const VrMenu::Snapshot& snap, int width = 1280, int height = 720,
                             bool sony_face = false, bool menu_option = false);

/** Semi-transparent file-dialog thumbnail card. Empty when there is no preview. */
QImage paint_file_thumb(const VrMenu::Snapshot& snap);

/** Centered control-guide card. Surrounding pixels stay transparent. */
HudPaintResult paint_controls_help(int width = 1280, int height = 720, int seconds_left = 0,
                                   bool sony_face = false, bool menu_option = false);

/** Short centered toast (Flat scale preset, etc.). */
HudPaintResult paint_hud_toast(const QString& text, int width = 1280, int height = 720);

}  // namespace vrp
