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
bool menu_button_option = false;

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
  return QString("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(r, 2, 10, QChar('0'));
}

int clock_label_width(const QString& s, const QFont& font) {
  const QFontMetrics fm(font);
  int digit = 1;
  for (int i = 0; i < 10; ++i) digit = std::max(digit, fm.horizontalAdvance(QChar('0' + i)));
  int w = 0;
  for (const QChar ch : s) {
    if (ch >= QLatin1Char('0') && ch <= QLatin1Char('9')) w += digit;
    else w += std::max(1, fm.horizontalAdvance(ch));
  }
  return w;
}

void draw_row(HudCanvas& c, const QRect& r, const QString& text, const QFont& font, bool dim = false) {
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

enum class DpadDir { Up, Down, Left, Right };

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

void paint_trigger_qp(QPainter& p, const QRect& r, const QString& label) {
  constexpr int kScale = 2;
  QImage plate(std::max(1, r.width()) * kScale, std::max(1, r.height()) * kScale,
               QImage::Format_ARGB32_Premultiplied);
  plate.fill(Qt::transparent);
  QPainter ip(&plate);
  ip.setRenderHint(QPainter::Antialiasing, true);
  ip.setRenderHint(QPainter::TextAntialiasing, true);
  ip.scale(kScale, kScale);
  ip.setPen(Qt::NoPen);
  ip.setBrush(Qt::white);
  const QRectF b(0.5, 0.5, std::max(1, r.width()) - 1.0, std::max(1, r.height()) - 1.0);
  const qreal rad = std::min(b.width(), b.height()) * 0.42;
  ip.drawRoundedRect(b, rad, rad);
  QFont font = iconGothicBold(6);
  for (int pt = 14; pt >= 6; --pt) {
    const QFont try_font = iconGothicBold(pt);
    if (QFontMetricsF(try_font).tightBoundingRect(label).width() <= b.width() - 4) {
      font = try_font;
      break;
    }
  }
  const QRectF ink = QFontMetricsF(font).tightBoundingRect(label);
  const QPointF c = b.center();
  const QPointF at(c.x() - (ink.x() + ink.width() / 2.0), c.y() - (ink.y() + ink.height() / 2.0));
  ip.setCompositionMode(QPainter::CompositionMode_DestinationOut);
  ip.setFont(font);
  ip.setPen(Qt::white);
  ip.drawText(at, label);
  ip.end();
  p.save();
  p.setRenderHint(QPainter::SmoothPixmapTransform, true);
  p.drawImage(r, plate);
  p.restore();
}

void paint_trigger_button(HudCanvas& c, const QRect& r, const QString& label) {
  c.paint_cached(hud_key({26, static_cast<uint64_t>(r.width()), static_cast<uint64_t>(r.height())}, label), r,
                 r.width(), r.height(), [&](QPainter& p) {
                   paint_trigger_qp(p, QRect(0, 0, r.width(), r.height()), label);
                 });
}

void paint_dpad_qp(QPainter& p, const QRect& box, DpadDir dir) {
  p.save();
  p.setRenderHint(QPainter::Antialiasing, true);
  p.setPen(Qt::NoPen);
  const QPointF c(box.x() + box.width() / 2.0, box.y() + box.height() / 2.0);
  const qreal s = std::min(box.width(), box.height());
  const qreal t = s / 3.0;
  const qreal rad = t * 0.28;
  const QRectF vert(c.x() - t / 2, c.y() - s / 2, t, s);
  const QRectF horiz(c.x() - s / 2, c.y() - t / 2, s, t);
  p.setBrush(QColor(78, 86, 102));
  p.drawRoundedRect(vert, rad, rad);
  p.drawRoundedRect(horiz, rad, rad);

  p.translate(c);
  if (dir == DpadDir::Down) p.rotate(180);
  else if (dir == DpadDir::Left) p.rotate(-90);
  else if (dir == DpadDir::Right) p.rotate(90);
  const qreal arm_len = (s - t) / 2;
  const QRectF arm(-t / 2, -s / 2, t, arm_len);
  const qreal rr = std::min(rad, std::min(arm.width(), arm.height()) * 0.5);
  QPainterPath tip;
  tip.moveTo(arm.left() + rr, arm.top());
  tip.lineTo(arm.right() - rr, arm.top());
  tip.arcTo(QRectF(arm.right() - 2 * rr, arm.top(), 2 * rr, 2 * rr), 90, -90);
  tip.lineTo(arm.right(), arm.bottom());
  tip.lineTo(arm.left(), arm.bottom());
  tip.lineTo(arm.left(), arm.top() + rr);
  tip.arcTo(QRectF(arm.left(), arm.top(), 2 * rr, 2 * rr), 180, -90);
  tip.closeSubpath();
  p.setBrush(QColor(214, 42, 48));
  p.drawPath(tip);
  p.restore();
}

void paint_dpad(HudCanvas& c, const QRect& box, DpadDir dir) {
  c.paint_cached(hud_key({20, static_cast<uint64_t>(box.width()), static_cast<uint64_t>(box.height()),
                         static_cast<uint64_t>(dir)}),
                 box, box.width(), box.height(), [&](QPainter& p) {
                   paint_dpad_qp(p, QRect(0, 0, box.width(), box.height()), dir);
                 });
}

enum class HelpIcon { None, A, B, Stick, StickR, Speed, SpeedReset, Start };

constexpr int kIcon = 32;
constexpr int kStick = kIcon;
constexpr int kStartPad = 2;

QString menu_button_text() {
  return menu_button_option ? QStringLiteral("OPTION") : QStringLiteral("MENU");
}

constexpr int kMenuPt = 11;

int menu_button_width() {
  const int tw = QFontMetrics(iconGothicBold(kMenuPt)).horizontalAdvance(menu_button_text());
  return tw + kStartPad * 2 + 8;
}

int start_icon_height() {
  const int text_h = QFontMetrics(iconGothicBold(kMenuPt)).tightBoundingRect(menu_button_text()).height();
  return text_h + kStartPad * 2 + 4;
}

void paint_start_qp(QPainter& p, const QRect& r) {
  p.save();
  p.setRenderHint(QPainter::Antialiasing, true);
  p.setRenderHint(QPainter::TextAntialiasing, true);
  const QString text = menu_button_text();
  const QFont font = iconGothicBold(kMenuPt);
  const QRectF ink = QFontMetricsF(font).tightBoundingRect(text);
  p.setBrush(Qt::NoBrush);
  p.setPen(QPen(Qt::white, 1.2));
  p.drawRoundedRect(QRectF(r).adjusted(0.6, 0.6, -0.6, -0.6), 3, 3);
  const QPointF c(r.x() + r.width() / 2.0, r.y() + r.height() / 2.0);
  const QPointF at(c.x() - (ink.x() + ink.width() / 2.0), c.y() - (ink.y() + ink.height() / 2.0));
  p.setFont(font);
  p.setPen(Qt::white);
  p.drawText(at, text);
  p.restore();
}

void paint_stick_qp(QPainter& p, const QRect& r, const QString& hand) {
  p.save();
  p.setRenderHint(QPainter::Antialiasing, true);
  p.setRenderHint(QPainter::TextAntialiasing, true);
  const QRectF b = QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5);
  const qreal side = std::min(b.width(), b.height());
  const QRectF circle(b.center().x() - side / 2.0, b.center().y() - side / 2.0, side, side);
  p.setPen(Qt::NoPen);
  p.setBrush(QColor(140, 146, 156));
  p.drawEllipse(circle);
  const qreal inner = side * 0.62;
  QFont font = iconGothicBold(8);
  for (int pt = 18; pt >= 6; --pt) {
    const QFont try_font = iconGothicBold(pt);
    const QRectF ink = QFontMetricsF(try_font).tightBoundingRect(hand);
    if (ink.width() <= inner && ink.height() <= inner) {
      font = try_font;
      break;
    }
  }
  const QRectF ink = QFontMetricsF(font).tightBoundingRect(hand);
  const QPointF c = circle.center();
  const QPointF at(c.x() - (ink.x() + ink.width() / 2.0), c.y() - (ink.y() + ink.height() / 2.0));
  p.setFont(font);
  p.setPen(Qt::white);
  p.drawText(at, hand);
  p.restore();
}

