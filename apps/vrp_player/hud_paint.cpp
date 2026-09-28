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
#include <chrono>
#include <cmath>
#include <cstdio>

namespace vrp {
namespace {

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

void draw_row(QPainter& p, const QRect& r, const QString& text, bool selected, bool dim = false) {
  if (selected) {
    p.fillRect(r.adjusted(8, 2, -8, -2), QColor(40, 120, 200, 220));
  }
  p.setPen(dim ? QColor(160, 170, 180) : QColor(240, 245, 250));
  p.drawText(r.adjusted(24, 0, -16, 0), Qt::AlignVCenter | Qt::AlignLeft, text);
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

void draw_row_with_folder(QPainter& p, const QRect& r, const QString& text, bool selected) {
  if (selected) {
    p.fillRect(r.adjusted(8, 2, -8, -2), QColor(40, 120, 200, 220));
  }
  const int icon = std::min(22, r.height() - 8);
  const QRect icon_r(r.left() + 14, r.center().y() - icon / 2, icon, icon);
  draw_folder_icon(p, icon_r, selected);
  p.setPen(QColor(240, 245, 250));
  p.drawText(r.adjusted(14 + icon + 8, 0, -16, 0), Qt::AlignVCenter | Qt::AlignLeft, text);
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

void paint_transport_icon(QPainter& p, const QRect& r, TransportIcon kind, bool selected) {
  // Render 2× then smooth-scale into the button for softer edges.
  const QImage hi = make_transport_icon(kind, r.width() * 2, selected);
  p.setRenderHint(QPainter::SmoothPixmapTransform, true);
  p.setRenderHint(QPainter::Antialiasing, true);
  p.drawImage(r, hi);
}

void draw_scrolling_text(QPainter& p, const QRect& r, const QString& text, const QColor& color) {
  p.setPen(color);
  const QFontMetrics fm(p.font());
  const int tw = fm.horizontalAdvance(text);
  if (tw <= r.width()) {
    p.drawText(r, Qt::AlignVCenter | Qt::AlignLeft, text);
    return;
  }
  // Marquee: scroll left, pause briefly at ends.
  using clock = std::chrono::steady_clock;
  static clock::time_point t0 = clock::now();
  const double sec = std::chrono::duration<double>(clock::now() - t0).count();
  const int travel = tw - r.width() + 8;
  const double cycle = 2.5 + travel / 40.0;  // seconds per round trip
  const double phase = std::fmod(sec, cycle * 2.0);
  double u = phase / cycle;
  if (u > 1.0) u = 2.0 - u;  // ping-pong
  // Ease ends
  u = u * u * (3.0 - 2.0 * u);
  const int xoff = static_cast<int>(std::lround(u * travel));
  p.save();
  p.setClipRect(r);
  p.drawText(QRect(r.x() - xoff, r.y(), tw + 8, r.height()), Qt::AlignVCenter | Qt::AlignLeft, text);
  p.restore();
}

enum class DpadDir { Up, Down };

void paint_knockout_icon(QPainter& p, const QRect& r, bool round, const QString& letter, QPoint shift,
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

void paint_round_button(QPainter& p, const QRect& r, const QString& letter) {
  paint_knockout_icon(p, r, true, letter, QPoint(0, 0), 0);
}

void paint_sony_face(QPainter& p, const QRect& r, bool cross) {
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

void paint_face_button(QPainter& p, const QRect& r, bool confirm) {
  if (!sony_face_buttons) {
    paint_round_button(p, r, confirm ? QStringLiteral("A") : QStringLiteral("B"));
    return;
  }
  paint_sony_face(p, r, confirm);
}

void paint_square_button(QPainter& p, const QRect& r, const QString& letter) {
  const qreal corner = std::min(r.width(), r.height()) * 0.35;
  paint_knockout_icon(p, r, false, letter, QPoint(0, 0), corner);
}

void paint_dpad(QPainter& p, const QRect& box, DpadDir dir) {
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

int paint_help_icons(QPainter& p, int x, int y, int h, HelpIcon kind) {
  constexpr int kFace = 32;
  const int face_y = y + (h - kFace) / 2;
  if (kind == HelpIcon::A || kind == HelpIcon::B) {
    paint_face_button(p, QRect(x, face_y, kFace, kFace), kind == HelpIcon::A);
    return kFace;
  }
  if (kind == HelpIcon::LR) {
    constexpr int kBarW = 54;
    constexpr int kBarH = 26;
    constexpr int kBarGap = 6;
    const int bar_y = y + (h - kBarH) / 2;
    paint_square_button(p, QRect(x, bar_y, kBarW, kBarH), QStringLiteral("L"));
    paint_square_button(p, QRect(x + kBarW + kBarGap, bar_y, kBarW, kBarH), QStringLiteral("R"));
    return kBarW * 2 + kBarGap;
  }
  if (kind == HelpIcon::Speed) {
    constexpr int kDpad = 36;
    constexpr int kTight = 2;
    const int dy = y + (h - kDpad) / 2;
    int cx = x;
    paint_dpad(p, QRect(cx, dy, kDpad, kDpad), DpadDir::Up);
    cx += kDpad + kTight;
    paint_face_button(p, QRect(cx, face_y, kFace, kFace), true);
    cx += kFace;
    p.setPen(QColor(180, 190, 205));
    QFont f = p.font();
    f.setPointSize(16);
    p.setFont(f);
    p.drawText(QRect(cx, y, 16, h), Qt::AlignCenter, QStringLiteral("·"));
    cx += 16;
    paint_dpad(p, QRect(cx, dy, kDpad, kDpad), DpadDir::Down);
    cx += kDpad + kTight;
    paint_face_button(p, QRect(cx, face_y, kFace, kFace), true);
    return help_icon_width(kind);
  }
  return 0;
}

constexpr int kFootFace = 26;

int paint_footer_label(QPainter& p, int x, int y, int h, const QString& text) {
  const int tw = QFontMetrics(p.font()).horizontalAdvance(text);
  p.setPen(QColor(230, 236, 245));
  p.drawText(QRect(x, y, tw + 2, h), Qt::AlignVCenter | Qt::AlignLeft, text);
  return x + tw;
}

int paint_ab_footer(QPainter& p, int right, int y, int h, const QString& a_label, const QString& b_label) {
  const QFontMetrics fm(p.font());
  const int face_y = y + (h - kFootFace) / 2;
  const int bw = fm.horizontalAdvance(b_label);
  const int aw = fm.horizontalAdvance(a_label);
  int x = right - bw;
  paint_footer_label(p, x, y, h, b_label);
  x -= 6 + kFootFace;
  paint_face_button(p, QRect(x, face_y, kFootFace, kFootFace), false);
  if (a_label == b_label) {
    x -= 4 + kFootFace;
  } else {
    x -= 14 + aw;
    paint_footer_label(p, x, y, h, a_label);
    x -= 6 + kFootFace;
  }
  paint_face_button(p, QRect(x, face_y, kFootFace, kFootFace), true);
  return x;
}

int paint_lr_cluster(QPainter& p, int x, int y, int h, const QString& label) {
  constexpr int kBarW = 40;
  constexpr int kBarH = 22;
  constexpr int kGap = 4;
  const int bar_y = y + (h - kBarH) / 2;
  paint_square_button(p, QRect(x, bar_y, kBarW, kBarH), QStringLiteral("L"));
  paint_square_button(p, QRect(x + kBarW + kGap, bar_y, kBarW, kBarH), QStringLiteral("R"));
  return paint_footer_label(p, x + kBarW * 2 + kGap + 4, y, h, label) + 14;
}

int paint_stick_a_cluster(QPainter& p, int x, int y, int h, DpadDir dir, const QString& label) {
  constexpr int kDpad = 26;
  const int face_y = y + (h - kFootFace) / 2;
  const int dy = y + (h - kDpad) / 2;
  paint_dpad(p, QRect(x, dy, kDpad, kDpad), dir);
  paint_face_button(p, QRect(x + kDpad + 2, face_y, kFootFace, kFootFace), true);
  return paint_footer_label(p, x + kDpad + 2 + kFootFace + 4, y, h, label) + 14;
}

int paint_stick_cluster(QPainter& p, int x, int y, int h, const QString& label) {
  constexpr int kDpad = 26;
  constexpr int kGap = 2;
  const int dy = y + (h - kDpad) / 2;
  paint_dpad(p, QRect(x, dy, kDpad, kDpad), DpadDir::Up);
  paint_dpad(p, QRect(x + kDpad + kGap, dy, kDpad, kDpad), DpadDir::Down);
  return paint_footer_label(p, x + kDpad * 2 + kGap + 4, y, h, label) + 14;
}

void draw_chip(QPainter& p, const QRect& r, const QString& text, bool selected, bool editing = false) {
  draw_flat_square(p, r, selected, editing);
  p.setPen(selected || editing ? QColor(20, 28, 40) : QColor(245, 248, 252));
  p.drawText(r, Qt::AlignCenter, text);
}

void draw_mode_chip(QPainter& p, const QRect& r, const QString& top, const QString& bottom, bool selected,
                    bool editing = false) {
  draw_flat_square(p, r, selected, editing);
  p.save();
  p.setPen(selected || editing ? QColor(20, 28, 40) : QColor(245, 248, 252));
  const QRect inner = r.adjusted(2, 1, -2, -1);
  const int half = inner.height() / 2;
  p.drawText(QRect(inner.x(), inner.y(), inner.width(), half), Qt::AlignCenter, top);
  p.drawText(QRect(inner.x(), inner.y() + half, inner.width(), inner.height() - half), Qt::AlignCenter,
             bottom);
  p.restore();
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

void paint_playback_controls(QPainter& p, const VrMenu::Snapshot& snap, int width, int height,
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
  const int fmt_w = 112;
  const int fsr_w = 88;
  const int hz_w = 80;
  const int group_gap = 6;
  const int row_w = btn * kBtns + gap * (kBtns - 1) + group_gap + vol_w + gap + fmt_w + gap +
                    fsr_w + gap + hz_w;
  const int pad = 12;
  const int panel_w = row_w + pad * 2;
  const int panel_x = std::max(0, (width - panel_w) / 2);
  const QColor panel_bg(16, 20, 30, 235);
  p.setRenderHint(QPainter::Antialiasing, true);
  p.setPen(Qt::NoPen);
  p.setBrush(panel_bg);
  p.drawRoundedRect(QRect(panel_x, top, panel_w, panel_h), 8, 8);

  // Title + rate (long names marquee)
  p.setFont(body_font);
  const QString fname = snap.media_name.empty() ? VRP_TR(snap.title.c_str())
                                                : QString::fromStdString(snap.media_name);
  draw_scrolling_text(p, QRect(panel_x + pad, top + 4, panel_w - pad * 2 - 80, 24), fname,
                      QColor(230, 240, 255));
  {
    QString speed = QStringLiteral("×%1").arg(snap.rate, 0, 'g', 3);
    p.setPen(QColor(120, 200, 255));
    p.setFont(hint_font);
    p.drawText(QRect(panel_x + panel_w - pad - 72, top + 4, 72, 24),
               Qt::AlignVCenter | Qt::AlignRight, speed);
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
    draw_flat_square(p, QRect(x, y, btn, btn), on);
    paint_transport_icon(p, QRect(x, y, btn, btn), icons[i], on);
    x += btn + gap;
  }
  x += group_gap - gap;

  p.setFont(hint_font);
  const int vol_pct = static_cast<int>(std::lround(snap.volume * 100.f));
  const QRect vol_r(x, y, vol_w, chip_h);
  draw_chip(p, vol_r, VRP_TR("Vol%1%").arg(vol_pct), sel == 6,
            snap.controls_edit == VrMenu::ControlsEdit::Volume);
  x += vol_w + gap;

  const QRect fmt_r(x, y, fmt_w, chip_h);
  draw_mode_chip(p, fmt_r, proj_label(snap.projection), stereo_label(snap.stereo), sel == 7,
                 snap.controls_edit == VrMenu::ControlsEdit::FormatPick);
  x += fmt_w + gap;

  const QRect fsr_r(x, y, fsr_w, chip_h);
  draw_chip(p, fsr_r, fsr_label(snap.fsr), sel == 8, snap.controls_edit == VrMenu::ControlsEdit::FsrPick);
  x += fsr_w + gap;

  const QRect hz_r(x, y, hz_w, chip_h);
  QString hz_chip;
  if (snap.preferred_hz > 0) hz_chip = QStringLiteral("%1Hz").arg(snap.preferred_hz);
  else if (snap.display_hz > 0.5f) hz_chip = QStringLiteral("%1Hz").arg(snap.display_hz, 0, 'f', 0);
  else hz_chip = VRP_TR("Auto");
  draw_chip(p, hz_r, hz_chip, sel == 9,
            snap.controls_edit == VrMenu::ControlsEdit::HzPick ||
                snap.controls_edit == VrMenu::ControlsEdit::HzConfirm);

  // Seek bar + time within panel
  const int scrub_y = y + btn + 14;
  const int time_w = 110;
  const QRect bar(panel_x + pad, scrub_y, panel_w - pad * 2 - time_w - 8, 10);
  p.setPen(Qt::NoPen);
  p.setBrush(QColor(30, 36, 48, 200));
  p.drawRoundedRect(bar, 3, 3);
  double t = 0;
  if (snap.duration_sec > 0) t = snap.position_sec / snap.duration_sec;
  t = std::clamp(t, 0.0, 1.0);
  p.setBrush(QColor(50, 160, 255, 230));
  p.drawRoundedRect(QRect(bar.x(), bar.y(), std::max(0, static_cast<int>(bar.width() * t)), bar.height()),
                    3, 3);
  p.setFont(hint_font);
  p.setPen(QColor(200, 210, 220));
  p.drawText(QRect(panel_x + panel_w - pad - time_w, scrub_y - 6, time_w, 22),
             Qt::AlignRight | Qt::AlignVCenter,
             fmt_time(snap.position_sec) + QStringLiteral(" / ") + fmt_time(snap.duration_sec));

  if (has_info) {
    const int info_top = scrub_y + 18;
    const QRect info_box(panel_x + pad, info_top, panel_w - pad * 2, info_h - 8);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 210));
    p.drawRoundedRect(info_box, 4, 4);
    QFont info_font = hint_font;
    info_font.setPointSize(11);
    p.setFont(info_font);
    p.setPen(QColor(20, 20, 24));
    const QFontMetrics fm(info_font);
    const int text_w = info_box.width() - 16;
    int iy = info_box.y() + 5;
    auto line = [&](const QString& text) {
      p.drawText(QRect(info_box.x() + 8, iy, text_w, 18), Qt::AlignLeft | Qt::AlignVCenter,
                 fm.elidedText(text, Qt::ElideRight, text_w));
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
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(24, 30, 42, 240));
    p.drawRoundedRect(box, 6, 6);
    p.setFont(hint_font);
    p.setPen(QColor(220, 230, 245));
    const QString msg = VRP_TR("Hz change\nneeds restart");
    p.drawText(box.adjusted(10, 8, -10, -40), Qt::AlignTop | Qt::AlignHCenter | Qt::TextWordWrap,
               msg);
    const QRect btn_r(box.center().x() - 48, box.bottom() - 36, 96, 28);
    p.setBrush(QColor(40, 120, 200, 230));
    p.drawRoundedRect(btn_r, 4, 4);
    p.setPen(QColor(255, 255, 255));
    p.drawText(btn_r, Qt::AlignCenter, VRP_TR("[Restart]"));
  } else if (snap.controls_edit == VrMenu::ControlsEdit::FsrPick ||
             snap.controls_edit == VrMenu::ControlsEdit::FormatPick ||
             snap.controls_edit == VrMenu::ControlsEdit::HzPick) {
    const bool fsr = snap.controls_edit == VrMenu::ControlsEdit::FsrPick;
    const bool hz = snap.controls_edit == VrMenu::ControlsEdit::HzPick;
    const int n = fsr ? 4 : (hz ? 3 : 6);
    const int row_h = 34;
    const int box_w = fsr ? 140 : (hz ? 120 : 168);
    const int box_h = 12 + n * row_h;
    const QRect anchor = fsr ? fsr_r : (hz ? hz_r : fmt_r);
    int box_x = anchor.center().x() - box_w / 2;
    box_x = std::clamp(box_x, panel_x + 4, panel_x + panel_w - box_w - 4);
    const int box_y = anchor.top() - box_h - 6;
    const QRect box(box_x, std::max(4, box_y), box_w, box_h);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(24, 30, 42, 235));
    p.drawRoundedRect(box, 6, 6);
    p.setFont(hint_font);
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
      } else if (i < 3) {
        label = proj_label(static_cast<ProjectionMode>(i));
      } else {
        label = stereo_label(static_cast<StereoLayout>(i - 3));
      }
      const QRect row(box.x() + 6, box.y() + 6 + i * row_h, box.width() - 12, row_h - 4);
      const bool on = (i == snap.picker_cursor);
      if (on) {
        p.setBrush(QColor(40, 120, 200, 220));
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(row, 4, 4);
      }
      p.setPen(on ? QColor(255, 255, 255) : QColor(210, 220, 235));
      p.drawText(row.adjusted(10, 0, -6, 0), Qt::AlignVCenter | Qt::AlignLeft, label);
    }
  }

  p.setFont(body_font);
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
      paint_ab_footer(p, foot.right(), foot.y(), foot.height(), VRP_TR(a_src), VRP_TR(b_src));
  const QFontMetrics fm(body_font);
  int lx = foot.x();
  const int limit = actions_left - 12;
  auto fits = [&](int w) { return lx + w <= limit; };
  if (left_kind == 0) {
    const QString seek = VRP_TR("Seek");
    const int seek_w = 40 + 4 + 40 + 4 + fm.horizontalAdvance(seek) + 14;
    if (fits(seek_w)) lx = paint_lr_cluster(p, lx, foot.y(), foot.height(), seek);
    const QString faster = VRP_TR("Faster");
    const int spd_w = 26 + 2 + kFootFace + 4 + fm.horizontalAdvance(faster) + 14;
    if (fits(spd_w)) lx = paint_stick_a_cluster(p, lx, foot.y(), foot.height(), DpadDir::Up, faster);
    const QString slower = VRP_TR("Slower");
    const int slow_w = 26 + 2 + kFootFace + 4 + fm.horizontalAdvance(slower) + 14;
    if (fits(slow_w)) paint_stick_a_cluster(p, lx, foot.y(), foot.height(), DpadDir::Down, slower);
  } else if (left_kind == 1 || left_kind == 2) {
    const QString label = left_kind == 1 ? VRP_TR("Volume") : VRP_TR("Select");
    const int w = 26 + 2 + 26 + 4 + fm.horizontalAdvance(label) + 14;
    if (fits(w)) paint_stick_cluster(p, lx, foot.y(), foot.height(), label);
  }
}

}  // namespace

QImage paint_vr_menu(const VrMenu::Snapshot& snap, int width, int height, bool sony_face) {
  sony_face_buttons = sony_face;
  QImage img(width, height, QImage::Format_RGBA8888);
  img.fill(QColor(12, 16, 24, 230));

  if (!snap.visible && !snap.controls_visible) {
    img.fill(Qt::transparent);
    return img;
  }

  QPainter p(&img);
  p.setRenderHint(QPainter::Antialiasing, true);
  p.setRenderHint(QPainter::TextAntialiasing, true);

  const QFont title_font = deUiFont(22, QFont::DemiBold);
  const QFont body_font = deUiFont(16, QFont::Normal);
  const QFont hint_font = deUiFont(13, QFont::Normal);

  if (snap.controls_visible) {
    img.fill(Qt::transparent);
    paint_playback_controls(p, snap, width, height, body_font, hint_font);
    p.end();
    return img;
  }

  // Start menu / file manager
  img.fill(qRgba(0, 0, 0, 0));
  const int side = width / 7;
  const int content_w = width - side * 2;

  p.setCompositionMode(QPainter::CompositionMode_Source);
  p.fillRect(side, 0, content_w, height, QColor(12, 16, 24, 230));
  p.setCompositionMode(QPainter::CompositionMode_SourceOver);

  p.fillRect(side, 0, content_w, 48, QColor(20, 28, 40, 255));
  p.setFont(title_font);
  p.setPen(QColor(230, 240, 255));
  p.drawText(QRect(side + 16, 4, content_w - 32, 40), Qt::AlignVCenter | Qt::AlignLeft,
             VRP_TR(snap.title.c_str()));

  p.setFont(body_font);
  const QRect foot(side + 16, height - 44, content_w - 32, 40);
  paint_ab_footer(p, foot.right(), foot.y(), foot.height(), VRP_TR("Decide"), VRP_TR("Back"));

  if (snap.screen == VrMenu::Screen::Browser) {
    const int list_top = 64;
    const int preview_w = 220;
    const bool show_preview =
        snap.cursor >= 0 && snap.cursor < static_cast<int>(snap.entries.size()) &&
        snap.entries[static_cast<size_t>(snap.cursor)].is_video && snap.preview_w > 0 &&
        snap.preview_h > 0 &&
        snap.preview_rgba.size() >= static_cast<size_t>(snap.preview_w * snap.preview_h * 4);

    const int list_x = side + 10;
    const int list_w = show_preview ? (content_w - preview_w - 32) : (content_w - 24);
    const int row_h = 34;
    const int max_rows = std::max(8, (height - list_top - 52) / row_h);
    p.setFont(body_font);
    const int start = snap.scroll;
    const int end = std::min(static_cast<int>(snap.entries.size()), start + max_rows);
    for (int i = start; i < end; ++i) {
      const auto& e = snap.entries[static_cast<size_t>(i)];
      const QRect row(list_x, list_top + (i - start) * row_h, list_w, row_h);
      const bool sel = (i == snap.cursor);
      if (e.is_special) {
        draw_row(p, row, QString::fromStdString(e.name), sel);
      } else if (e.is_dir) {
        draw_row_with_folder(p, row, QString::fromStdString(e.name), sel);
      } else {
        draw_row(p, row, QString::fromStdString(e.name), sel);
      }
    }

    if (show_preview) {
      const QRect prev(side + content_w - preview_w - 12, list_top, preview_w, 124);
      p.fillRect(prev, QColor(8, 10, 14));
      p.setPen(QColor(60, 70, 90));
      p.drawRect(prev);
      QImage thumb(snap.preview_rgba.data(), snap.preview_w, snap.preview_h, snap.preview_w * 4,
                   QImage::Format_RGBA8888);
      p.drawImage(prev, thumb);
    }
  } else if (snap.screen == VrMenu::Screen::Format) {
    p.setFont(body_font);
    const char* labels[] = {QT_TR_NOOP("Flat"), "180°", "360°", "mono", "SBS", "OU"};
    for (int i = 0; i < 6; ++i) {
      bool on = false;
      if (i == 0) on = snap.projection == ProjectionMode::Flat;
      if (i == 1) on = snap.projection == ProjectionMode::Deg180;
      if (i == 2) on = snap.projection == ProjectionMode::Deg360;
      if (i == 3) on = snap.stereo == StereoLayout::Mono;
      if (i == 4) on = snap.stereo == StereoLayout::Sbs;
      if (i == 5) on = snap.stereo == StereoLayout::OverUnder;
      QString t = (i == 0 || i == 3) ? VRP_TR(labels[i]) : QString::fromUtf8(labels[i]);
      if (on) t += QStringLiteral("  ✓");
      draw_row(p, QRect(side + 24, 80 + i * 48, content_w - 48, 44), t, i == snap.format_cursor);
    }
  }

  p.setCompositionMode(QPainter::CompositionMode_Source);
  p.fillRect(0, 0, side, height, QColor(0, 0, 0, 0));
  p.fillRect(side + content_w, 0, width - (side + content_w), height, QColor(0, 0, 0, 0));
  p.setCompositionMode(QPainter::CompositionMode_SourceOver);

  p.end();
  return img;
}

QImage paint_controls_help(int width, int height, int seconds_left, bool sony_face) {
  sony_face_buttons = sony_face;
  QImage img(width, height, QImage::Format_RGBA8888);
  img.fill(Qt::transparent);

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

  QPainter p(&img);
  p.setRenderHint(QPainter::Antialiasing, true);
  p.setRenderHint(QPainter::TextAntialiasing, true);
  p.setPen(Qt::NoPen);
  p.setBrush(QColor(16, 20, 30, 238));
  p.drawRoundedRect(QRect(panel_x, panel_y, panel_w, panel_h), 12, 12);

  p.setFont(title_font);
  p.setPen(QColor(230, 240, 255));
  p.drawText(QRect(panel_x + pad, panel_y + 6, panel_w - pad * 2, title_h - 8),
             Qt::AlignVCenter | Qt::AlignLeft, VRP_TR("Controls"));

  const int action_x = panel_x + pad + left_max + col_gap;
  const int action_w = panel_x + panel_w - pad - action_x;

  for (int i = 0; i < kRows; ++i) {
    const int y = panel_y + title_h + i * row_h;
    const QRect row(panel_x + 8, y, panel_w - 16, row_h);
    if (i % 2 == 0) {
      p.setPen(Qt::NoPen);
      p.setBrush(QColor(255, 255, 255, 10));
      p.drawRoundedRect(row, 6, 6);
    }
    p.setFont(body_font);
    const int icon_w = paint_help_icons(p, panel_x + pad, y, row_h, rows[i].icon);
    int text_x = panel_x + pad + icon_w;
    if (icon_w > 0 && rows[i].name) text_x += 4;
    p.setPen(QColor(230, 236, 245));
    if (rows[i].name) {
      const QString name = VRP_TR(rows[i].name);
      p.drawText(QRect(text_x, y, name_fm.horizontalAdvance(name) + 8, row_h),
                 Qt::AlignVCenter | Qt::AlignLeft, name);
    }
    p.setFont(desc_font);
    p.setPen(QColor(245, 248, 252));
    p.drawText(QRect(action_x, y, action_w, row_h), Qt::AlignVCenter | Qt::AlignLeft,
               VRP_TR(rows[i].action));
  }

  {
    p.setFont(body_font);
    const QString close = VRP_TR("Close");
    constexpr int kFace = 26;
    const int tw = name_fm.horizontalAdvance(close);
    const int fy = panel_y + panel_h - foot_h;
    const int fx = panel_x + panel_w - pad - tw - 8 - kFace;
    p.setPen(QColor(180, 190, 205));
    p.drawText(QRect(panel_x + pad, fy, std::max(0, fx - panel_x - pad - 8), foot_h),
               Qt::AlignVCenter | Qt::AlignLeft, QString::number(std::max(0, seconds_left)));
    paint_face_button(p, QRect(fx, fy + (foot_h - kFace) / 2, kFace, kFace), false);
    p.setPen(QColor(230, 236, 245));
    p.drawText(QRect(fx + kFace + 6, fy, tw + 4, foot_h), Qt::AlignVCenter | Qt::AlignLeft, close);
  }

  p.end();
  return img;
}

}  // namespace vrp
