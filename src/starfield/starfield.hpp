#pragma once

/**
 * Reusable all-sky starfield (no Qt / Vulkan / OpenXR).
 *
 * 1. Load catalogs via load_catalog() / load_deepsky_catalog()
 * 2. Build soft-disc quads with build_mesh()
 * 3. Optional sky sphere mesh: build_sky_sphere() (app samples Gaia equirect)
 *
 * Catalog / ephemeris space: Y up, RA=0 toward −Z.
 * Use earth_sky_matrix() for an Earth observer (−Z = north, LST rotation).
 */

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace vrp::starfield {

inline constexpr uint32_t kCatalogMagic = 0x31525453u;  // 'STR1'
inline constexpr uint32_t kDeepskyMagic = 0x314B5344u;  // 'DSK1'

struct Star {
  float x, y, z;  // unit direction
  float mag;      // visual magnitude (or brightness proxy)
  float r, g, b;  // sRGB chromaticity 0..1
};
static_assert(sizeof(Star) == 28, "star catalog binary layout");

/** Interleaved vertex for soft circular billboards (2 triangles / object). */
struct Vertex {
  float x, y, z;
  float r, g, b;
  float u, v;  // −1..1 in the disc quad
};
static_assert(sizeof(Vertex) == 32, "star mesh vertex layout");

/** Position-only vertex for the galactic-plane glow sphere. */
struct SkyVertex {
  float x, y, z;
};
static_assert(sizeof(SkyVertex) == 12, "sky sphere vertex layout");

struct MeshOptions {
  float radius = 95.f;
  float mag_pivot = 3.f;
  float mag_contrast = 0.85f;
  float intensity_min = 0.02f;
  float intensity_max = 2.f;
  float ang_min_deg = 0.008f;
  float ang_scale_deg = 0.032f;
};

/** Defaults tuned for nebulae (compact discs; core glow is in the shader). */
inline MeshOptions nebula_mesh_options() {
  MeshOptions o;
  o.radius = 94.f;
  o.mag_pivot = 7.5f;
  o.mag_contrast = 0.50f;
  o.intensity_min = 0.40f;
  o.intensity_max = 0.90f;
  o.ang_min_deg = 0.12f;
  o.ang_scale_deg = 0.22f;
  return o;
}

/** @deprecated Use nebula_mesh_options(). */
inline MeshOptions deepsky_mesh_options() { return nebula_mesh_options(); }

std::vector<Star> load_catalog(const std::string& path);
std::vector<Star> load_deepsky_catalog(const std::string& path);

std::vector<Vertex> build_mesh(const std::vector<Star>& stars, const MeshOptions& opt = {});

/**
 * Inward-facing sphere for the galactic sky backdrop (Gaia equirect in the app).
 * Returns (vertices, triangle indices). Radius should sit just outside stars.
 */
std::pair<std::vector<SkyVertex>, std::vector<uint32_t>> build_sky_sphere(float radius = 98.f,
                                                                         int segments = 64,
                                                                         int rings = 32);

/** Galactic latitude in degrees from an equatorial unit direction (app space). */
float galactic_latitude_deg(float x, float y, float z);

/** Moon apparent direction / phase from Unix UTC seconds (host clock). */
struct MoonState {
  float x = 0.f, y = 0.f, z = -1.f;  // unit direction (app space)
  float phase = 0.5f;                 // illuminated fraction 0..1
  float sun_x = 1.f, sun_y = 0.f, sun_z = 0.f;  // unit sun dir (for limb shading)
  float ang_diam_deg = 0.52f;
};

/** Low-precision ephemeris (~1°) suitable for a sky backdrop. */
MoonState moon_at_unix(std::int64_t unix_utc_sec);

/**
 * Equatorial app-space → local horizon app-space (column-major 4×4, no translation).
 *
 * - Directions are geocentric (Earth radius ignored — parallel rays).
 * - Y = local up (sin altitude), −Z = true north, +X = east
 *   (looking north: east is to the right — equinox sunrise at +X).
 * - NCP lands at (0, sin lat, −cos lat); altitude matches the HA formula
 *   sin(alt) = sinδ sinφ + cosδ cosφ cos(LST − RA).
 * - Local sidereal time from lon + UTC rotates the sky once per sidereal day.
 */
void earth_sky_matrix(std::int64_t unix_utc_sec, float lat_deg, float lon_deg, float out_m4[16]);

}  // namespace vrp::starfield
