#include "hud_paint.hpp"

#include "options.hpp"

#include <QPainter>
#include <QFont>
#include <QFontMetrics>
#include <QPainterPath>
#include <QImage>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace vrp {
namespace {

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

void draw_chip(QPainter& p, const QRect& r, const QString& text, bool selected, bool editing = false) {
  draw_flat_square(p, r, selected, editing);
  p.setPen(selected || editing ? QColor(20, 28, 40) : QColor(245, 248, 252));
  p.drawText(r, Qt::AlignCenter, text);
}

QString fsr_label(FsrMode m) {
  switch (m) {
    case FsrMode::Off: return QStringLiteral("FSR切");
    case FsrMode::UltraQuality: return QStringLiteral("FSR UQ");
    case FsrMode::Quality: return QStringLiteral("FSR Q");
    case FsrMode::Performance: return QStringLiteral("FSR P");
  }
  return QStringLiteral("FSR");
}

QString proj_label(ProjectionMode m) {
  switch (m) {
    case ProjectionMode::Flat: return QStringLiteral("FLAT");
    case ProjectionMode::Deg180: return QStringLiteral("180");
    case ProjectionMode::Deg360: return QStringLiteral("360");
  }
  return QStringLiteral("?");
}

QString stereo_label(StereoLayout s) {
  switch (s) {
    case StereoLayout::Mono: return QStringLiteral("MONO");
    case StereoLayout::Sbs: return QStringLiteral("SBS");
    case StereoLayout::OverUnder: return QStringLiteral("TB");
  }
  return QStringLiteral("?");
}

void paint_playback_controls(QPainter& p, const VrMenu::Snapshot& snap, int width, int height,
                             const QFont& body_font, const QFont& hint_font) {
  const bool has_info = !snap.info_video_codec.empty() || !snap.info_res.empty() ||
                        !snap.info_audio_codec.empty();
  const int info_h = has_info ? 72 : 0;
  const int panel_h = 156 + info_h;
  const int top = height - panel_h;

  const int btn = 48;
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
  const QString fname = snap.media_name.empty() ? QString::fromStdString(snap.title)
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
  draw_chip(p, vol_r, QStringLiteral("音量%1%").arg(vol_pct), sel == 6,
            snap.controls_edit == VrMenu::ControlsEdit::Volume);
  x += vol_w + gap;

  const QRect fmt_r(x, y, fmt_w, chip_h);
  const QString fmt =
      proj_label(snap.projection) + QStringLiteral("/") + stereo_label(snap.stereo);
  draw_chip(p, fmt_r, fmt, sel == 7, snap.controls_edit == VrMenu::ControlsEdit::FormatPick);
  x += fmt_w + gap;

  const QRect fsr_r(x, y, fsr_w, chip_h);
  draw_chip(p, fsr_r, fsr_label(snap.fsr), sel == 8,
            snap.controls_edit == VrMenu::ControlsEdit::FsrPick);
  x += fsr_w + gap;

  const QRect hz_r(x, y, hz_w, chip_h);
  QString hz_chip;
  if (snap.preferred_hz > 0) hz_chip = QStringLiteral("%1Hz").arg(snap.preferred_hz);
  else if (snap.display_hz > 0.5f) hz_chip = QStringLiteral("%1Hz").arg(snap.display_hz, 0, 'f', 0);
  else hz_chip = QStringLiteral("自動");
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
    const QString msg = QStringLiteral("リフレッシュレート変更には\n再起動が必要です");
    p.drawText(box.adjusted(10, 8, -10, -40), Qt::AlignTop | Qt::AlignHCenter | Qt::TextWordWrap,
               msg);
    const QRect btn_r(box.center().x() - 48, box.bottom() - 36, 96, 28);
    p.setBrush(QColor(40, 120, 200, 230));
    p.drawRoundedRect(btn_r, 4, 4);
    p.setPen(QColor(255, 255, 255));
    p.drawText(btn_r, Qt::AlignCenter, QStringLiteral("[再起動]"));
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
          default: label = QStringLiteral("自動"); break;
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

  p.setPen(QColor(140, 160, 180));
  p.setFont(hint_font);
  p.drawText(QRect(panel_x + pad, height - 24, panel_w - pad * 2, 18),
             Qt::AlignVCenter | Qt::AlignLeft, QString::fromStdString(snap.hint));
}

}  // namespace

QImage paint_vr_menu(const VrMenu::Snapshot& snap, int width, int height) {
  QImage img(width, height, QImage::Format_RGBA8888);
  img.fill(QColor(12, 16, 24, 230));

  if (!snap.visible && !snap.controls_visible) {
    img.fill(Qt::transparent);
    return img;
  }

  QPainter p(&img);
  p.setRenderHint(QPainter::Antialiasing, true);
  p.setRenderHint(QPainter::TextAntialiasing, true);

  QFont title_font(QStringLiteral("Noto Sans CJK JP"), 22, QFont::DemiBold);
  if (!title_font.exactMatch()) title_font = QFont(QStringLiteral("Sans Serif"), 22, QFont::DemiBold);
  QFont body_font(title_font);
  body_font.setPointSize(16);
  body_font.setWeight(QFont::Normal);
  QFont hint_font(title_font);
  hint_font.setPointSize(13);

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
             QString::fromStdString(snap.title));

  p.setFont(hint_font);
  p.setPen(QColor(140, 160, 180));
  p.drawText(QRect(side + 16, height - 36, content_w - 32, 28), Qt::AlignVCenter | Qt::AlignLeft,
             QString::fromStdString(snap.hint));

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
    const int max_rows = std::max(8, (height - list_top - 44) / row_h);
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
    const char* labels[] = {"投影: Flat", "投影: 180°", "投影: 360°",
                            "立体: Mono", "立体: SBS", "立体: TB (上下)"};
    for (int i = 0; i < 6; ++i) {
      bool on = false;
      if (i == 0) on = snap.projection == ProjectionMode::Flat;
      if (i == 1) on = snap.projection == ProjectionMode::Deg180;
      if (i == 2) on = snap.projection == ProjectionMode::Deg360;
      if (i == 3) on = snap.stereo == StereoLayout::Mono;
      if (i == 4) on = snap.stereo == StereoLayout::Sbs;
      if (i == 5) on = snap.stereo == StereoLayout::OverUnder;
      QString t = QString::fromUtf8(labels[i]);
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

}  // namespace vrp
