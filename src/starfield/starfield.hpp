#pragma once

/**
 * Reusable all-sky starfield (no Qt / Vulkan / OpenXR).
 *
 * 1. Load `stars_wikidata.bin` (Wikidata / CC0) via load_catalog()
 * 2. Build camera-facing soft-disc quads with build_mesh()
 * 3. Draw with the bundled shaders/star.{vert,frag} (additive blend)
 *
 * Coordinate system: Y up, RA=0 toward −Z (same as monaSphere video sphere).
 */

#include <cstdint>
#include <string>
#include <vector>

namespace vrp::starfield {

inline constexpr uint32_t kCatalogMagic = 0x31525453u;  // 'STR1'

struct Star {
  float x, y, z;  // unit direction
  float mag;      // visual magnitude
  float r, g, b;  // sRGB chromaticity 0..1
};
static_assert(sizeof(Star) == 28, "star catalog binary layout");

/** Interleaved vertex for soft circular billboards (2 triangles / star). */
struct Vertex {
  float x, y, z;
  float r, g, b;
  float u, v;  // −1..1 in the disc quad
};
static_assert(sizeof(Vertex) == 32, "star mesh vertex layout");

struct MeshOptions {
  float radius = 95.f;
  float mag_pivot = 3.f;       // magnitude at which intensity ≈ 1 before clamp
  float mag_contrast = 0.85f;  // Pogson exponent scale (higher = more contrast)
  float intensity_min = 0.02f;
  float intensity_max = 2.f;
  float ang_min_deg = 0.008f;
  float ang_scale_deg = 0.032f;
};

/** Load catalog. Empty on failure (missing file, bad magic, etc.). */
std::vector<Star> load_catalog(const std::string& path);

/** Build triangle list: 6 vertices per star (two triangles). */
std::vector<Vertex> build_mesh(const std::vector<Star>& stars, const MeshOptions& opt = {});

}  // namespace vrp::starfield
