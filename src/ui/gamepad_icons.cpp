#include "ui/gamepad_icons.hpp"

#include <QFontInfo>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace vrp::icons {
namespace {

constexpr int kMenuPt = 11;
constexpr int kMenuPad = 2;
constexpr int kSuperSample = 2;

QImage make_plate(int w, int h) {
  QImage plate(std::max(1, w) * kSuperSample, std::max(1, h) * kSuperSample,
               QImage::Format_ARGB32_Premultiplied);
  plate.fill(Qt::transparent);
  return plate;
}

void blit_plate(QPainter& p, const QRect& r, const QImage& plate) {
  p.save();
  p.setRenderHint(QPainter::SmoothPixmapTransform, true);
  p.drawImage(r, plate);
  p.restore();
}

void paint_knockout(QPainter& p, const QRect& r, bool round, const QString& letter, QPoint shift,
                    qreal corner) {
  QImage plate = make_plate(r.width(), r.height());
  QPainter ip(&plate);
  ip.setRenderHint(QPainter::Antialiasing, true);
  ip.setRenderHint(QPainter::TextAntialiasing, true);
  ip.scale(kSuperSample, kSuperSample);
  ip.setPen(Qt::NoPen);
  ip.setBrush(Qt::white);
  const QRectF box(0, 0, r.width(), r.height());
  if (round) ip.drawEllipse(box.adjusted(0.5, 0.5, -0.5, -0.5));
  else ip.drawRoundedRect(box.adjusted(0.5, 0.5, -0.5, -0.5), corner, corner);
  ip.setCompositionMode(QPainter::CompositionMode_DestinationOut);
  ip.setFont(gothic_bold(15));
  ip.setPen(Qt::white);
  ip.drawText(box.translated(shift).toAlignedRect(), Qt::AlignCenter, letter);
  ip.end();
  blit_plate(p, r, plate);
}

void paint_sony_ab(QPainter& p, const QRect& r, bool cross) {
  QImage plate = make_plate(r.width(), r.height());
  QPainter ip(&plate);
  ip.setRenderHint(QPainter::Antialiasing, true);
  ip.scale(kSuperSample, kSuperSample);
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
  blit_plate(p, r, plate);
}

void paint_sony_xy(QPainter& p, const QRect& r, bool triangle) {
  QImage plate = make_plate(r.width(), r.height());
  QPainter ip(&plate);
  ip.setRenderHint(QPainter::Antialiasing, true);
  ip.scale(kSuperSample, kSuperSample);
  ip.setPen(Qt::NoPen);
  ip.setBrush(Qt::white);
  const QRectF box(0, 0, r.width(), r.height());
  ip.drawEllipse(box.adjusted(0.5, 0.5, -0.5, -0.5));

  const qreal d = std::min(box.width(), box.height());
  const QPointF c = box.center();
  const qreal thick = d * 0.14;
  ip.setCompositionMode(QPainter::CompositionMode_DestinationOut);
  ip.setBrush(Qt::white);
  if (triangle) {
    const qreal outer = d * 0.43;
    const qreal inner = outer - thick * (2.6 / std::sqrt(3.0));
    auto tri = [](const QPointF& cen, qreal s) {
      QPainterPath path;
      path.moveTo(cen.x(), cen.y() - s);
      path.lineTo(cen.x() + s * 0.866, cen.y() + s * 0.5);
      path.lineTo(cen.x() - s * 0.866, cen.y() + s * 0.5);
      path.closeSubpath();
      return path;
    };
    QPainterPath cut;
    cut.setFillRule(Qt::OddEvenFill);
    cut.addPath(tri(c, outer));
    cut.addPath(tri(c, inner));
    ip.drawPath(cut);
  } else {
    const qreal outer = d * 0.40;
    const qreal rad = d * 0.08;
    QPainterPath cut;
    cut.setFillRule(Qt::OddEvenFill);
    cut.addRoundedRect(QRectF(c.x() - outer, c.y() - outer, outer * 2, outer * 2), rad, rad);
    const qreal inn = outer - thick;
    cut.addRoundedRect(QRectF(c.x() - inn, c.y() - inn, inn * 2, inn * 2), rad * 0.7, rad * 0.7);
    ip.drawPath(cut);
  }
  ip.end();
  blit_plate(p, r, plate);
}

void paint_stick_chevron(QPainter& p, const QPointF& tip, qreal len, qreal point_angle_deg) {
  p.save();
  p.translate(tip);
  p.rotate(point_angle_deg);
  QPainterPath path;
  path.moveTo(0, 0);
  path.lineTo(len, len * 0.62);
  path.lineTo(len, -len * 0.62);
  path.closeSubpath();
  p.setPen(Qt::NoPen);
  p.setBrush(Qt::white);
  p.drawPath(path);
  p.restore();
}

QImage blank(int w, int h) {
  QImage img(std::max(1, w), std::max(1, h), QImage::Format_ARGB32_Premultiplied);
  img.fill(Qt::transparent);
  return img;
}

}  // namespace

