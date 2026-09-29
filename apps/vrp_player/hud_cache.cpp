#include "hud_cache.hpp"

#include <QHash>
#include <QPainter>
#include <QFontMetrics>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace vrp {
namespace {

uint64_t mix_key(uint64_t h, uint64_t v) {
  h ^= v;
  h *= 1099511628211ull;
  return h;
}

uint64_t domain_key(uint64_t domain, uint64_t key) {
  return key ^ (domain * 0x9E3779B97F4A7C15ull);
}

uint64_t text_key(const QString& s, const QFont& font, const QColor& color) {
  uint64_t h = 14695981039346656037ull;
  h = mix_key(h, static_cast<uint64_t>(font.pointSize()));
  h = mix_key(h, static_cast<uint64_t>(font.pixelSize()));
  h = mix_key(h, static_cast<uint64_t>(font.weight()));
  h = mix_key(h, font.bold() ? 1u : 0u);
  h = mix_key(h, color.rgba());
  h = mix_key(h, static_cast<uint64_t>(qHash(font.family())));
  for (QChar ch : s) h = mix_key(h, ch.unicode());
  return h;
}

}  // namespace

HudCanvas::HudCanvas(int atlas_w, int atlas_h)
    : atlas_w_(atlas_w),
      atlas_h_(atlas_h),
      pixels_(static_cast<size_t>(atlas_w) * static_cast<size_t>(atlas_h) * 4, 0) {}

void HudCanvas::begin(int panel_w, int panel_h) {
  panel_w_ = panel_w;
  panel_h_ = panel_h;
  verts_.clear();
  if (!overflow_) return;
  overflow_ = false;
  if (atlas_w_ < 4096) {
    atlas_w_ *= 2;
  } else if (atlas_h_ < 4096) {
    atlas_h_ *= 2;
  } else {
    return;
  }
  pixels_.assign(static_cast<size_t>(atlas_w_) * static_cast<size_t>(atlas_h_) * 4, 0);
  sprites_.clear();
  texts_.clear();
  pen_x_ = 1;
  pen_y_ = 1;
  row_h_ = 0;
  ++gen_;
}

HudPaintResult HudCanvas::take() {
  HudPaintResult result;
  result.atlas_w = atlas_w_;
  result.atlas_h = atlas_h_;
  result.atlas_gen = gen_;
  result.atlas = pixels_.data();
  result.atlas_bytes = pixels_.size();
  result.verts = std::move(verts_);
  return result;
}

bool HudCanvas::allocate(int w, int h, int& x, int& y) {
  constexpr int kPad = 1;
  if (w <= 0 || h <= 0) return false;
  if (w + kPad * 2 > atlas_w_ || h + kPad * 2 > atlas_h_) return false;
  if (pen_x_ + w + kPad > atlas_w_) {
    pen_x_ = kPad;
    pen_y_ += row_h_ + kPad;
    row_h_ = 0;
  }
  if (pen_y_ + h + kPad > atlas_h_) return false;
  x = pen_x_;
  y = pen_y_;
  pen_x_ += w + kPad;
  if (h > row_h_) row_h_ = h;
  return true;
}

HudCanvas::Sprite HudCanvas::pack_image(const QImage& src) {
  Sprite sprite;
  const QImage img =
      src.format() == QImage::Format_RGBA8888 ? src : src.convertToFormat(QImage::Format_RGBA8888);
  if (img.isNull() || img.width() <= 0 || img.height() <= 0) return sprite;
  int x = 0;
  int y = 0;
  if (!allocate(img.width(), img.height(), x, y)) {
    overflow_ = true;
    return sprite;
  }
  for (int row = 0; row < img.height(); ++row) {
    std::memcpy(pixels_.data() + (static_cast<size_t>(y + row) * static_cast<size_t>(atlas_w_) +
                                  static_cast<size_t>(x)) *
                                     4,
                img.constScanLine(row), static_cast<size_t>(img.width()) * 4);
  }
  ++gen_;
  sprite.w = img.width();
  sprite.h = img.height();
  sprite.u0 = static_cast<float>(x) / static_cast<float>(atlas_w_);
  sprite.v0 = static_cast<float>(y) / static_cast<float>(atlas_h_);
  sprite.u1 = static_cast<float>(x + img.width()) / static_cast<float>(atlas_w_);
  sprite.v1 = static_cast<float>(y + img.height()) / static_cast<float>(atlas_h_);
  return sprite;
}

