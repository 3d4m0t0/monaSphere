#pragma once

#include "ui/hud_mesh.hpp"

#include <QColor>
#include <QFont>
#include <QImage>
#include <QRect>
#include <QString>

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

class QPainter;

namespace vrp {

/** Atlas pixels plus the quads that sample them. Atlas bytes stay valid until the next paint. */
struct HudPaintResult {
  int atlas_w = 0;
  int atlas_h = 0;
  uint64_t atlas_gen = 0;
  const uint8_t* atlas = nullptr;
  size_t atlas_bytes = 0;
  std::vector<HudVertex> verts;
};

/**
 * CPU-rasterized HUD pieces kept in one atlas.
 * Fills, text, and icons are uploaded once; later frames only move quads.
 */
class HudCanvas {
 public:
  HudCanvas(int atlas_w, int atlas_h);

  void begin(int panel_w, int panel_h);
  HudPaintResult take();

  void fill(const QRect& r, const QColor& color);
  /** Horizontal 3-slice so a changing width does not rasterize again. */
  void rounded(const QRect& r, const QColor& color, int radius);
  void text(const QRect& r, const QString& s, const QFont& font, const QColor& color, int flags);
  void scrolling_text(const QRect& r, const QString& s, const QFont& font, const QColor& color);
  void paint_cached(uint64_t key, const QRect& dest, int raster_w, int raster_h,
                    const std::function<void(QPainter&)>& draw);

 private:
  struct Sprite {
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    int w = 0, h = 0;
  };
  struct TextSprite {
    int width = 0;
    int height = 0;
    int pad = 1;
    std::vector<Sprite> slices;
  };

  Sprite pack_image(const QImage& src);
  Sprite solid(const QColor& color);
  TextSprite make_text(const QString& s, const QFont& font, const QColor& color);
  bool allocate(int w, int h, int& x, int& y);
  void push_quad(float x, float y, float w, float h, float u0, float v0, float u1, float v1);
  void blit_clipped(const QRect& dest, const Sprite& sprite, const QRect& clip);
  void blit_text(int x, int y, const TextSprite& text, const QRect& clip);

  int atlas_w_ = 0;
  int atlas_h_ = 0;
  int panel_w_ = 1280;
  int panel_h_ = 720;
  int pen_x_ = 1;
  int pen_y_ = 1;
  int row_h_ = 0;
  uint64_t gen_ = 1;
  bool overflow_ = false;
  std::vector<uint8_t> pixels_;
  std::unordered_map<uint64_t, Sprite> sprites_;
  std::unordered_map<uint64_t, TextSprite> texts_;
  std::vector<HudVertex> verts_;
};

HudCanvas& shared_hud_canvas();

}  // namespace vrp
