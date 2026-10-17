#pragma once

/**
 * Reusable gamepad / HUD control icons (Qt Gui only).
 *
 * Paint into any QPainter, or call render_* to get a standalone QImage.
 * No dependency on HudCanvas, OpenXR, or the player app.
 */

#include <QFont>
#include <QImage>
#include <QPainter>
#include <QRect>
#include <QString>

#include <cstdint>

namespace vrp::icons {

inline constexpr int kDefaultFaceSize = 32;
inline constexpr int kDefaultTriggerW = 44;
inline constexpr int kDefaultTriggerH = 30;

enum class FaceLayout : uint8_t { Xbox = 0, Sony = 1 };

/** A/B confirm-cancel; X/Y west-north. Sony maps to ×/○/□/△. */
enum class FaceButton : uint8_t { A = 0, B = 1, X = 2, Y = 3 };

enum class StickMotion : uint8_t { Plain = 0, AxisLR = 1, AxisUD = 2, Click = 3 };

enum class DpadDir : uint8_t { Up = 0, Down = 1, Left = 2, Right = 3 };

/** Prefer DejaVu/Liberation/Noto Sans Bold for punched glyphs. */
QFont gothic_bold(int point_size);

/** Font fitted to an L1/R1-style chip (reference 44×30). Stick letters use this too. */
QFont trigger_label_font(const QString& label, qreal box_w = kDefaultTriggerW,
                         qreal box_h = kDefaultTriggerH);

void paint_face(QPainter& p, const QRect& r, FaceButton button, FaceLayout layout);
void paint_trigger(QPainter& p, const QRect& r, const QString& label);
void paint_stick(QPainter& p, const QRect& r, const QString& hand, StickMotion motion);
void paint_dpad(QPainter& p, const QRect& r, DpadDir dir);
/** Outlined MENU / OPTION chip (or any short label). */
void paint_menu_chip(QPainter& p, const QRect& r, const QString& text);

int menu_chip_width(const QString& text);
int menu_chip_height(const QString& text);

QImage render_face(int size, FaceButton button, FaceLayout layout);
QImage render_trigger(int w, int h, const QString& label);
QImage render_stick(int size, const QString& hand, StickMotion motion);
QImage render_dpad(int size, DpadDir dir);
QImage render_menu_chip(int w, int h, const QString& text);

}  // namespace vrp::icons