HudCanvas::Sprite HudCanvas::solid(const QColor& color) {
  const uint64_t key = domain_key(1, color.rgba());
  const auto found = sprites_.find(key);
  if (found != sprites_.end()) return found->second;
  QImage img(1, 1, QImage::Format_RGBA8888);
  img.fill(color);
  Sprite sprite = pack_image(img);
  if (sprite.w <= 0) return sprite;
  sprite.u0 = sprite.u1 = (sprite.u0 + sprite.u1) * 0.5f;
  sprite.v0 = sprite.v1 = (sprite.v0 + sprite.v1) * 0.5f;
  sprites_.emplace(key, sprite);
  return sprite;
}

void HudCanvas::push_quad(float x, float y, float w, float h, float u0, float v0, float u1, float v1) {
  constexpr uint32_t kMaxVerts = 2048 * 6;
  if (w <= 0.f || h <= 0.f || panel_w_ <= 0 || panel_h_ <= 0) return;
  if (verts_.size() + 6 > kMaxVerts) return;
  constexpr float kMeshHalfW = 16.f / 9.f;
  const float pw = static_cast<float>(panel_w_);
  const float ph = static_cast<float>(panel_h_);
  const auto px = [&](float pixel_x) { return ((pixel_x / pw) - 0.5f) * 2.f * kMeshHalfW; };
  const auto py = [&](float pixel_y) { return (0.5f - (pixel_y / ph)) * 2.f; };
  const float x0 = px(x);
  const float x1 = px(x + w);
  const float y0 = py(y + h);
  const float y1 = py(y);
  const HudVertex tri[6] = {
      {x0, y0, 0.f, u0, v1}, {x1, y0, 0.f, u1, v1}, {x1, y1, 0.f, u1, v0},
      {x0, y0, 0.f, u0, v1}, {x1, y1, 0.f, u1, v0}, {x0, y1, 0.f, u0, v0},
  };
  verts_.insert(verts_.end(), tri, tri + 6);
}

void HudCanvas::fill(const QRect& r, const QColor& color) {
  if (r.width() <= 0 || r.height() <= 0 || color.alpha() == 0) return;
  const Sprite sprite = solid(color);
  if (sprite.w <= 0) return;
  push_quad(static_cast<float>(r.x()), static_cast<float>(r.y()), static_cast<float>(r.width()),
            static_cast<float>(r.height()), sprite.u0, sprite.v0, sprite.u1, sprite.v1);
}

void HudCanvas::rounded(const QRect& r, const QColor& color, int radius) {
  if (r.width() <= 0 || r.height() <= 0 || color.alpha() == 0) return;
  if (radius <= 0) {
    fill(r, color);
    return;
  }
  const int rad = std::min(radius, std::min(r.width(), r.height()) / 2);
  if (rad <= 0) {
    fill(r, color);
    return;
  }
  const int src_w = std::min(r.width(), rad * 2 + 2);
  const int src_h = r.height();
  if (src_h + 2 >= atlas_h_) {
    fill(r, color);
    return;
  }
  uint64_t key = 14695981039346656037ull;
  key = mix_key(key, static_cast<uint64_t>(src_w));
  key = mix_key(key, static_cast<uint64_t>(src_h));
  key = mix_key(key, static_cast<uint64_t>(rad));
  key = mix_key(key, color.rgba());
  key = domain_key(2, key);
  auto found = sprites_.find(key);
  if (found == sprites_.end()) {
    QImage img(src_w, src_h, QImage::Format_RGBA8888);
    img.fill(Qt::transparent);
    QPainter painter(&img);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawRoundedRect(QRectF(0, 0, src_w, src_h), rad, rad);
    painter.end();
    const Sprite packed = pack_image(img);
    if (packed.w <= 0) return;
    found = sprites_.emplace(key, packed).first;
  }
  const Sprite sprite = found->second;
  if (r.width() <= src_w) {
    push_quad(static_cast<float>(r.x()), static_cast<float>(r.y()), static_cast<float>(r.width()),
              static_cast<float>(r.height()), sprite.u0, sprite.v0, sprite.u1, sprite.v1);
    return;
  }
  const int cap = std::min(rad + 1, std::max(1, sprite.w / 2));
  const float du = (sprite.u1 - sprite.u0) / static_cast<float>(std::max(sprite.w, 1));
  const float mid_u = (sprite.u0 + sprite.u1) * 0.5f;
  push_quad(static_cast<float>(r.x()), static_cast<float>(r.y()), static_cast<float>(cap),
            static_cast<float>(r.height()), sprite.u0, sprite.v0, sprite.u0 + du * static_cast<float>(cap),
            sprite.v1);
  push_quad(static_cast<float>(r.x() + cap), static_cast<float>(r.y()),
            static_cast<float>(r.width() - cap * 2), static_cast<float>(r.height()), mid_u, sprite.v0, mid_u,
            sprite.v1);
  push_quad(static_cast<float>(r.x() + r.width() - cap), static_cast<float>(r.y()), static_cast<float>(cap),
            static_cast<float>(r.height()), sprite.u1 - du * static_cast<float>(cap), sprite.v0, sprite.u1,
            sprite.v1);
}

