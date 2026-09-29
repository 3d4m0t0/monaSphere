#include "hud_paint.hpp"
#include "i18n.hpp"
#include "options.hpp"

#include <QGuiApplication>
#include <QPainter>
#include <QFont>
#include <QFontInfo>
#include <QFontMetrics>
#include <QPainterPath>
#include <QImage>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>

namespace vrp {
namespace {

uint64_t hud_key(std::initializer_list<uint64_t> parts, const QString& extra = {}) {
  uint64_t h = 14695981039346656037ull;
  for (uint64_t part : parts) {
    h ^= part;
    h *= 1099511628211ull;
  }
  for (QChar ch : extra) {
    h ^= ch.unicode();
    h *= 1099511628211ull;
  }
  return h;
}

bool sony_face_buttons = false;

QFont iconGothicBold(int point_size) {
  const char* families[] = {"DejaVu Sans", "Liberation Sans", "Nimbus Sans", "Noto Sans"};
  for (const char* name : families) {
    QFont f(QString::fromUtf8(name), point_size, QFont::Bold);
    f.setStyleName(QStringLiteral("Bold"));
    if (QFontInfo(f).family().compare(QLatin1String(name), Qt::CaseInsensitive) == 0) return f;
  }
  QFont f(QStringLiteral("Sans Serif"), point_size, QFont::Bold);
  f.setBold(true);
  return f;
}

QFont deUiFont(int point_size, QFont::Weight weight) {
  QFont f = QGuiApplication::font();
  f.setStyleName(QString());
  f.setWeight(weight);
  f.setBold(weight >= QFont::DemiBold);
  f.setPointSize(point_size);
  return f;
}

QString fmt_time(double sec) {
  if (sec < 0) sec = 0;
  const int s = static_cast<int>(sec);
  const int h = s / 3600;
  const int m = (s % 3600) / 60;
  const int r = s % 60;
  if (h > 0) return QString("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(r, 2, 10, QChar('0'));
  return QString("%1:%2").arg(m).arg(r, 2, 10, QChar('0'));
}

void draw_row(HudCanvas& c, const QRect& r, const QString& text, const QFont& font, bool selected,
              bool dim = false) {
  if (selected) c.fill(r.adjusted(8, 2, -8, -2), QColor(40, 120, 200, 220));
  c.text(r.adjusted(24, 0, -16, 0), text, font, dim ? QColor(160, 170, 180) : QColor(240, 245, 250),
         Qt::AlignVCenter | Qt::AlignLeft);
}

void draw_folder_icon(QPainter& p, const QRect& cell, bool selected) {
  p.setRenderHint(QPainter::Antialiasing, true);
  const QColor ink = selected ? QColor(245, 248, 252) : QColor(180, 198, 220);
  const int m = std::max(2, cell.width() / 8);
  const QRectF body = QRectF(cell).adjusted(m, m + 2, -m, -m);
  const qreal tab_h = std::max(3.0, body.height() / 4.0);
  const qreal tab_w = body.width() * 2.0 / 5.0;
  p.setPen(Qt::NoPen);
  p.setBrush(ink);
  p.drawRoundedRect(QRectF(body.left(), body.top(), tab_w, tab_h + 1), 1.5, 1.5);
  p.drawRoundedRect(QRectF(body.left(), body.top() + tab_h - 1, body.width(), body.height() - tab_h + 1),
                    2.0, 2.0);
}

void draw_row_with_folder(HudCanvas& c, const QRect& r, const QString& text, const QFont& font, bool selected) {
  if (selected) c.fill(r.adjusted(8, 2, -8, -2), QColor(40, 120, 200, 220));
  const int icon = std::min(22, r.height() - 8);
  const QRect icon_r(r.left() + 14, r.center().y() - icon / 2, icon, icon);
  c.paint_cached(hud_key({1, static_cast<uint64_t>(icon), selected ? 1ull : 0ull}), icon_r, icon, icon,
                 [&](QPainter& p) { draw_folder_icon(p, QRect(0, 0, icon, icon), selected); });
  c.text(r.adjusted(14 + icon + 8, 0, -16, 0), text, font, QColor(240, 245, 250),
         Qt::AlignVCenter | Qt::AlignLeft);
}

void draw_flat_square(QPainter& p, const QRect& r, bool selected, bool editing = false) {
  p.setRenderHint(QPainter::Antialiasing, true);
  const QRectF rf = QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5);
  p.setPen(Qt::NoPen);
  p.setBrush(selected || editing ? QColor(235, 242, 250) : QColor(36, 42, 54));
  p.drawRoundedRect(rf, 4.0, 4.0);
  if (selected || editing) {
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(editing ? QColor(220, 140, 40) : QColor(40, 120, 200), 2.0));
    p.drawRoundedRect(rf.adjusted(1, 1, -1, -1), 3.0, 3.0);
  }
}

enum class TransportIcon { Prev, Rewind, Play, Pause, Stop, Forward, Next };

/** Paint a single AA icon into a square image (device-independent). */
QImage make_transport_icon(TransportIcon kind, int size, bool selected) {
  QImage img(size, size, QImage::Format_ARGB32_Premultiplied);
  img.fill(Qt::transparent);
  QPainter ip(&img);
  ip.setRenderHint(QPainter::Antialiasing, true);
  ip.setRenderHint(QPainter::SmoothPixmapTransform, true);
  const qreal m = size * 0.22;
  const QRectF icon(m, m, size - 2 * m, size - 2 * m);
  const QColor ink = selected ? QColor(20, 28, 40) : QColor(245, 248, 252);
  ip.setPen(QPen(ink, 1.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  ip.setBrush(ink);

  auto chevron = [&](qreal cx, bool left) {
    QPainterPath path;
    const qreal w = icon.width() / 3.0;
    const qreal mid = icon.center().y();
    if (left) {
      path.moveTo(cx + w / 2, icon.top());
      path.lineTo(cx - w / 2, mid);
      path.lineTo(cx + w / 2, icon.bottom());
    } else {
      path.moveTo(cx - w / 2, icon.top());
      path.lineTo(cx + w / 2, mid);
      path.lineTo(cx - w / 2, icon.bottom());
    }
    path.closeSubpath();
    ip.drawPath(path);
  };

  auto bar = [&](qreal x, qreal w) {
    ip.drawRoundedRect(QRectF(x, icon.top(), w, icon.height()), size * 0.04, size * 0.04);
  };

  switch (kind) {
    case TransportIcon::Prev: {
      const qreal bw = std::max(2.0, icon.width() / 8.0);
      bar(icon.left(), bw);
      const qreal cx = icon.center().x() + bw / 2;
      chevron(cx - icon.width() / 6, true);
      chevron(cx + icon.width() / 6, true);
      break;
    }
    case TransportIcon::Next: {
      const qreal bw = std::max(2.0, icon.width() / 8.0);
      bar(icon.right() - bw, bw);
      const qreal cx = icon.center().x() - bw / 2;
      chevron(cx - icon.width() / 6, false);
      chevron(cx + icon.width() / 6, false);
      break;
    }
    case TransportIcon::Rewind: {
      chevron(icon.center().x(), true);
      break;
    }
    case TransportIcon::Forward: {
      chevron(icon.center().x(), false);
      break;
    }
    case TransportIcon::Play: {
      QPainterPath path;
      path.moveTo(icon.left() + icon.width() / 6, icon.top());
      path.lineTo(icon.right() - icon.width() / 8, icon.center().y());
      path.lineTo(icon.left() + icon.width() / 6, icon.bottom());
      path.closeSubpath();
      ip.drawPath(path);
      break;
    }
    case TransportIcon::Pause: {
      const qreal bw = std::max(3.0, icon.width() / 5.0);
      const qreal gap = std::max(3.0, icon.width() / 6.0);
      const qreal left = icon.center().x() - gap / 2 - bw;
      bar(left, bw);
      bar(left + bw + gap, bw);
      break;
    }
    case TransportIcon::Stop: {
      const qreal s = icon.width() * 0.55;
      ip.drawRoundedRect(QRectF(icon.center().x() - s / 2, icon.center().y() - s / 2, s, s),
                         size * 0.05, size * 0.05);
      break;
    }
  }
  ip.end();
  return img;
}

void paint_transport_icon(HudCanvas& c, const QRect& r, TransportIcon kind, bool selected) {
  const int size = std::max(2, r.width() * 2);
  c.paint_cached(hud_key({2, static_cast<uint64_t>(kind), static_cast<uint64_t>(size), selected ? 1ull : 0ull}),
                 r, size, size, [&](QPainter& p) {
                   const QImage hi = make_transport_icon(kind, size, selected);
                   p.drawImage(QRect(0, 0, size, size), hi);
                 });
}

enum class DpadDir { Up, Down };

void paint_knockout_qp(QPainter& p, const QRect& r, bool round, const QString& letter, QPoint shift,
                       qreal corner) {
  constexpr int kScale = 2;
  QImage plate(r.width() * kScale, r.height() * kScale, QImage::Format_ARGB32_Premultiplied);
  plate.fill(Qt::transparent);
  QPainter ip(&plate);
  ip.setRenderHint(QPainter::Antialiasing, true);
  ip.setRenderHint(QPainter::TextAntialiasing, true);
  ip.scale(kScale, kScale);
  ip.setPen(Qt::NoPen);
  ip.setBrush(Qt::white);
  const QRectF box(0, 0, r.width(), r.height());
  if (round) ip.drawEllipse(box.adjusted(0.5, 0.5, -0.5, -0.5));
  else ip.drawRoundedRect(box.adjusted(0.5, 0.5, -0.5, -0.5), corner, corner);
  ip.setCompositionMode(QPainter::CompositionMode_DestinationOut);
  ip.setFont(iconGothicBold(15));
  ip.setPen(Qt::white);
  ip.drawText(box.translated(shift).toAlignedRect(), Qt::AlignCenter, letter);
  ip.end();
  p.save();
  p.setRenderHint(QPainter::SmoothPixmapTransform, true);
  p.drawImage(r, plate);
  p.restore();
}

void paint_round_qp(QPainter& p, const QRect& r, const QString& letter) {
  paint_knockout_qp(p, r, true, letter, QPoint(0, 0), 0);
}

void paint_sony_qp(QPainter& p, const QRect& r, bool cross) {
  constexpr int kScale = 2;
  QImage plate(r.width() * kScale, r.height() * kScale, QImage::Format_ARGB32_Premultiplied);
  plate.fill(Qt::transparent);
  QPainter ip(&plate);
  ip.setRenderHint(QPainter::Antialiasing, true);
  ip.scale(kScale, kScale);
  ip.setPen(Qt::NoPen);
  ip.setBrush(Qt::white);
  const QRectF box(0, 0, r.width(), r.height());
  ip.drawEllipse(box.adjusted(0.5, 0.5, -0.5, -0.5));

  const qreal d = std::min(box.width(), box.height());
  const QPointF c = box.center();
  const qreal thick = d * 0.14;
  ip.setCompositionMode(QPainter::CompositionMode_DestinationOut);
  ip.setBrush(Qt::white);
  if (cross) {
    const qreal arm = d * 0.36;
    ip.save();
    ip.translate(c);
    ip.rotate(45);
    ip.drawRoundedRect(QRectF(-arm, -thick / 2, arm * 2, thick), thick / 2, thick / 2);
    ip.rotate(90);
    ip.drawRoundedRect(QRectF(-arm, -thick / 2, arm * 2, thick), thick / 2, thick / 2);
    ip.restore();
  } else {
    const qreal outer = d * 0.40;
    QPainterPath ring;
    ring.setFillRule(Qt::OddEvenFill);
    ring.addEllipse(c, outer, outer);
    ring.addEllipse(c, outer - thick, outer - thick);
    ip.drawPath(ring);
  }
  ip.end();
  p.save();
  p.setRenderHint(QPainter::SmoothPixmapTransform, true);
  p.drawImage(r, plate);
  p.restore();
}

void paint_face_qp(QPainter& p, const QRect& r, bool confirm) {
  if (!sony_face_buttons) {
    paint_round_qp(p, r, confirm ? QStringLiteral("A") : QStringLiteral("B"));
    return;
  }
  paint_sony_qp(p, r, confirm);
}

void paint_square_qp(QPainter& p, const QRect& r, const QString& letter) {
  const qreal corner = std::min(r.width(), r.height()) * 0.35;
  paint_knockout_qp(p, r, false, letter, QPoint(0, 0), corner);
}

void paint_face_button(HudCanvas& c, const QRect& r, bool confirm) {
  const QString mark = confirm ? (sony_face_buttons ? QStringLiteral("cross") : QStringLiteral("A"))
                               : (sony_face_buttons ? QStringLiteral("circle") : QStringLiteral("B"));
  c.paint_cached(hud_key({4, static_cast<uint64_t>(r.width()), static_cast<uint64_t>(r.height()),
                          sony_face_buttons ? 1ull : 0ull, confirm ? 1ull : 0ull},
                         mark),
                 r, r.width(), r.height(),
                 [&](QPainter& p) { paint_face_qp(p, QRect(0, 0, r.width(), r.height()), confirm); });
}

void paint_square_button(HudCanvas& c, const QRect& r, const QString& letter) {
  c.paint_cached(hud_key({5, static_cast<uint64_t>(r.width()), static_cast<uint64_t>(r.height())}, letter), r,
                 r.width(), r.height(), [&](QPainter& p) {
                   paint_square_qp(p, QRect(0, 0, r.width(), r.height()), letter);
                 });
}

void paint_dpad_qp(QPainter& p, const QRect& box, DpadDir dir) {
  p.save();
  p.setRenderHint(QPainter::Antialiasing, true);
  p.setPen(Qt::NoPen);
  const int s = std::min(box.width(), box.height());
  const QRect r(box.center().x() - s / 2, box.center().y() - s / 2, s, s);
  const int t = std::max(8, s / 3);
  const int mid_x = r.center().x() - t / 2;
  const int mid_y = r.center().y() - t / 2;
  const QColor idle(78, 86, 102);
  const QColor hot(214, 42, 48);
  const qreal rad = t * 0.25;
  p.setBrush(idle);
  p.drawRoundedRect(QRectF(mid_x, r.y(), t, s), rad, rad);
  p.drawRoundedRect(QRectF(r.x(), mid_y, s, t), rad, rad);
  const QRectF arm = dir == DpadDir::Up
                         ? QRectF(mid_x, r.y(), t, mid_y - r.y())
                         : QRectF(mid_x, mid_y + t, t, r.bottom() - (mid_y + t) + 1);
  const qreal rr = std::min(rad, std::min(arm.width(), arm.height()) * 0.5);
  QPainterPath tip;
  if (dir == DpadDir::Up) {
    tip.moveTo(arm.left() + rr, arm.top());
    tip.lineTo(arm.right() - rr, arm.top());
    tip.arcTo(QRectF(arm.right() - 2 * rr, arm.top(), 2 * rr, 2 * rr), 90, -90);
    tip.lineTo(arm.right(), arm.bottom());
    tip.lineTo(arm.left(), arm.bottom());
    tip.lineTo(arm.left(), arm.top() + rr);
    tip.arcTo(QRectF(arm.left(), arm.top(), 2 * rr, 2 * rr), 180, -90);
  } else {
    tip.moveTo(arm.left(), arm.top());
    tip.lineTo(arm.right(), arm.top());
    tip.lineTo(arm.right(), arm.bottom() - rr);
    tip.arcTo(QRectF(arm.right() - 2 * rr, arm.bottom() - 2 * rr, 2 * rr, 2 * rr), 0, -90);
    tip.lineTo(arm.left() + rr, arm.bottom());
    tip.arcTo(QRectF(arm.left(), arm.bottom() - 2 * rr, 2 * rr, 2 * rr), 270, -90);
    tip.lineTo(arm.left(), arm.top());
  }
  tip.closeSubpath();
  p.setBrush(hot);
  p.drawPath(tip);
  p.restore();
}

void paint_dpad(HudCanvas& c, const QRect& box, DpadDir dir) {
  c.paint_cached(hud_key({6, static_cast<uint64_t>(box.width()), static_cast<uint64_t>(box.height()),
                         static_cast<uint64_t>(dir)}),
                 box, box.width(), box.height(), [&](QPainter& p) {
                   paint_dpad_qp(p, QRect(0, 0, box.width(), box.height()), dir);
                 });
}

enum class HelpIcon { None, A, B, LR, Speed };

int help_icon_width(HelpIcon kind) {
  constexpr int kFace = 32;
  constexpr int kBarW = 54;
  constexpr int kBarGap = 6;
  constexpr int kDpad = 36;
  constexpr int kTight = 2;
  if (kind == HelpIcon::A || kind == HelpIcon::B) return kFace;
  if (kind == HelpIcon::LR) return kBarW * 2 + kBarGap;
  if (kind == HelpIcon::Speed) return (kDpad + kTight + kFace) * 2 + 16;
  return 0;
}

int paint_help_icons(HudCanvas& c, int x, int y, int h, HelpIcon kind, const QFont& font) {
  constexpr int kFace = 32;
  const int face_y = y + (h - kFace) / 2;
  if (kind == HelpIcon::A || kind == HelpIcon::B) {
    paint_face_button(c, QRect(x, face_y, kFace, kFace), kind == HelpIcon::A);
    return kFace;
  }
  if (kind == HelpIcon::LR) {
    constexpr int kBarW = 54;
    constexpr int kBarH = 26;
    constexpr int kBarGap = 6;
    const int bar_y = y + (h - kBarH) / 2;
    paint_square_button(c, QRect(x, bar_y, kBarW, kBarH), QStringLiteral("L"));
    paint_square_button(c, QRect(x + kBarW + kBarGap, bar_y, kBarW, kBarH), QStringLiteral("R"));
    return kBarW * 2 + kBarGap;
  }
  if (kind == HelpIcon::Speed) {
    constexpr int kDpad = 36;
    constexpr int kTight = 2;
    const int dy = y + (h - kDpad) / 2;
    int cx = x;
    paint_dpad(c, QRect(cx, dy, kDpad, kDpad), DpadDir::Up);
    cx += kDpad + kTight;
    paint_face_button(c, QRect(cx, face_y, kFace, kFace), true);
    cx += kFace;
    QFont dot = font;
    dot.setPointSize(16);
    c.text(QRect(cx, y, 16, h), QStringLiteral("·"), dot, QColor(180, 190, 205), Qt::AlignCenter);
    cx += 16;
    paint_dpad(c, QRect(cx, dy, kDpad, kDpad), DpadDir::Down);
    cx += kDpad + kTight;
    paint_face_button(c, QRect(cx, face_y, kFace, kFace), true);
    return help_icon_width(kind);
  }
  return 0;
}

constexpr int kFootFace = 26;

int paint_footer_label(HudCanvas& c, int x, int y, int h, const QString& text, const QFont& font) {
  const int tw = QFontMetrics(font).horizontalAdvance(text);
  c.text(QRect(x, y, tw + 2, h), text, font, QColor(230, 236, 245), Qt::AlignVCenter | Qt::AlignLeft);
  return x + tw;
}

int paint_ab_footer(HudCanvas& c, int right, int y, int h, const QString& a_label, const QString& b_label,
                    const QFont& font) {
  const QFontMetrics fm(font);
  const int face_y = y + (h - kFootFace) / 2;
  const int bw = fm.horizontalAdvance(b_label);
  const int aw = fm.horizontalAdvance(a_label);
  int x = right - bw;
  paint_footer_label(c, x, y, h, b_label, font);
  x -= 6 + kFootFace;
  paint_face_button(c, QRect(x, face_y, kFootFace, kFootFace), false);
  if (a_label == b_label) {
    x -= 4 + kFootFace;
  } else {
    x -= 14 + aw;
    paint_footer_label(c, x, y, h, a_label, font);
    x -= 6 + kFootFace;
  }
  paint_face_button(c, QRect(x, face_y, kFootFace, kFootFace), true);
  return x;
}

int paint_lr_cluster(HudCanvas& c, int x, int y, int h, const QString& label, const QFont& font) {
  constexpr int kBarW = 40;
  constexpr int kBarH = 22;
  constexpr int kGap = 4;
  const int bar_y = y + (h - kBarH) / 2;
  paint_square_button(c, QRect(x, bar_y, kBarW, kBarH), QStringLiteral("L"));
  paint_square_button(c, QRect(x + kBarW + kGap, bar_y, kBarW, kBarH), QStringLiteral("R"));
  return paint_footer_label(c, x + kBarW * 2 + kGap + 4, y, h, label, font) + 14;
}

int paint_stick_a_cluster(HudCanvas& c, int x, int y, int h, DpadDir dir, const QString& label,
                          const QFont& font) {
  constexpr int kDpad = 26;
  const int face_y = y + (h - kFootFace) / 2;
  const int dy = y + (h - kDpad) / 2;
  paint_dpad(c, QRect(x, dy, kDpad, kDpad), dir);
  paint_face_button(c, QRect(x + kDpad + 2, face_y, kFootFace, kFootFace), true);
  return paint_footer_label(c, x + kDpad + 2 + kFootFace + 4, y, h, label, font) + 14;
}

int paint_stick_cluster(HudCanvas& c, int x, int y, int h, const QString& label, const QFont& font) {
  constexpr int kDpad = 26;
  constexpr int kGap = 2;
  const int dy = y + (h - kDpad) / 2;
  paint_dpad(c, QRect(x, dy, kDpad, kDpad), DpadDir::Up);
  paint_dpad(c, QRect(x + kDpad + kGap, dy, kDpad, kDpad), DpadDir::Down);
  return paint_footer_label(c, x + kDpad * 2 + kGap + 4, y, h, label, font) + 14;
}

void draw_chip(HudCanvas& c, const QRect& r, const QString& text, const QFont& font, bool selected,
               bool editing = false) {
  c.paint_cached(hud_key({7, static_cast<uint64_t>(r.width()), static_cast<uint64_t>(r.height()),
                         selected ? 1ull : 0ull, editing ? 1ull : 0ull}),
                 r, r.width(), r.height(), [&](QPainter& p) {
                   draw_flat_square(p, QRect(0, 0, r.width(), r.height()), selected, editing);
                 });
  c.text(r, text, font, selected || editing ? QColor(20, 28, 40) : QColor(245, 248, 252), Qt::AlignCenter);
}

void draw_mode_chip(HudCanvas& c, const QRect& r, const QString& top, const QString& bottom, const QFont& font,
                    bool selected, bool editing = false) {
  c.paint_cached(hud_key({8, static_cast<uint64_t>(r.width()), static_cast<uint64_t>(r.height()),
                         selected ? 1ull : 0ull, editing ? 1ull : 0ull}),
                 r, r.width(), r.height(), [&](QPainter& p) {
                   draw_flat_square(p, QRect(0, 0, r.width(), r.height()), selected, editing);
                 });
  const QColor ink = selected || editing ? QColor(20, 28, 40) : QColor(245, 248, 252);
  const QRect inner = r.adjusted(2, 1, -2, -1);
  const int half = inner.height() / 2;
  c.text(QRect(inner.x(), inner.y(), inner.width(), half), top, font, ink, Qt::AlignCenter);
  c.text(QRect(inner.x(), inner.y() + half, inner.width(), inner.height() - half), bottom, font, ink,
         Qt::AlignCenter);
}

QString fsr_label(FsrMode m) {
  switch (m) {
    case FsrMode::Off: return QStringLiteral("FSR ") + VRP_TR("Off");
    case FsrMode::UltraQuality: return QStringLiteral("FSR UQ");
    case FsrMode::Quality: return QStringLiteral("FSR Q");
    case FsrMode::Performance: return QStringLiteral("FSR P");
  }
  return QStringLiteral("FSR");
}

QString proj_label(ProjectionMode m) {
  switch (m) {
    case ProjectionMode::Flat: return VRP_TR("Flat");
    case ProjectionMode::Deg180: return QStringLiteral("180°");
    case ProjectionMode::Deg360: return QStringLiteral("360°");
  }
  return QStringLiteral("?");
}

QString stereo_label(StereoLayout s) {
  switch (s) {
    case StereoLayout::Mono: return VRP_TR("mono");
    case StereoLayout::Sbs: return QStringLiteral("SBS");
    case StereoLayout::OverUnder: return QStringLiteral("OU");
  }
  return QStringLiteral("?");
}

bool flat_pack_choices(const VrMenu::Snapshot& snap) {
  return snap.projection == ProjectionMode::Flat && snap.stereo != StereoLayout::Mono;
}

QString format_choice_label(FormatChoice choice) {
  switch (choice) {
    case FormatChoice::Flat: return proj_label(ProjectionMode::Flat);
    case FormatChoice::Deg180: return QStringLiteral("180°");
    case FormatChoice::Deg360: return QStringLiteral("360°");
    case FormatChoice::Mono: return stereo_label(StereoLayout::Mono);
    case FormatChoice::Sbs: return stereo_label(StereoLayout::Sbs);
    case FormatChoice::Ou: return stereo_label(StereoLayout::OverUnder);
    case FormatChoice::Full: return VRP_TR("Full");
    case FormatChoice::Half: return VRP_TR("Half");
  }
  return QStringLiteral("?");
}

bool format_choice_active(const VrMenu::Snapshot& snap, FormatChoice choice) {
  switch (choice) {
    case FormatChoice::Flat: return snap.projection == ProjectionMode::Flat;
    case FormatChoice::Deg180: return snap.projection == ProjectionMode::Deg180;
    case FormatChoice::Deg360: return snap.projection == ProjectionMode::Deg360;
    case FormatChoice::Mono: return snap.stereo == StereoLayout::Mono;
    case FormatChoice::Sbs: return snap.stereo == StereoLayout::Sbs;
    case FormatChoice::Ou: return snap.stereo == StereoLayout::OverUnder;
    case FormatChoice::Full: return snap.stereo_full;
    case FormatChoice::Half: return !snap.stereo_full;
  }
  return false;
}

bool format_row_mark(const VrMenu::Snapshot& snap, FormatChoice choice) {
  if (!format_choice_active(snap, choice)) return false;
  return choice == FormatChoice::Deg180 || choice == FormatChoice::Deg360 || choice == FormatChoice::Mono ||
         choice == FormatChoice::Sbs || choice == FormatChoice::Ou || choice == FormatChoice::Full ||
         choice == FormatChoice::Half;
}

void paint_format_row(HudCanvas& c, const QRect& row, const QString& label, const QFont& font, bool selected,
                      bool checked) {
  if (selected) c.rounded(row, QColor(40, 120, 200, 220), 4);
  const QColor ink = selected ? QColor(255, 255, 255) : QColor(210, 220, 235);
  if (checked) c.text(QRect(row.x() + 2, row.y(), 18, row.height()), QStringLiteral("✓"), font, ink, Qt::AlignCenter);
  c.text(row.adjusted(22, 0, -6, 0), label, font, ink, Qt::AlignVCenter | Qt::AlignLeft);
}

QString stereo_chip_label(const VrMenu::Snapshot& snap) {
  QString s = stereo_label(snap.stereo);
  if (flat_pack_choices(snap)) s += QLatin1Char(' ') + (snap.stereo_full ? VRP_TR("Full") : VRP_TR("Half"));
  return s;
}

void paint_playback_controls(HudCanvas& c, const VrMenu::Snapshot& snap, int width, int height,
                             const QFont& body_font, const QFont& hint_font) {
  const bool has_info = !snap.info_video_codec.empty() || !snap.info_res.empty() ||
                        !snap.info_audio_codec.empty();
  const int info_h = has_info ? 72 : 0;
  const int foot_h = 40;
  const int btn = 48;
  const int buttons_off = 32;
  const int scrub_off = buttons_off + btn + 14;
  const int above_foot = has_info ? (scrub_off + 18 + (info_h - 8)) : (scrub_off + 16);
  const int panel_h = above_foot + foot_h;
  const int top = height - panel_h;

  const int gap = 4;
  const int chip_h = 48;
  constexpr int kBtns = 6;
  const int vol_w = 100;
  const int fmt_w = 156;
  const int fsr_w = 88;
  const int hz_w = 80;
  const int group_gap = 6;
  const int row_w = btn * kBtns + gap * (kBtns - 1) + group_gap + vol_w + gap + fmt_w + gap +
                    fsr_w + gap + hz_w;
  const int pad = 12;
  const int panel_w = row_w + pad * 2;
  const int panel_x = std::max(0, (width - panel_w) / 2);
  c.rounded(QRect(panel_x, top, panel_w, panel_h), QColor(16, 20, 30, 235), 8);

  const QString fname = snap.media_name.empty() ? VRP_TR(snap.title.c_str())
                                                : QString::fromStdString(snap.media_name);
  c.scrolling_text(QRect(panel_x + pad, top + 4, panel_w - pad * 2 - 80, 24), fname, body_font,
                   QColor(230, 240, 255));
  {
    const QString speed = QStringLiteral("×%1").arg(snap.rate, 0, 'g', 3);
    c.text(QRect(panel_x + panel_w - pad - 72, top + 4, 72, 24), speed, hint_font, QColor(120, 200, 255),
           Qt::AlignVCenter | Qt::AlignRight);
  }

  int x = panel_x + pad;
  const int y = top + 32;

  const int sel = std::clamp(snap.transport_cursor, 0, 9);
  const bool editing = snap.controls_edit != VrMenu::ControlsEdit::None;

  const TransportIcon icons[kBtns] = {
      TransportIcon::Prev, TransportIcon::Rewind,
      snap.playing ? TransportIcon::Pause : TransportIcon::Play, TransportIcon::Stop,
      TransportIcon::Forward, TransportIcon::Next};
  for (int i = 0; i < kBtns; ++i) {
    const bool on = (!editing && sel == i);
    c.paint_cached(hud_key({9, static_cast<uint64_t>(btn), on ? 1ull : 0ull}), QRect(x, y, btn, btn), btn, btn,
                   [&](QPainter& painter) { draw_flat_square(painter, QRect(0, 0, btn, btn), on); });
    paint_transport_icon(c, QRect(x, y, btn, btn), icons[i], on);
    x += btn + gap;
  }
  x += group_gap - gap;

  const int vol_pct = static_cast<int>(std::lround(snap.volume * 100.f));
  const QRect vol_r(x, y, vol_w, chip_h);
  draw_chip(c, vol_r, VRP_TR("Vol%1%").arg(vol_pct), hint_font, sel == 6,
            snap.controls_edit == VrMenu::ControlsEdit::Volume);
  x += vol_w + gap;

  const QRect fmt_r(x, y, fmt_w, chip_h);
  draw_mode_chip(c, fmt_r, proj_label(snap.projection), stereo_chip_label(snap), hint_font, sel == 7,
                 snap.controls_edit == VrMenu::ControlsEdit::FormatPick);
  x += fmt_w + gap;

  const QRect fsr_r(x, y, fsr_w, chip_h);
  draw_chip(c, fsr_r, fsr_label(snap.fsr), hint_font, sel == 8,
            snap.controls_edit == VrMenu::ControlsEdit::FsrPick);
  x += fsr_w + gap;

  const QRect hz_r(x, y, hz_w, chip_h);
  QString hz_chip;
  if (snap.preferred_hz > 0) hz_chip = QStringLiteral("%1Hz").arg(snap.preferred_hz);
  else if (snap.display_hz > 0.5f) hz_chip = QStringLiteral("%1Hz").arg(snap.display_hz, 0, 'f', 0);
  else hz_chip = VRP_TR("Auto");
  draw_chip(c, hz_r, hz_chip, hint_font, sel == 9,
            snap.controls_edit == VrMenu::ControlsEdit::HzPick ||
                snap.controls_edit == VrMenu::ControlsEdit::HzConfirm);

  // Seek bar + time within panel
  const int scrub_y = y + btn + 14;
  const int time_w = 110;
  const QRect bar(panel_x + pad, scrub_y, panel_w - pad * 2 - time_w - 8, 10);
  c.rounded(bar, QColor(30, 36, 48, 200), 3);
  double t = 0;
  if (snap.duration_sec > 0) t = snap.position_sec / snap.duration_sec;
  t = std::clamp(t, 0.0, 1.0);
  const int fill_w = std::max(0, static_cast<int>(bar.width() * t));
  if (fill_w > 0) c.rounded(QRect(bar.x(), bar.y(), fill_w, bar.height()), QColor(50, 160, 255, 230), 3);
  c.text(QRect(panel_x + panel_w - pad - time_w, scrub_y - 6, time_w, 22),
         fmt_time(snap.position_sec) + QStringLiteral(" / ") + fmt_time(snap.duration_sec), hint_font,
         QColor(200, 210, 220), Qt::AlignRight | Qt::AlignVCenter);

  if (has_info) {
    const int info_top = scrub_y + 18;
    const QRect info_box(panel_x + pad, info_top, panel_w - pad * 2, info_h - 8);
    c.rounded(info_box, QColor(255, 255, 255, 210), 4);
    QFont info_font = hint_font;
    info_font.setPointSize(11);
    const QFontMetrics fm(info_font);
    const int text_w = info_box.width() - 16;
    int iy = info_box.y() + 5;
    auto line = [&](const QString& text) {
      c.text(QRect(info_box.x() + 8, iy, text_w, 18), fm.elidedText(text, Qt::ElideRight, text_w), info_font,
             QColor(20, 20, 24), Qt::AlignLeft | Qt::AlignVCenter);
      iy += 18;
    };
    auto v = [](const std::string& s) {
      return s.empty() ? QStringLiteral("-") : QString::fromStdString(s);
    };
    line(QStringLiteral("V  codec:%1  res:%2  fps:%3  br:%4")
             .arg(v(snap.info_video_codec), v(snap.info_res), v(snap.info_fps),
                  v(snap.info_bitrate)));
    line(QStringLiteral("A  codec:%1  rate:%2  ch:%3")
             .arg(v(snap.info_audio_codec), v(snap.info_audio_rate), v(snap.info_audio_channels)));
    line(QStringLiteral("O  hw:%1  path:%2  hz:%3")
             .arg(v(snap.info_hwaccel), v(snap.info_output_note), v(snap.info_display_hz)));
  }

  if (snap.controls_edit == VrMenu::ControlsEdit::HzConfirm) {
    const int box_w = 220;
    const int box_h = 96;
    int box_x = hz_r.center().x() - box_w / 2;
    box_x = std::clamp(box_x, panel_x + 4, panel_x + panel_w - box_w - 4);
    const int box_y = std::max(4, hz_r.top() - box_h - 6);
    const QRect box(box_x, box_y, box_w, box_h);
    c.rounded(box, QColor(24, 30, 42, 240), 6);
    const QString msg = VRP_TR("Hz change\nneeds restart");
    c.text(box.adjusted(10, 8, -10, -40), msg, hint_font, QColor(220, 230, 245),
           Qt::AlignTop | Qt::AlignHCenter | Qt::TextWordWrap);
    const QRect btn_r(box.center().x() - 48, box.bottom() - 36, 96, 28);
    c.rounded(btn_r, QColor(40, 120, 200, 230), 4);
    c.text(btn_r, VRP_TR("[Restart]"), hint_font, QColor(255, 255, 255), Qt::AlignCenter);
  } else if (snap.controls_edit == VrMenu::ControlsEdit::FsrPick ||
             snap.controls_edit == VrMenu::ControlsEdit::FormatPick ||
             snap.controls_edit == VrMenu::ControlsEdit::HzPick) {
    const bool fsr = snap.controls_edit == VrMenu::ControlsEdit::FsrPick;
    const bool hz = snap.controls_edit == VrMenu::ControlsEdit::HzPick;
    const int n = fsr ? 4 : (hz ? 3 : format_menu_count(snap.projection));
    const int row_h = 34;
    const int box_w = fsr ? 140 : (hz ? 120 : 168);
    const int box_h = 12 + n * row_h;
    const QRect anchor = fsr ? fsr_r : (hz ? hz_r : fmt_r);
    int box_x = anchor.center().x() - box_w / 2;
    box_x = std::clamp(box_x, panel_x + 4, panel_x + panel_w - box_w - 4);
    const int box_y = anchor.top() - box_h - 6;
    const QRect box(box_x, std::max(4, box_y), box_w, box_h);
    c.rounded(box, QColor(24, 30, 42, 235), 6);
    for (int i = 0; i < n; ++i) {
      QString label;
      if (fsr) {
        label = fsr_label(static_cast<FsrMode>(i));
      } else if (hz) {
        switch (i) {
          case 1: label = QStringLiteral("90Hz"); break;
          case 2: label = QStringLiteral("120Hz"); break;
          default: label = VRP_TR("Auto"); break;
        }
      } else {
        const FormatChoice choice = format_menu_at(snap.projection, i);
        const QRect row(box.x() + 6, box.y() + 6 + i * row_h, box.width() - 12, row_h - 4);
        const bool mark = format_row_mark(snap, choice);
        paint_format_row(c, row, format_choice_label(choice), hint_font, i == snap.picker_cursor, mark);
        continue;
      }
      const QRect row(box.x() + 6, box.y() + 6 + i * row_h, box.width() - 12, row_h - 4);
      const bool on = (i == snap.picker_cursor);
      if (on) c.rounded(row, QColor(40, 120, 200, 220), 4);
      c.text(row.adjusted(10, 0, -6, 0), label, hint_font, on ? QColor(255, 255, 255) : QColor(210, 220, 235),
             Qt::AlignVCenter | Qt::AlignLeft);
    }
  }

  const QRect foot(panel_x + pad, top + panel_h - foot_h, panel_w - pad * 2, foot_h);
  const char* a_src = "Execute";
  const char* b_src = "Close";
  int left_kind = 0;  // 0 seek/speed, 1 volume, 2 up/down select, 4 none
  switch (snap.controls_edit) {
    case VrMenu::ControlsEdit::Volume:
      a_src = "Decide";
      b_src = "Cancel";
      left_kind = 1;
      break;
    case VrMenu::ControlsEdit::FsrPick:
    case VrMenu::ControlsEdit::FormatPick:
    case VrMenu::ControlsEdit::HzPick:
      a_src = "Decide";
      b_src = "Cancel";
      left_kind = 2;
      break;
    case VrMenu::ControlsEdit::HzConfirm:
      a_src = "Restart";
      b_src = "Back";
      left_kind = 4;
      break;
    default:
      break;
  }
  const int actions_left =
      paint_ab_footer(c, foot.right(), foot.y(), foot.height(), VRP_TR(a_src), VRP_TR(b_src), body_font);
  const QFontMetrics fm(body_font);
  int lx = foot.x();
  const int limit = actions_left - 12;
  auto fits = [&](int w) { return lx + w <= limit; };
  if (left_kind == 0) {
    const QString seek = VRP_TR("Seek");
    const int seek_w = 40 + 4 + 40 + 4 + fm.horizontalAdvance(seek) + 14;
    if (fits(seek_w)) lx = paint_lr_cluster(c, lx, foot.y(), foot.height(), seek, body_font);
    const QString faster = VRP_TR("Faster");
    const int spd_w = 26 + 2 + kFootFace + 4 + fm.horizontalAdvance(faster) + 14;
    if (fits(spd_w)) lx = paint_stick_a_cluster(c, lx, foot.y(), foot.height(), DpadDir::Up, faster, body_font);
    const QString slower = VRP_TR("Slower");
    const int slow_w = 26 + 2 + kFootFace + 4 + fm.horizontalAdvance(slower) + 14;
    if (fits(slow_w)) paint_stick_a_cluster(c, lx, foot.y(), foot.height(), DpadDir::Down, slower, body_font);
  } else if (left_kind == 1 || left_kind == 2) {
    const QString label = left_kind == 1 ? VRP_TR("Volume") : VRP_TR("Select");
    const int w = 26 + 2 + 26 + 4 + fm.horizontalAdvance(label) + 14;
    if (fits(w)) paint_stick_cluster(c, lx, foot.y(), foot.height(), label, body_font);
  }
}

}  // namespace

QImage paint_file_thumb(const VrMenu::Snapshot& snap) {
  const bool ready = snap.visible && snap.screen == VrMenu::Screen::Browser && snap.preview_w > 0 &&
                     snap.preview_h > 0 &&
                     snap.preview_rgba.size() >= static_cast<size_t>(snap.preview_w * snap.preview_h * 4);
  if (!ready) return {};
  QImage img(snap.preview_w, snap.preview_h, QImage::Format_RGBA8888);
  img.fill(Qt::transparent);
  QPainter p(&img);
  p.setOpacity(0.78);
  QImage frame(snap.preview_rgba.data(), snap.preview_w, snap.preview_h, snap.preview_w * 4,
               QImage::Format_RGBA8888);
  p.drawImage(QRect(0, 0, snap.preview_w, snap.preview_h), frame);
  p.end();
  return img;
}

HudPaintResult paint_vr_menu(const VrMenu::Snapshot& snap, int width, int height, bool sony_face) {
  sony_face_buttons = sony_face;
  HudCanvas& canvas = shared_hud_canvas();
  canvas.begin(width, height);
  if (!snap.visible && !snap.controls_visible) return canvas.take();

  const QFont title_font = deUiFont(22, QFont::DemiBold);
  const QFont body_font = deUiFont(16, QFont::Normal);
  const QFont hint_font = deUiFont(13, QFont::Normal);

  if (snap.controls_visible) {
    paint_playback_controls(canvas, snap, width, height, body_font, hint_font);
    return canvas.take();
  }

  const int side = width / 7;
  const int content_w = width - side * 2;
  canvas.fill(QRect(side, 0, content_w, height), QColor(12, 16, 24, 230));
  canvas.fill(QRect(side, 0, content_w, 48), QColor(20, 28, 40, 255));
  canvas.text(QRect(side + 16, 4, content_w - 32, 40), VRP_TR(snap.title.c_str()), title_font,
              QColor(230, 240, 255), Qt::AlignVCenter | Qt::AlignLeft);

  const QRect foot(side + 16, height - 44, content_w - 32, 40);
  paint_ab_footer(canvas, foot.right(), foot.y(), foot.height(), VRP_TR("Decide"), VRP_TR("Back"), body_font);

  if (snap.screen == VrMenu::Screen::Browser) {
    const int list_top = 64;
    const int list_x = side + 10;
    const int row_h = 34;
    const int fit_rows = std::max(1, (height - list_top - 52) / row_h);
    const int max_rows = std::min(fit_rows, VrMenu::kVisibleRows);
    const int count = static_cast<int>(snap.entries.size());
    const bool show_scroll = count > max_rows;
    constexpr int kScrollW = 14;
    constexpr int kScrollGap = 8;
    int list_w = content_w - 24;
    if (show_scroll) list_w -= kScrollW + kScrollGap;
    const int start = snap.scroll;
    const int end = std::min(static_cast<int>(snap.entries.size()), start + max_rows);
    for (int i = start; i < end; ++i) {
      const auto& e = snap.entries[static_cast<size_t>(i)];
      const QRect row(list_x, list_top + (i - start) * row_h, list_w, row_h);
      const bool sel = (i == snap.cursor);
      const QString name = QString::fromStdString(e.name);
      if (e.is_dir && !e.is_special) draw_row_with_folder(canvas, row, name, body_font, sel);
      else draw_row(canvas, row, name, body_font, sel);
    }

    if (show_scroll) {
      const QRect track(list_x + list_w + kScrollGap, list_top + 2, kScrollW, max_rows * row_h - 4);
      canvas.paint_cached(hud_key({10, static_cast<uint64_t>(track.width()), static_cast<uint64_t>(track.height())}),
                          track, track.width(), track.height(), [&](QPainter& p) {
                            p.setPen(QPen(QColor(190, 206, 230), 2));
                            p.setBrush(QColor(36, 46, 64));
                            p.drawRoundedRect(QRectF(0.5, 0.5, track.width() - 1, track.height() - 1), 6, 6);
                          });
      const int span = std::max(1, count - max_rows);
      const float pos = std::clamp(static_cast<float>(snap.scroll) / static_cast<float>(span), 0.f, 1.f);
      const int thumb_h =
          std::min(track.height() - 4, std::max(56, track.height() * max_rows / std::max(count, 1)));
      const int travel = std::max(0, track.height() - thumb_h);
      const int thumb_y = track.y() + static_cast<int>(std::lround(travel * pos));
      const QRect thumb(track.x() + 3, thumb_y + 2, track.width() - 6, thumb_h - 4);
      canvas.paint_cached(hud_key({11, static_cast<uint64_t>(thumb.width()), static_cast<uint64_t>(thumb.height())}),
                          thumb, thumb.width(), thumb.height(), [&](QPainter& p) {
                            p.setPen(Qt::NoPen);
                            p.setBrush(QColor(232, 240, 255));
                            p.drawRoundedRect(QRectF(0, 0, thumb.width(), thumb.height()), 4, 4);
                          });
    }
  } else if (snap.screen == VrMenu::Screen::Format) {
    const int n = format_menu_count(snap.projection);
    for (int i = 0; i < n; ++i) {
      const FormatChoice choice = format_menu_at(snap.projection, i);
      const bool mark = format_row_mark(snap, choice);
      const QRect row(side + 24, 80 + i * 44, content_w - 48, 40);
      if (i == snap.format_cursor) canvas.fill(row.adjusted(8, 2, -8, -2), QColor(40, 120, 200, 220));
      if (mark) {
        canvas.text(QRect(row.x() + 8, row.y(), 22, row.height()), QStringLiteral("✓"), body_font,
                    QColor(240, 245, 250), Qt::AlignCenter);
      }
      canvas.text(row.adjusted(32, 0, -16, 0), format_choice_label(choice), body_font, QColor(240, 245, 250),
                  Qt::AlignVCenter | Qt::AlignLeft);
    }
  }
  return canvas.take();
}

HudPaintResult paint_controls_help(int width, int height, int seconds_left, bool sony_face) {
  sony_face_buttons = sony_face;
  HudCanvas& canvas = shared_hud_canvas();
  canvas.begin(width, height);

  struct Row {
    HelpIcon icon;
    const char* name;
    const char* action;
  };
  const Row rows[] = {
      {HelpIcon::None, "Start long-press", "Recenter"},
      {HelpIcon::None, "Start", "File dialog"},
      {HelpIcon::B, nullptr, "Playback controls"},
      {HelpIcon::A, nullptr, "Play or pause"},
      {HelpIcon::LR, nullptr, "Seek"},
      {HelpIcon::Speed, nullptr, "Playback speed"},
      {HelpIcon::B, "long-press", "Speed ×1"},
  };

  const QFont title_font = deUiFont(22, QFont::DemiBold);
  const QFont body_font = deUiFont(16, QFont::Normal);
  const QFont desc_font = body_font;

  constexpr int kRows = 7;
  const int row_h = 48;
  const int title_h = 44;
  const int pad = 16;
  const int col_gap = 24;
  const QFontMetrics name_fm(body_font);
  const QFontMetrics desc_fm(desc_font);
  int left_max = 0;
  int action_max = 0;
  for (const Row& row : rows) {
    const int icon_w = help_icon_width(row.icon);
    const int gap = (icon_w > 0 && row.name) ? 4 : 0;
    const int text_w = row.name ? name_fm.horizontalAdvance(VRP_TR(row.name)) : 0;
    left_max = std::max(left_max, icon_w + gap + text_w);
    action_max = std::max(action_max, desc_fm.horizontalAdvance(VRP_TR(row.action)));
  }
  const int panel_w = std::min(width - 160, pad * 2 + left_max + col_gap + action_max);
  const int foot_h = 36;
  const int panel_h = title_h + kRows * row_h + foot_h;
  const int panel_x = (width - panel_w) / 2;
  const int panel_y = std::max(12, (height - panel_h) / 2);

  canvas.rounded(QRect(panel_x, panel_y, panel_w, panel_h), QColor(16, 20, 30, 238), 12);
  canvas.text(QRect(panel_x + pad, panel_y + 6, panel_w - pad * 2, title_h - 8), VRP_TR("Controls"), title_font,
              QColor(230, 240, 255), Qt::AlignVCenter | Qt::AlignLeft);

  const int action_x = panel_x + pad + left_max + col_gap;
  const int action_w = panel_x + panel_w - pad - action_x;

  for (int i = 0; i < kRows; ++i) {
    const int y = panel_y + title_h + i * row_h;
    const QRect row(panel_x + 8, y, panel_w - 16, row_h);
    if (i % 2 == 0) canvas.rounded(row, QColor(255, 255, 255, 10), 6);
    const int icon_w = paint_help_icons(canvas, panel_x + pad, y, row_h, rows[i].icon, body_font);
    int text_x = panel_x + pad + icon_w;
    if (icon_w > 0 && rows[i].name) text_x += 4;
    if (rows[i].name) {
      const QString name = VRP_TR(rows[i].name);
      canvas.text(QRect(text_x, y, name_fm.horizontalAdvance(name) + 8, row_h), name, body_font,
                  QColor(230, 236, 245), Qt::AlignVCenter | Qt::AlignLeft);
    }
    canvas.text(QRect(action_x, y, action_w, row_h), VRP_TR(rows[i].action), desc_font, QColor(245, 248, 252),
                Qt::AlignVCenter | Qt::AlignLeft);
  }

  {
    const QString close = VRP_TR("Close");
    constexpr int kFace = 26;
    const int tw = name_fm.horizontalAdvance(close);
    const int fy = panel_y + panel_h - foot_h;
    const int fx = panel_x + panel_w - pad - tw - 8 - kFace;
    canvas.text(QRect(panel_x + pad, fy, std::max(0, fx - panel_x - pad - 8), foot_h),
                QString::number(std::max(0, seconds_left)), body_font, QColor(180, 190, 205),
                Qt::AlignVCenter | Qt::AlignLeft);
    paint_face_button(canvas, QRect(fx, fy + (foot_h - kFace) / 2, kFace, kFace), false);
    canvas.text(QRect(fx + kFace + 6, fy, tw + 4, foot_h), close, body_font, QColor(230, 236, 245),
                Qt::AlignVCenter | Qt::AlignLeft);
  }
  return canvas.take();
}

}  // namespace vrp