QFont gothic_bold(int point_size) {
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

QFont trigger_label_font(const QString& label, qreal box_w, qreal box_h) {
  (void)box_h;
  QFont font = gothic_bold(6);
  for (int pt = 14; pt >= 6; --pt) {
    const QFont try_font = gothic_bold(pt);
    if (QFontMetricsF(try_font).tightBoundingRect(label).width() <= box_w - 4) {
      font = try_font;
      break;
    }
  }
  return font;
}

void paint_face(QPainter& p, const QRect& r, FaceButton button, FaceLayout layout) {
  if (layout == FaceLayout::Sony) {
    switch (button) {
      case FaceButton::A:
        paint_sony_ab(p, r, true);
        return;
      case FaceButton::B:
        paint_sony_ab(p, r, false);
        return;
      case FaceButton::X:
        paint_sony_xy(p, r, false);
        return;
      case FaceButton::Y:
        paint_sony_xy(p, r, true);
        return;
    }
  }
  QString letter;
  switch (button) {
    case FaceButton::A:
      letter = QStringLiteral("A");
      break;
    case FaceButton::B:
      letter = QStringLiteral("B");
      break;
    case FaceButton::X:
      letter = QStringLiteral("X");
      break;
    case FaceButton::Y:
      letter = QStringLiteral("Y");
      break;
  }
  paint_knockout(p, r, true, letter, QPoint(0, 0), 0);
}

void paint_trigger(QPainter& p, const QRect& r, const QString& label) {
  QImage plate = make_plate(r.width(), r.height());
  QPainter ip(&plate);
  ip.setRenderHint(QPainter::Antialiasing, true);
  ip.setRenderHint(QPainter::TextAntialiasing, true);
  ip.scale(kSuperSample, kSuperSample);
  ip.setPen(Qt::NoPen);
  ip.setBrush(Qt::white);
  const QRectF b(0.5, 0.5, std::max(1, r.width()) - 1.0, std::max(1, r.height()) - 1.0);
  const qreal rad = std::min(b.width(), b.height()) * 0.42;
  ip.drawRoundedRect(b, rad, rad);
  const QFont font = trigger_label_font(label, b.width(), b.height());
  const QRectF ink = QFontMetricsF(font).tightBoundingRect(label);
  const QPointF c = b.center();
  const QPointF at(c.x() - (ink.x() + ink.width() / 2.0), c.y() - (ink.y() + ink.height() / 2.0));
  ip.setCompositionMode(QPainter::CompositionMode_DestinationOut);
  ip.setFont(font);
  ip.setPen(Qt::white);
  ip.drawText(at, label);
  ip.end();
  blit_plate(p, r, plate);
}

void paint_stick(QPainter& p, const QRect& r, const QString& hand, StickMotion motion) {
  p.save();
  p.setRenderHint(QPainter::Antialiasing, true);
  p.setRenderHint(QPainter::TextAntialiasing, true);
  const QRectF b = QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5);
  const qreal side = std::min(b.width(), b.height());
  const QRectF circle(b.center().x() - side / 2.0, b.center().y() - side / 2.0, side, side);
  p.setPen(Qt::NoPen);
  p.setBrush(QColor(140, 146, 156));
  p.drawEllipse(circle);
  const QPointF c = circle.center();
  const QFont font = trigger_label_font(QStringLiteral("R1"));
  p.setFont(font);
  p.setPen(Qt::white);
  p.drawText(circle.toAlignedRect(), Qt::AlignCenter, hand);
  const QRectF ink = QFontMetricsF(font).tightBoundingRect(hand);
  const qreal chev = side * 0.14;
  const qreal rim_inset = side * 0.005;
  auto paint_lr = [&] {
    paint_stick_chevron(p, QPointF(circle.left() + rim_inset, c.y()), chev, 0);
    paint_stick_chevron(p, QPointF(circle.right() - rim_inset, c.y()), chev, 180);
  };
  auto paint_ud = [&] {
    paint_stick_chevron(p, QPointF(c.x(), circle.top() + rim_inset), chev, 90);
    paint_stick_chevron(p, QPointF(c.x(), circle.bottom() - rim_inset), chev, -90);
  };
  if (motion == StickMotion::AxisLR) {
    paint_lr();
  } else if (motion == StickMotion::AxisUD) {
    paint_ud();
  } else if (motion == StickMotion::Click) {
    const qreal rr = std::max(ink.width(), ink.height()) * 0.92;
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(Qt::white, std::max(1.0, side * 0.035)));
    p.drawEllipse(c, rr, rr);
  }
  p.restore();
}

