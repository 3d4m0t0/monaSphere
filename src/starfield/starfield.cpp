#include "starfield/starfield.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace vrp::starfield {
namespace {

std::vector<Star> load_magic_catalog(const std::string& path, uint32_t expect_magic) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  uint32_t magic = 0;
  uint32_t count = 0;
  in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
  in.read(reinterpret_cast<char*>(&count), sizeof(count));
  if (!in || magic != expect_magic || count == 0 || count > 500000u) return {};
  std::vector<Star> stars(count);
  in.read(reinterpret_cast<char*>(stars.data()), static_cast<std::streamsize>(count * sizeof(Star)));
  if (!in) return {};
  return stars;
}

}  // namespace

std::vector<Star> load_catalog(const std::string& path) {
  return load_magic_catalog(path, kCatalogMagic);
}

std::vector<Star> load_deepsky_catalog(const std::string& path) {
  return load_magic_catalog(path, kDeepskyMagic);
}

std::vector<Vertex> build_mesh(const std::vector<Star>& stars, const MeshOptions& opt) {
  std::vector<Vertex> verts;
  verts.reserve(stars.size() * 6);
  const float radius = opt.radius;
  for (const Star& s : stars) {
    const float px = s.x * radius;
    const float py = s.y * radius;
    const float pz = s.z * radius;
    const float flux = std::pow(2.512f, (opt.mag_pivot - s.mag) * opt.mag_contrast);
    const float intensity = std::clamp(flux, opt.intensity_min, opt.intensity_max);
    const float ang_deg = opt.ang_min_deg + opt.ang_scale_deg * std::min(intensity, 1.4f);
    const float half = radius * std::tan(ang_deg * static_cast<float>(M_PI) / 180.f);

    float nx = -s.x, ny = -s.y, nz = -s.z;
    float tx = 0.f, ty = 1.f, tz = 0.f;
    if (std::fabs(ny) > 0.9f) {
      tx = 1.f;
      ty = 0.f;
    }
    float sx = ty * nz - tz * ny;
    float sy = tz * nx - tx * nz;
    float sz = tx * ny - ty * nx;
    float sl = std::sqrt(sx * sx + sy * sy + sz * sz);
    if (sl < 1e-5f) continue;
    sx /= sl;
    sy /= sl;
    sz /= sl;
    float bx = ny * sz - nz * sy;
    float by = nz * sx - nx * sz;
    float bz = nx * sy - ny * sx;

    const float cr = s.r * intensity;
    const float cg = s.g * intensity;
    const float cb = s.b * intensity;
    const Vertex q[4] = {
        {px - sx * half - bx * half, py - sy * half - by * half, pz - sz * half - bz * half, cr, cg, cb, -1.f, -1.f},
        {px + sx * half - bx * half, py + sy * half - by * half, pz + sz * half - bz * half, cr, cg, cb, 1.f, -1.f},
        {px + sx * half + bx * half, py + sy * half + by * half, pz + sz * half + bz * half, cr, cg, cb, 1.f, 1.f},
        {px - sx * half + bx * half, py - sy * half + by * half, pz - sz * half + bz * half, cr, cg, cb, -1.f, 1.f},
    };
    const int idx[6] = {0, 1, 2, 0, 2, 3};
    for (int i : idx) verts.push_back(q[i]);
  }
  return verts;
}

float galactic_latitude_deg(float x, float y, float z) {
  // Recover equatorial angles from app-space unit direction.
  const float len = std::sqrt(x * x + y * y + z * z);
  if (len < 1e-8f) return 0.f;
  x /= len;
  y /= len;
  z /= len;
  const float dec = std::asin(std::clamp(y, -1.f, 1.f));
  const float ra = std::atan2(x, -z);

  // J2000 north galactic pole.
  constexpr float kNgPRa = 192.85948f * static_cast<float>(M_PI) / 180.f;
  constexpr float kNgPDec = 27.12825f * static_cast<float>(M_PI) / 180.f;
  const float sin_b = std::sin(dec) * std::sin(kNgPDec) +
                      std::cos(dec) * std::cos(kNgPDec) * std::cos(ra - kNgPRa);
  return std::asin(std::clamp(sin_b, -1.f, 1.f)) * 180.f / static_cast<float>(M_PI);
}