void HudCanvas::blit_clipped(const QRect& dest, const Sprite& sprite, const QRect& clip) {
  if (dest.width() <= 0 || dest.height() <= 0 || sprite.w <= 0) return;
  const int x0 = std::max(dest.x(), clip.x());
  const int y0 = std::max(dest.y(), clip.y());
  const int x1 = std::min(dest.x() + dest.width(), clip.x() + clip.width());
  const int y1 = std::min(dest.y() + dest.height(), clip.y() + clip.height());
  if (x1 <= x0 || y1 <= y0) return;
  const float u0 = sprite.u0 + (sprite.u1 - sprite.u0) * static_cast<float>(x0 - dest.x()) /
                                   static_cast<float>(dest.width());
  const float u1 = sprite.u0 + (sprite.u1 - sprite.u0) * static_cast<float>(x1 - dest.x()) /
                                   static_cast<float>(dest.width());
  const float v0 = sprite.v0 + (sprite.v1 - sprite.v0) * static_cast<float>(y0 - dest.y()) /
                                   static_cast<float>(dest.height());
  const float v1 = sprite.v0 + (sprite.v1 - sprite.v0) * static_cast<float>(y1 - dest.y()) /
                                   static_cast<float>(dest.height());
  push_quad(static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(x1 - x0),
            static_cast<float>(y1 - y0), u0, v0, u1, v1);
}

HudCanvas::TextSprite HudCanvas::make_text(const QString& s, const QFont& font, const QColor& color) {
  TextSprite text;
  const QFontMetrics metrics(font);
  text.width = metrics.horizontalAdvance(s);
  text.height = std::max(1, metrics.height());
  text.pad = 1;
  if (s.isEmpty() || text.width <= 0) return {};
  const int max_w = std::max(8, atlas_w_ - 4);
  for (int origin = 0; origin < text.width; origin += max_w) {
    const int slice_w = std::min(max_w, text.width - origin);
    QImage img(slice_w + text.pad * 2, text.height + text.pad * 2, QImage::Format_RGBA8888);
    img.fill(Qt::transparent);
    QPainter painter(&img);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setFont(font);
    painter.setPen(color);
    painter.drawText(QRect(text.pad - origin, text.pad, text.width, text.height),
                     Qt::AlignLeft | Qt::AlignVCenter, s);
    painter.end();
    const Sprite sprite = pack_image(img);
    if (sprite.w <= 0) return {};
    text.slices.push_back(sprite);
  }
  return text;
}

void HudCanvas::blit_text(int x, int y, const TextSprite& text, const QRect& clip) {
  const int max_w = std::max(8, atlas_w_ - 4);
  int cursor = 0;
  for (const Sprite& slice : text.slices) {
    const int content_w = std::min(max_w, text.width - cursor);
    if (content_w <= 0) break;
    const QRect dest(x + cursor - text.pad, y - text.pad, content_w + text.pad * 2,
                     text.height + text.pad * 2);
    blit_clipped(dest, slice, clip);
    cursor += content_w;
  }
}