void paint_start_button(HudCanvas& c, const QRect& r) {
  const QString text = menu_button_text();
  c.paint_cached(hud_key({24, static_cast<uint64_t>(r.width()), static_cast<uint64_t>(r.height())}, text), r,
                 r.width(), r.height(), [&](QPainter& p) {
                   paint_start_qp(p, QRect(0, 0, r.width(), r.height()));
                 });
}

void paint_stick_icon(HudCanvas& c, const QRect& r, const QString& hand) {
  c.paint_cached(hud_key({18, static_cast<uint64_t>(r.width()), static_cast<uint64_t>(r.height())}, hand), r,
                 r.width(), r.height(), [&](QPainter& p) {
                   paint_stick_qp(p, QRect(0, 0, r.width(), r.height()), hand);
                 });
}

int help_icon_width(HelpIcon kind) {
  constexpr int kFace = kIcon;
  if (kind == HelpIcon::A || kind == HelpIcon::B) return kFace;
  if (kind == HelpIcon::Start) return menu_button_width();
  if (kind == HelpIcon::Stick) return 28 * 2 + 4 + 8 + kStick;
  if (kind == HelpIcon::StickR) return kStick;
  if (kind == HelpIcon::Speed) return 44 * 2 + 8;
  if (kind == HelpIcon::SpeedReset) return 96;
  return 0;
}

