#include "starfield/starfield.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace vrp::starfield {

std::vector<Star> load_catalog(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  uint32_t magic = 0;
  uint32_t count = 0;
  in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
  in.read(reinterpret_cast<char*>(&count), sizeof(count));
  if (!in || magic != kCatalogMagic || count == 0 || count > 500000u) return {};
  std::vector<Star> stars(count);
  in.read(reinterpret_cast<char*>(stars.data()), static_cast<std::streamsize>(count * sizeof(Star)));
  if (!in) return {};
  return stars;
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

}  // namespace vrp::starfield