std::pair<std::vector<SkyVertex>, std::vector<uint32_t>> build_sky_sphere(float radius, int segments,
                                                                         int rings) {
  segments = std::max(8, segments);
  rings = std::max(4, rings);
  std::vector<SkyVertex> verts;
  std::vector<uint32_t> indices;
  verts.reserve(static_cast<size_t>((rings + 1) * (segments + 1)));
  for (int i = 0; i <= rings; ++i) {
    const float v = static_cast<float>(i) / static_cast<float>(rings);
    const float phi = v * static_cast<float>(M_PI);  // 0..π
    const float sp = std::sin(phi);
    const float y = std::cos(phi) * radius;
    for (int j = 0; j <= segments; ++j) {
      const float u = static_cast<float>(j) / static_cast<float>(segments);
      const float theta = u * static_cast<float>(2.0 * M_PI);
      // Match video sphere convention: u=0.5 → −Z.
      const float x = std::sin(theta) * sp * radius;
      const float z = -std::cos(theta) * sp * radius;
      verts.push_back({x, y, z});
    }
  }
  for (int i = 0; i < rings; ++i) {
    for (int j = 0; j < segments; ++j) {
      const uint32_t a = static_cast<uint32_t>(i * (segments + 1) + j);
      const uint32_t b = a + static_cast<uint32_t>(segments + 1);
      // Inward-facing (viewed from origin).
      indices.push_back(a);
      indices.push_back(b);
      indices.push_back(a + 1);
      indices.push_back(b);
      indices.push_back(b + 1);
      indices.push_back(a + 1);
    }
  }
  return {std::move(verts), std::move(indices)};
}

namespace {

constexpr double kPi = M_PI;
constexpr double kDeg = kPi / 180.0;
constexpr double kRad = 180.0 / kPi;

double wrap360(double d) {
  d = std::fmod(d, 360.0);
  if (d < 0.0) d += 360.0;
  return d;
}

double wrap180(double d) {
  d = std::fmod(d + 180.0, 360.0);
  if (d < 0.0) d += 360.0;
  return d - 180.0;
}

void ecliptic_to_app(double lon_deg, double lat_deg, double obl_deg, float& x, float& y, float& z) {
  const double lon = lon_deg * kDeg;
  const double lat = lat_deg * kDeg;
  const double obl = obl_deg * kDeg;
  // Ecliptic → equatorial.
  const double ra = std::atan2(std::sin(lon) * std::cos(obl) - std::tan(lat) * std::sin(obl), std::cos(lon));
  const double dec =
      std::asin(std::sin(lat) * std::cos(obl) + std::cos(lat) * std::sin(obl) * std::sin(lon));
  const double cdec = std::cos(dec);
  // App space: Y up, RA=0 toward −Z.
  x = static_cast<float>(std::sin(ra) * cdec);
  y = static_cast<float>(std::sin(dec));
  z = static_cast<float>(-std::cos(ra) * cdec);
}

}  // namespace

void earth_sky_matrix(std::int64_t unix_utc_sec, float lat_deg, float lon_deg, float out_m4[16]) {
  const double jd = static_cast<double>(unix_utc_sec) / 86400.0 + 2440587.5;
  const double D = jd - 2451545.0;
  // Greenwich mean sidereal time (degrees), then local.
  const double gmst = wrap360(280.46061837 + 360.98564736629 * D);
  const double lst = wrap360(gmst + static_cast<double>(lon_deg));
  const double L = lst * kDeg;
  const double P = static_cast<double>(lat_deg) * kDeg;
  const double cL = std::cos(L);
  const double sL = std::sin(L);
  const double sP = std::sin(P);
  const double cP = std::cos(P);

  // Equatorial app (Y up, RA=0 → −Z) → local horizon app (Y up, −Z = north).
  // Hour angle H = LST − RA; local up matches sin(alt) = sinδ sinφ + cosδ cosφ cos H.
  // Column-major 3×3 in the upper-left of out_m4.
  auto set_col = [&](int c, double x, double y, double z) {
    out_m4[c * 4 + 0] = static_cast<float>(x);
    out_m4[c * 4 + 1] = static_cast<float>(y);
    out_m4[c * 4 + 2] = static_cast<float>(z);
    out_m4[c * 4 + 3] = 0.f;
  };
  set_col(0, -cL, sL * cP, sL * sP);
  set_col(1, 0.0, sP, -cP);
  set_col(2, -sL, -cL * cP, -cL * sP);
  out_m4[12] = out_m4[13] = out_m4[14] = 0.f;
  out_m4[3] = out_m4[7] = out_m4[11] = 0.f;
  out_m4[15] = 1.f;
}