int paint_help_icons(HudCanvas& c, int x, int y, int h, HelpIcon kind) {
  constexpr int kFace = kIcon;
  const int face_y = y + (h - kFace) / 2;
  if (kind == HelpIcon::A || kind == HelpIcon::B) {
    paint_face_button(c, QRect(x, face_y, kFace, kFace), kind == HelpIcon::A);
    return kFace;
  }
  if (kind == HelpIcon::Start) {
    const int start_h = start_icon_height();
    const int by = y + (h - start_h) / 2;
    const int start_w = menu_button_width();
    paint_start_button(c, QRect(x, by, start_w, start_h));
    return start_w;
  }
  if (kind == HelpIcon::Stick) {
    constexpr int kBarW = 28;
    constexpr int kBarH = 22;
    constexpr int kBarGap = 4;
    const int bar_y = y + (h - kBarH) / 2;
    paint_square_button(c, QRect(x, bar_y, kBarW, kBarH), QStringLiteral("L"));
    paint_square_button(c, QRect(x + kBarW + kBarGap, bar_y, kBarW, kBarH), QStringLiteral("R"));
    const int sx = x + kBarW * 2 + kBarGap + 8;
    const int sy = y + (h - kStick) / 2;
    paint_stick_icon(c, QRect(sx, sy, kStick, kStick), QStringLiteral("L"));
    return help_icon_width(kind);
  }
  if (kind == HelpIcon::StickR) {
    const int sy = y + (h - kStick) / 2;
    paint_stick_icon(c, QRect(x, sy, kStick, kStick), QStringLiteral("R"));
    return kStick;
  }
  if (kind == HelpIcon::Speed) {
    constexpr int kTrigW = 44;
    constexpr int kTrigH = 30;
    constexpr int kTrigGap = 8;
    const int by = y + (h - kTrigH) / 2;
    paint_trigger_button(c, QRect(x, by, kTrigW, kTrigH), QStringLiteral("L2"));
    paint_trigger_button(c, QRect(x + kTrigW + kTrigGap, by, kTrigW, kTrigH), QStringLiteral("R2"));
    return help_icon_width(kind);
  }
  if (kind == HelpIcon::SpeedReset) {
    constexpr int kW = 96;
    constexpr int kH = 30;
    const int by = y + (h - kH) / 2;
    paint_trigger_button(c, QRect(x, by, kW, kH), QStringLiteral("L2+R2"));
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

int paint_stick_a_cluster(HudCanvas& c, int x, int y, int h, DpadDir dir, const QString& label,
                          const QFont& font) {
  constexpr int kDpad = 26;
  const int face_y = y + (h - kFootFace) / 2;
  const int dy = y + (h - kDpad) / 2;
  paint_dpad(c, QRect(x, dy, kDpad, kDpad), dir);
  paint_face_button(c, QRect(x + kDpad + 2, face_y, kFootFace, kFootFace), true);
  return paint_footer_label(c, x + kDpad + 2 + kFootFace + 4, y, h, label, font) + 14;
}

int paint_dpad_lr_cluster(HudCanvas& c, int x, int y, int h, const QString& label, const QFont& font) {
  constexpr int kDpad = 26;
  constexpr int kGap = 2;
  const int dy = y + (h - kDpad) / 2;
  paint_dpad(c, QRect(x, dy, kDpad, kDpad), DpadDir::Left);
  paint_dpad(c, QRect(x + kDpad + kGap, dy, kDpad, kDpad), DpadDir::Right);
  return paint_footer_label(c, x + kDpad * 2 + kGap + 4, y, h, label, font) + 14;
}

int paint_lr_stick_cluster(HudCanvas& c, int x, int y, int h, const QString& label, const QFont& font) {
  constexpr int kW = 22;
  constexpr int kH = 16;
  constexpr int kGap = 2;
  constexpr int kStick = 26;
  const int by = y + (h - kH) / 2;
  paint_square_button(c, QRect(x, by, kW, kH), QStringLiteral("L"));
  paint_square_button(c, QRect(x + kW + kGap, by, kW, kH), QStringLiteral("R"));
  const int sx = x + kW * 2 + kGap + 8;
  const int sy = y + (h - kStick) / 2;
  paint_stick_icon(c, QRect(sx, sy, kStick, kStick), QStringLiteral("L"));
  return paint_footer_label(c, sx + kStick + 4, y, h, label, font) + 14;
}

int paint_dpad_ud_stick_cluster(HudCanvas& c, int x, int y, int h, const QString& label, const QFont& font) {
  constexpr int kDpad = 26;
  constexpr int kGap = 2;
  constexpr int kStick = 26;
  const int dy = y + (h - kDpad) / 2;
  paint_dpad(c, QRect(x, dy, kDpad, kDpad), DpadDir::Up);
  paint_dpad(c, QRect(x + kDpad + kGap, dy, kDpad, kDpad), DpadDir::Down);
  const int sx = x + kDpad * 2 + kGap + 8;
  const int sy = y + (h - kStick) / 2;
  paint_stick_icon(c, QRect(sx, sy, kStick, kStick), QStringLiteral("L"));
  return paint_footer_label(c, sx + kStick + 4, y, h, label, font) + 14;
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

struct PlaybackPanel {
  int panel_x = 0;
  int panel_y = 0;
  int panel_w = 0;
  int panel_h = 0;
  int pad = 12;
  int scrub_y = 0;
  int title_y = 0;
  int buttons_y = 0;
  int info_y = 0;
  QRect bar;
  QRect time;
  bool has_info = false;
  int info_h = 0;
};

PlaybackPanel playback_panel(const VrMenu::Snapshot& snap, int width, int height, const QFont& time_font) {
  PlaybackPanel p;
  p.has_info = !snap.info_video_codec.empty() || !snap.info_res.empty() || !snap.info_audio_codec.empty();
  p.info_h = p.has_info ? 72 : 0;
  constexpr int kFootH = 40;
  constexpr int kBtn = 48;
  constexpr int kButtonsOff = 32;
  const int scrub_off = kButtonsOff + kBtn + 14;
  const int above_foot = p.has_info ? (scrub_off + 18 + (p.info_h - 8)) : (scrub_off + 16);
  p.panel_h = above_foot + kFootH;
  p.panel_y = height - p.panel_h;
  constexpr int kGap = 4;
  constexpr int kBtns = 6;
  constexpr int kVolW = 100;
  constexpr int kFmtW = 156;
  constexpr int kFsrW = 88;
  constexpr int kHzW = 80;
  constexpr int kGroupGap = 6;
  const int row_w = kBtn * kBtns + kGap * (kBtns - 1) + kGroupGap + kVolW + kGap + kFmtW + kGap + kFsrW +
                    kGap + kHzW;
  p.panel_w = row_w + p.pad * 2;
  p.panel_x = std::max(0, (width - p.panel_w) / 2);
  p.title_y = p.panel_y + 4;
  p.buttons_y = p.panel_y + kButtonsOff;
  p.scrub_y = p.panel_y + scrub_off;
  p.info_y = p.scrub_y + 18;
  const QString shown = fmt_time(snap.position_sec) + QStringLiteral(" / ") + fmt_time(snap.duration_sec);
  const QString wide = QStringLiteral("00:00:00 / 00:00:00");
  const int time_w = std::max(clock_label_width(shown, time_font), clock_label_width(wide, time_font)) + 6;
  const int time_h = std::max(22, QFontMetrics(time_font).height() + 4);
  constexpr int kBarH = 10;
  constexpr int kTimeGap = 10;
  const int bar_w = std::max(40, p.panel_w - p.pad * 2 - time_w - kTimeGap);
  p.bar = QRect(p.panel_x + p.pad, p.scrub_y, bar_w, kBarH);
  p.time = QRect(p.panel_x + p.pad + bar_w + kTimeGap, p.scrub_y + (kBarH - time_h) / 2, time_w, time_h);
  return p;
}

void paint_scrub_bar(HudCanvas& c, const VrMenu::Snapshot& snap, const QRect& bar, const QRect& time,
                     const QFont& hint_font) {
  const double duration = snap.duration_sec > 0 ? snap.duration_sec : 0;
  double ready = duration > 0 ? snap.thumb_cover_sec / duration : 1.0;
  ready = std::clamp(ready, 0.0, 1.0);
  const int ready_w = static_cast<int>(std::lround(bar.width() * ready));
  c.rounded(bar, QColor(92, 48, 36, 220), 3);
  if (ready_w > 0) {
    c.rounded(QRect(bar.x(), bar.y(), std::min(bar.width(), ready_w), bar.height()), QColor(30, 36, 48, 220), 3);
  }
  double t = 0;
  if (duration > 0) t = snap.position_sec / duration;
  t = std::clamp(t, 0.0, 1.0);
  const int fill_w = std::max(0, static_cast<int>(bar.width() * t));
  if (fill_w > 0) c.rounded(QRect(bar.x(), bar.y(), fill_w, bar.height()), QColor(50, 160, 255, 230), 3);
  const QString label = fmt_time(snap.position_sec) + QStringLiteral(" / ") + fmt_time(snap.duration_sec);
  c.clock(time, label, hint_font, QColor(200, 210, 220), Qt::AlignRight | Qt::AlignVCenter);
}

void paint_playback_controls(HudCanvas& c, const VrMenu::Snapshot& snap, int width, int height,
                             const QFont& body_font, const QFont& hint_font) {
  const PlaybackPanel panel = playback_panel(snap, width, height, hint_font);
  const bool has_info = panel.has_info;
  const int info_h = panel.info_h;
  const int top = panel.panel_y;
  const int panel_x = panel.panel_x;
  const int panel_w = panel.panel_w;
  const int panel_h = panel.panel_h;
  const int pad = panel.pad;

  const int gap = 4;
  const int btn = 48;
  const int foot_h = 40;
  const int chip_h = 48;
  constexpr int kBtns = 6;
  const int vol_w = 100;
  const int fmt_w = 156;
  const int fsr_w = 88;
  const int hz_w = 80;
  const int group_gap = 6;
  c.rounded(QRect(panel_x, top, panel_w, panel_h), QColor(16, 20, 30, 235), 8);

  const QString fname = snap.media_name.empty() ? VRP_TR(snap.title.c_str())
                                                : QString::fromStdString(snap.media_name);
  c.scrolling_text(QRect(panel_x + pad, panel.title_y, panel_w - pad * 2 - 80, 24), fname, body_font,
                   QColor(230, 240, 255));
  {
    const QString speed = QStringLiteral("×%1").arg(snap.rate, 0, 'g', 3);
    c.text(QRect(panel_x + panel_w - pad - 72, panel.title_y, 72, 24), speed, hint_font, QColor(120, 200, 255),
           Qt::AlignVCenter | Qt::AlignRight);
  }

  int x = panel_x + pad;
  const int y = panel.buttons_y;

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

  // Seek bar sits under the buttons. The standalone bar uses this same rect.
  paint_scrub_bar(c, snap, panel.bar, panel.time, hint_font);

  if (has_info) {
    const int info_top = panel.info_y;
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
    const QString select = VRP_TR("Select");
    const int select_w = 26 + 2 + 26 + 4 + fm.horizontalAdvance(select) + 14;
    if (fits(select_w)) lx = paint_dpad_lr_cluster(c, lx, foot.y(), foot.height(), select, body_font);
    const QString seek = VRP_TR("Seek");
    const int seek_w = 22 + 2 + 22 + 8 + 26 + 4 + fm.horizontalAdvance(seek) + 14;
    if (fits(seek_w)) paint_lr_stick_cluster(c, lx, foot.y(), foot.height(), seek, body_font);
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

void paint_seek_bar_only(HudCanvas& c, const VrMenu::Snapshot& snap, int width, int height,
                          const QFont& hint_font) {
  const PlaybackPanel panel = playback_panel(snap, width, height, hint_font);
  constexpr int kStripH = 36;
  const int strip_y = panel.bar.y() - (kStripH - panel.bar.height()) / 2;
  const QRect strip(panel.panel_x, strip_y, panel.panel_w, kStripH);
  c.rounded(strip, QColor(16, 20, 30, 220), 8);
  paint_scrub_bar(c, snap, panel.bar, panel.time, hint_font);
}

HudPaintResult paint_vr_menu(const VrMenu::Snapshot& snap, int width, int height, bool sony_face) {
  sony_face_buttons = sony_face;
  HudCanvas& canvas = shared_hud_canvas();
  canvas.begin(width, height);
  const QFont title_font = deUiFont(22, QFont::DemiBold);
  const QFont body_font = deUiFont(16, QFont::Normal);
  const QFont hint_font = deUiFont(13, QFont::Normal);

  if (!snap.visible && !snap.controls_visible) {
    if (snap.seek_bar_only) paint_seek_bar_only(canvas, snap, width, height, hint_font);
    return canvas.take();
  }

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
    paint_dpad_ud_stick_cluster(canvas, foot.x(), foot.y(), foot.height(), VRP_TR("Select"), body_font);
  }

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
    const QRect view(list_x, list_top, list_w, max_rows * row_h);
    canvas.set_clip(view);
    if (count > 0) {
      const float hy =
          static_cast<float>(list_top) + (snap.focus - snap.scroll_f) * static_cast<float>(row_h);
      canvas.fill(QRect(list_x + 8, static_cast<int>(std::lround(hy)) + 2, list_w - 16, row_h - 4),
                  QColor(40, 120, 200, 220));
    }
    const int start = std::max(0, static_cast<int>(std::floor(snap.scroll_f)));
    const int end = std::min(count, static_cast<int>(std::ceil(snap.scroll_f + static_cast<float>(max_rows))));
    for (int i = start; i < end; ++i) {
      const auto& e = snap.entries[static_cast<size_t>(i)];
      const float yf =
          static_cast<float>(list_top) + (static_cast<float>(i) - snap.scroll_f) * static_cast<float>(row_h);
      const QRect row(list_x, static_cast<int>(std::lround(yf)), list_w, row_h);
      const QString name = QString::fromStdString(e.name);
      if (e.is_dir && !e.is_special) draw_row_with_folder(canvas, row, name, body_font, i == snap.cursor);
      else draw_row(canvas, row, name, body_font);
    }
    canvas.set_clip({});

    if (show_scroll) {
      const QRect track(list_x + list_w + kScrollGap, list_top + 2, kScrollW, max_rows * row_h - 4);
      canvas.paint_cached(hud_key({10, static_cast<uint64_t>(track.width()), static_cast<uint64_t>(track.height())}),
                          track, track.width(), track.height(), [&](QPainter& p) {
                            p.setPen(QPen(QColor(190, 206, 230), 2));
                            p.setBrush(QColor(36, 46, 64));
                            p.drawRoundedRect(QRectF(0.5, 0.5, track.width() - 1, track.height() - 1), 6, 6);
                          });
      const int span = std::max(1, count - max_rows);
      const float pos = std::clamp(snap.scroll_f / static_cast<float>(span), 0.f, 1.f);
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

HudPaintResult paint_controls_help(int width, int height, int seconds_left, bool sony_face,
                                   bool menu_option) {
  sony_face_buttons = sony_face;
  menu_button_option = menu_option;
  HudCanvas& canvas = shared_hud_canvas();
  canvas.begin(width, height);

  struct Row {
    HelpIcon icon;
    const char* name;
    const char* action;
  };
  const Row rows[] = {
      {HelpIcon::Start, "long-press", "Recenter"},
      {HelpIcon::Start, nullptr, "File dialog"},
      {HelpIcon::B, nullptr, "Playback controls"},
      {HelpIcon::A, nullptr, "Play or pause"},
      {HelpIcon::Stick, nullptr, "Seek"},
      {HelpIcon::StickR, nullptr, "Look"},
      {HelpIcon::B, "long-press", "Reset view"},
      {HelpIcon::Speed, nullptr, "Playback speed"},
      {HelpIcon::SpeedReset, nullptr, "Speed ×1"},
  };

  const QFont title_font = deUiFont(22, QFont::DemiBold);
  const QFont body_font = deUiFont(16, QFont::Normal);
  const QFont desc_font = body_font;

  constexpr int kRows = static_cast<int>(sizeof(rows) / sizeof(rows[0]));
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
    const int icon_w = paint_help_icons(canvas, panel_x + pad, y, row_h, rows[i].icon);
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