void paint_dpad(QPainter& p, const QRect& r, DpadDir dir) {
  p.save();
  p.setRenderHint(QPainter::Antialiasing, true);
  p.setPen(Qt::NoPen);
  const QPointF c(r.x() + r.width() / 2.0, r.y() + r.height() / 2.0);
  const qreal s = std::min(r.width(), r.height());
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

void paint_menu_chip(QPainter& p, const QRect& r, const QString& text) {
  p.save();
  p.setRenderHint(QPainter::Antialiasing, true);
  p.setRenderHint(QPainter::TextAntialiasing, true);
  const QFont font = gothic_bold(kMenuPt);
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

int menu_chip_width(const QString& text) {
  const int tw = QFontMetrics(gothic_bold(kMenuPt)).horizontalAdvance(text);
  return tw + kMenuPad * 2 + 8;
}

int menu_chip_height(const QString& text) {
  const int text_h = QFontMetrics(gothic_bold(kMenuPt)).tightBoundingRect(text).height();
  return text_h + kMenuPad * 2 + 4;
}

QImage render_face(int size, FaceButton button, FaceLayout layout) {
  QImage img = blank(size, size);
  QPainter p(&img);
  p.setRenderHint(QPainter::Antialiasing, true);
  paint_face(p, QRect(0, 0, size, size), button, layout);
  return img;
}

QImage render_trigger(int w, int h, const QString& label) {
  QImage img = blank(w, h);
  QPainter p(&img);
  p.setRenderHint(QPainter::Antialiasing, true);
  paint_trigger(p, QRect(0, 0, w, h), label);
  return img;
}

QImage render_stick(int size, const QString& hand, StickMotion motion) {
  QImage img = blank(size, size);
  QPainter p(&img);
  p.setRenderHint(QPainter::Antialiasing, true);
  paint_stick(p, QRect(0, 0, size, size), hand, motion);
  return img;
}

QImage render_dpad(int size, DpadDir dir) {
  QImage img = blank(size, size);
  QPainter p(&img);
  p.setRenderHint(QPainter::Antialiasing, true);
  paint_dpad(p, QRect(0, 0, size, size), dir);
  return img;
}

QImage render_menu_chip(int w, int h, const QString& text) {
  QImage img = blank(w, h);
  QPainter p(&img);
  p.setRenderHint(QPainter::Antialiasing, true);
  paint_menu_chip(p, QRect(0, 0, w, h), text);
  return img;
}

}  // namespace vrp::icons