MoonState moon_at_unix(std::int64_t unix_utc_sec) {
  // Julian Day / centuries from J2000.0 (Meeus-style low-precision).
  const double jd = static_cast<double>(unix_utc_sec) / 86400.0 + 2440587.5;
  const double T = (jd - 2451545.0) / 36525.0;
  const double T2 = T * T;

  // Sun mean anomaly / ecliptic longitude (approx).
  const double Ms = wrap360(357.5291 + 35999.0503 * T - 0.0001559 * T2);
  const double Ls = wrap360(280.4665 + 36000.7698 * T + 0.0003032 * T2);
  const double Cs = (1.9146 - 0.004817 * T) * std::sin(Ms * kDeg) + 0.019993 * std::sin(2.0 * Ms * kDeg);
  const double lon_sun = wrap360(Ls + Cs);

  // Moon mean elements (degrees).
  const double Lp = wrap360(218.3165 + 481267.8813 * T - 0.0016 * T2);
  const double D = wrap360(297.8502 + 445267.1115 * T - 0.0016300 * T2);
  const double M = wrap360(134.9634 + 477198.8676 * T + 0.0089970 * T2);
  const double F = wrap360(93.2720 + 483202.0175 * T - 0.0036825 * T2);
  const double Mm = Ms;  // sun anomaly already computed

  // Periodic terms (truncated ELP / Meeus subset), arcminutes → degrees.
  auto s = [](double a) { return std::sin(a * kDeg); };
  double lon = Lp
             + (6.289 * s(M) + 1.274 * s(2.0 * D - M) + 0.658 * s(2.0 * D) + 0.214 * s(2.0 * M)
                - 0.186 * s(Mm) - 0.114 * s(2.0 * F) + 0.059 * s(2.0 * D - 2.0 * M)
                + 0.057 * s(2.0 * D - Mm - M) + 0.053 * s(2.0 * D + M) + 0.046 * s(2.0 * D - Mm)
                + 0.041 * s(M - Mm) - 0.035 * s(D) - 0.031 * s(M + Mm));
  double lat = 5.128 * s(F) + 0.281 * s(M + F) + 0.278 * s(M - F) + 0.173 * s(2.0 * D - F)
             + 0.055 * s(2.0 * D + F - M) + 0.046 * s(2.0 * D - F - M) + 0.033 * s(2.0 * D + F)
             + 0.017 * s(2.0 * M + F);
  lon = wrap360(lon);

  const double obl = 23.439291 - 0.0130042 * T;
  MoonState out;
  ecliptic_to_app(lon, lat, obl, out.x, out.y, out.z);

  float sx, sy, sz;
  ecliptic_to_app(lon_sun, 0.0, obl, sx, sy, sz);
  out.sun_x = sx;
  out.sun_y = sy;
  out.sun_z = sz;

  // Phase from elongation (sun–moon angle).
  const double elong = std::acos(std::clamp(
      static_cast<double>(out.x * sx + out.y * sy + out.z * sz), -1.0, 1.0));
  out.phase = static_cast<float>(0.5 * (1.0 - std::cos(elong)));
  out.ang_diam_deg = 0.5182f;  // mean; good enough for backdrop
  (void)wrap180;
  (void)kRad;
  return out;
}

}  // namespace vrp::starfield
