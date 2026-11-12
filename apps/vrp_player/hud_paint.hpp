#pragma once

#include "hud_cache.hpp"
#include "ui/vr_menu.hpp"

#include <QImage>
#include <QVector>

namespace vrp {

/**
 * Build the HMD menu from cached sprites (call on the Qt GUI thread).
 * New text and icons are rasterized once; the returned quads only place them.
 * While network_loading, chrome/list quads are frozen and only the spinner
 * frame is retargeted (lightening 9).
 */
HudPaintResult paint_vr_menu(const VrMenu::Snapshot& snap, int width = 1280, int height = 720,
                             bool sony_face = false, bool menu_option = false);

/** Semi-transparent file-dialog thumbnail card. Empty when there is no preview. */
QImage paint_file_thumb(const VrMenu::Snapshot& snap);

/** Centered control-guide card (startup). Close with B/×. Surrounding pixels stay transparent. */
HudPaintResult paint_controls_help(int width = 1280, int height = 720, bool sony_face = false,
                                   bool menu_option = false);

/** Short centered toast (Flat scale preset, etc.). */
HudPaintResult paint_hud_toast(const QString& text, int width = 1280, int height = 720);

/**
 * Shared 3-dot busy spinner (Live Flat + DLNA / network menu loading).
 * 8 frames cover 120° (next dot lands on previous); loop = smooth spin.
 */
constexpr int kLiveBusySpinnerW = 256;
constexpr int kLiveBusySpinnerH = 256;
constexpr int kLiveBusyFrames = 8;
constexpr int kLiveBusyPanelW = 1920;
constexpr int kLiveBusyPanelH = 1080;
constexpr float kLiveBusyPeriod = 2.09439510239f;  // 2π/3

int live_busy_spinner_frame_index(float phase_rad);
/** opaque_bg: Flat panel blit. transparent: HMD menu overlay. */
QImage paint_live_busy_spinner(float phase, bool opaque_bg = true);
/** Title under spinner. percent is ignored (no % readout). */
QImage paint_live_busy_panel(int width, int height, const QString& status, int percent = -1);
QImage paint_live_busy(int width, int height, float phase, const QString& status,
                       int percent = -1);
/** Eight opaque 256² frames for Flat VRAM (build once). */
QVector<QImage> paint_live_busy_spinner_atlas();
/** Transparent frame for HUD paint_cached (build once per index). */
const QImage& live_busy_spinner_hud_frame(int index);

}  // namespace vrp