void HudCanvas::text(const QRect& r, const QString& s, const QFont& font, const QColor& color, int flags) {
  if (s.isEmpty() || r.width() <= 0 || r.height() <= 0 || color.alpha() == 0) return;
  if (flags & Qt::TextWordWrap) {
    uint64_t key = text_key(s, font, color);
    key = mix_key(key, static_cast<uint64_t>(flags));
    key = mix_key(key, static_cast<uint64_t>(r.width()));
    key = mix_key(key, static_cast<uint64_t>(r.height()));
    paint_cached(key, r, r.width(), r.height(), [&](QPainter& painter) {
      painter.setFont(font);
      painter.setPen(color);
      painter.drawText(QRect(0, 0, r.width(), r.height()), flags, s);
    });
    return;
  }
  const uint64_t key = text_key(s, font, color);
  auto found = texts_.find(key);
  if (found == texts_.end()) {
    TextSprite raster = make_text(s, font, color);
    if (raster.slices.empty()) return;
    found = texts_.emplace(key, std::move(raster)).first;
  }
  const TextSprite& raster = found->second;
  int x = r.x();
  if (flags & Qt::AlignHCenter) x = r.x() + (r.width() - raster.width) / 2;
  else if (flags & Qt::AlignRight) x = r.x() + r.width() - raster.width;
  int y = r.y();
  if (flags & Qt::AlignBottom) y = r.y() + r.height() - raster.height;
  else if (!(flags & Qt::AlignTop)) y = r.y() + (r.height() - raster.height) / 2;
  blit_text(x, y, raster, r);
}

void HudCanvas::scrolling_text(const QRect& r, const QString& s, const QFont& font, const QColor& color) {
  if (s.isEmpty() || r.width() <= 0 || r.height() <= 0) return;
  const QFontMetrics metrics(font);
  const int text_w = metrics.horizontalAdvance(s);
  if (text_w <= r.width()) {
    text(r, s, font, color, Qt::AlignVCenter | Qt::AlignLeft);
    return;
  }
  using clock = std::chrono::steady_clock;
  static clock::time_point t0 = clock::now();
  const double sec = std::chrono::duration<double>(clock::now() - t0).count();
  const int travel = text_w - r.width() + 8;
  const double cycle = 2.5 + travel / 40.0;
  const double phase = std::fmod(sec, cycle * 2.0);
  double u = phase / cycle;
  if (u > 1.0) u = 2.0 - u;
  u = u * u * (3.0 - 2.0 * u);
  const int xoff = static_cast<int>(std::lround(u * travel));

  const uint64_t key = text_key(s, font, color);
  auto found = texts_.find(key);
  if (found == texts_.end()) {
    TextSprite raster = make_text(s, font, color);
    if (raster.slices.empty()) return;
    found = texts_.emplace(key, std::move(raster)).first;
  }
  const int y = r.y() + (r.height() - found->second.height) / 2;
  blit_text(r.x() - xoff, y, found->second, r);
}

void HudCanvas::paint_cached(uint64_t key, const QRect& dest, int raster_w, int raster_h,
                             const std::function<void(QPainter&)>& draw) {
  if (!draw || dest.width() <= 0 || dest.height() <= 0 || raster_w <= 0 || raster_h <= 0) return;
  const uint64_t stored = domain_key(3, key);
  auto found = sprites_.find(stored);
  if (found == sprites_.end()) {
    QImage img(raster_w, raster_h, QImage::Format_RGBA8888);
    img.fill(Qt::transparent);
    {
      QPainter painter(&img);
      painter.setRenderHint(QPainter::Antialiasing, true);
      painter.setRenderHint(QPainter::TextAntialiasing, true);
      painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
      draw(painter);
    }
    const Sprite sprite = pack_image(img);
    if (sprite.w <= 0) return;
    found = sprites_.emplace(stored, sprite).first;
  }
  const Sprite sprite = found->second;
  push_quad(static_cast<float>(dest.x()), static_cast<float>(dest.y()), static_cast<float>(dest.width()),
            static_cast<float>(dest.height()), sprite.u0, sprite.v0, sprite.u1, sprite.v1);
}

HudCanvas& shared_hud_canvas() {
  static HudCanvas canvas(2048, 2048);
  return canvas;
}

}  // namespace vrp
