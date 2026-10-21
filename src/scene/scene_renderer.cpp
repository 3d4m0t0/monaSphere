#include "scene/scene_renderer.hpp"
#include "starfield/starfield.hpp"
#include "common.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#if defined(VRP_HAS_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace vrp {
namespace {

struct Mat4 {
  float m[16]{};
  static Mat4 identity() {
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.f;
    return r;
  }
  static Mat4 mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
      for (int row = 0; row < 4; ++row)
        r.m[c * 4 + row] = a.m[0 * 4 + row] * b.m[c * 4 + 0] + a.m[1 * 4 + row] * b.m[c * 4 + 1] +
                           a.m[2 * 4 + row] * b.m[c * 4 + 2] + a.m[3 * 4 + row] * b.m[c * 4 + 3];
    return r;
  }
};

Mat4 pose_to_matrix(const XrPosef& pose) {
  const auto& q = pose.orientation;
  const auto& p = pose.position;
  const float x2 = q.x + q.x, y2 = q.y + q.y, z2 = q.z + q.z;
  const float xx = q.x * x2, yy = q.y * y2, zz = q.z * z2;
  const float xy = q.x * y2, xz = q.x * z2, yz = q.y * z2;
  const float wx = q.w * x2, wy = q.w * y2, wz = q.w * z2;
  Mat4 r = Mat4::identity();
  r.m[0] = 1.f - (yy + zz);
  r.m[1] = xy + wz;
  r.m[2] = xz - wy;
  r.m[4] = xy - wz;
  r.m[5] = 1.f - (xx + zz);
  r.m[6] = yz + wx;
  r.m[8] = xz + wy;
  r.m[9] = yz - wx;
  r.m[10] = 1.f - (xx + yy);
  r.m[12] = p.x;
  r.m[13] = p.y;
  r.m[14] = p.z;
  return r;
}

Mat4 invert_rigid(const Mat4& m) {
  Mat4 r = Mat4::identity();
  // transpose rotation
  r.m[0] = m.m[0];
  r.m[1] = m.m[4];
  r.m[2] = m.m[8];
  r.m[4] = m.m[1];
  r.m[5] = m.m[5];
  r.m[6] = m.m[9];
  r.m[8] = m.m[2];
  r.m[9] = m.m[6];
  r.m[10] = m.m[10];
  // translation
  const float tx = m.m[12], ty = m.m[13], tz = m.m[14];
  r.m[12] = -(r.m[0] * tx + r.m[4] * ty + r.m[8] * tz);
  r.m[13] = -(r.m[1] * tx + r.m[5] * ty + r.m[9] * tz);
  r.m[14] = -(r.m[2] * tx + r.m[6] * ty + r.m[10] * tz);
  return r;
}

Mat4 projection_fov(const XrFovf& fov, float near_z, float far_z) {
  // OpenXR FOV → Vulkan clip space (Y down, Z in [0, 1]).
  // Matching Khronos xr_linear.h GRAPHICS_VULKAN.
  const float l = std::tan(fov.angleLeft);
  const float r = std::tan(fov.angleRight);
  const float u = std::tan(fov.angleUp);
  const float d = std::tan(fov.angleDown);
  const float w = r - l;
  const float h = u - d;
  Mat4 m{};
  m.m[0] = 2.f / w;
  m.m[5] = -2.f / h;  // Vulkan: invert Y vs OpenGL
  m.m[8] = (r + l) / w;
  m.m[9] = (u + d) / h;
  m.m[10] = far_z / (near_z - far_z);
  m.m[11] = -1.f;
  m.m[14] = (far_z * near_z) / (near_z - far_z);
  return m;
}

struct PushConstants {
  Mat4 mvp;
  int32_t viewIndex;
  int32_t stereoMode;
  int32_t is180;
  int32_t nv12;
  int32_t project;  // 1 = thumbnail samples a 90° forward window
};

Mat4 rot_x(float a) {
  Mat4 r = Mat4::identity();
  const float c = std::cos(a);
  const float s = std::sin(a);
  r.m[5] = c;
  r.m[6] = s;
  r.m[9] = -s;
  r.m[10] = c;
  return r;
}

Mat4 rot_y(float a) {
  Mat4 r = Mat4::identity();
  const float c = std::cos(a);
  const float s = std::sin(a);
  r.m[0] = c;
  r.m[2] = -s;
  r.m[8] = s;
  r.m[10] = c;
  return r;
}

std::vector<uint32_t> read_spv(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) vrp_fatal("Cannot open shader: " + path);
  auto size = f.tellg();
  f.seekg(0);
  std::vector<uint32_t> data(static_cast<size_t>(size) / 4);
  f.read(reinterpret_cast<char*>(data.data()), size);
  return data;
}

struct GridVert {
  float x, y, z;
  float r, g, b;
};

struct GridCol {
  float r, g, b;
};

void add_grid_seg(std::vector<GridVert>& out, float ax, float ay, float az, float bx, float by, float bz,
                  GridCol col, float half_w) {
  const float dx = bx - ax;
  const float dy = by - ay;
  const float dz = bz - az;
  const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
  if (len < 1e-5f) return;
  const float inv = 1.f / len;
  const float dirx = dx * inv;
  const float diry = dy * inv;
  const float dirz = dz * inv;
  float mx = ax + bx;
  float my = ay + by;
  float mz = az + bz;
  const float ml = std::sqrt(mx * mx + my * my + mz * mz);
  if (ml < 1e-5f) return;
  mx /= ml;
  my /= ml;
  mz /= ml;
  float sx = diry * mz - dirz * my;
  float sy = dirz * mx - dirx * mz;
  float sz = dirx * my - diry * mx;
  const float sl = std::sqrt(sx * sx + sy * sy + sz * sz);
  if (sl < 1e-5f) return;
  const float s = half_w / sl;
  sx *= s;
  sy *= s;
  sz *= s;
  const GridVert q[4] = {
      {ax - sx, ay - sy, az - sz, col.r, col.g, col.b},
      {ax + sx, ay + sy, az + sz, col.r, col.g, col.b},
      {bx + sx, by + sy, bz + sz, col.r, col.g, col.b},
      {bx - sx, by - sy, bz - sz, col.r, col.g, col.b},
  };
  const int idx[6] = {0, 1, 2, 0, 2, 3};
  for (int i : idx) out.push_back(q[i]);
}

void sphere_point(float lat_deg, float lon_deg, float radius, float& x, float& y, float& z) {
  const float lat = lat_deg * static_cast<float>(M_PI) / 180.f;
  const float lon = lon_deg * static_cast<float>(M_PI) / 180.f;
  const float cl = std::cos(lat);
  x = std::sin(lon) * cl * radius;
  y = std::sin(lat) * radius;
  z = -std::cos(lon) * cl * radius;
}

using StarVert = starfield::Vertex;

std::vector<GridVert> build_comfort_grid() {
  // Y up. Longitude 0 looks down -Z, matching the video sphere.
  // ±90° are the poles: every meridian ends there, so those latitudes are the meeting points.
  constexpr float kRadius = 80.f;
  constexpr float kStep = 3.f;
  // Thin ribbons (was 0.18°); equator stays a bit stronger for orientation.
  const float half_w = kRadius * std::tan(0.07f * static_cast<float>(M_PI) / 180.f);
  const float equator_w = half_w * 2.2f;
  const GridCol line{0.78f, 0.84f, 0.90f};
  const GridCol equator{1.f, 1.f, 1.f};
  std::vector<GridVert> verts;
  const float lats[] = {90.f, 60.f, 30.f, 0.f, -30.f, -60.f, -90.f};
  for (float lat : lats) {
    if (std::fabs(lat) >= 89.f) continue;
    const GridCol col = lat == 0.f ? equator : line;
    for (float lon = 0.f; lon < 360.f; lon += kStep) {
      float ax, ay, az, bx, by, bz;
      sphere_point(lat, lon, kRadius, ax, ay, az);
      sphere_point(lat, lon + kStep, kRadius, bx, by, bz);
      add_grid_seg(verts, ax, ay, az, bx, by, bz, col, lat == 0.f ? equator_w : half_w);
    }
  }
  for (float lon = 0.f; lon < 360.f; lon += 45.f) {
    for (float lat = -90.f; lat < 90.f; lat += kStep) {
      float ax, ay, az, bx, by, bz;
      sphere_point(lat, lon, kRadius, ax, ay, az);
      sphere_point(lat + kStep, lon, kRadius, bx, by, bz);
      add_grid_seg(verts, ax, ay, az, bx, by, bz, line, half_w);
    }
  }
  return verts;
}

#if defined(VRP_HAS_FFMPEG)
bool load_jpeg_rgba(const std::string& path, std::vector<uint8_t>& rgba, int& w, int& h) {
  rgba.clear();
  w = h = 0;
  AVFormatContext* fmt = nullptr;
  if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return false;
  if (avformat_find_stream_info(fmt, nullptr) < 0) {
    avformat_close_input(&fmt);
    return false;
  }
  int si = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (si < 0) {
    avformat_close_input(&fmt);
    return false;
  }
  const AVCodec* dec = avcodec_find_decoder(fmt->streams[si]->codecpar->codec_id);
  AVCodecContext* ctx = dec ? avcodec_alloc_context3(dec) : nullptr;
  if (!ctx || avcodec_parameters_to_context(ctx, fmt->streams[si]->codecpar) < 0 ||
      avcodec_open2(ctx, dec, nullptr) < 0) {
    if (ctx) avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    return false;
  }
  AVPacket* pkt = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  bool ok = false;
  while (av_read_frame(fmt, pkt) >= 0) {
    if (pkt->stream_index != si) {
      av_packet_unref(pkt);
      continue;
    }
    if (avcodec_send_packet(ctx, pkt) == 0 && avcodec_receive_frame(ctx, frame) == 0) {
      SwsContext* sws =
          sws_getContext(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                         frame->width, frame->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr,
                         nullptr);
      if (sws) {
        w = frame->width;
        h = frame->height;
        const int row = w * 4;
        const int stride = (row + 63) & ~63;
        std::vector<uint8_t> padded(static_cast<size_t>(stride) * static_cast<size_t>(h) + 64);
        uint8_t* dst[4] = {padded.data(), nullptr, nullptr, nullptr};
        int dst_stride[4] = {stride, 0, 0, 0};
        sws_scale(sws, frame->data, frame->linesize, 0, h, dst, dst_stride);
        rgba.resize(static_cast<size_t>(row) * static_cast<size_t>(h));
        for (int y = 0; y < h; ++y) {
          std::memcpy(rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(row),
                      padded.data() + static_cast<size_t>(y) * static_cast<size_t>(stride),
                      static_cast<size_t>(row));
        }
        sws_freeContext(sws);
        ok = true;
      }
      av_packet_unref(pkt);
      break;
    }
    av_packet_unref(pkt);
  }
  av_frame_free(&frame);
  av_packet_free(&pkt);
  avcodec_free_context(&ctx);
  avformat_close_input(&fmt);
  if (!ok) {
    rgba.clear();
    w = h = 0;
  }
  return ok;
}
#else
bool load_jpeg_rgba(const std::string&, std::vector<uint8_t>& rgba, int& w, int& h) {
  rgba.clear();
  w = h = 0;
  return false;
}
#endif

/** Seal ℓ=±180° for REPEAT sampling (no denoise — plate is pre-cleaned). */
void seal_equirect_seam(std::vector<uint8_t>& rgba, int w, int h) {
  if (w < 16 || h < 8 || rgba.size() < static_cast<size_t>(w) * static_cast<size_t>(h) * 4u) return;
  const int row = w * 4;
  constexpr int kSeam = 48;
  std::vector<uint8_t> seam_snap(static_cast<size_t>(kSeam) * 2u * 3u);
  for (int y = 0; y < h; ++y) {
    for (int i = 0; i < kSeam; ++i) {
      const uint8_t* Lp =
          rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(row) + static_cast<size_t>(i) * 4u;
      const uint8_t* Rp = rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(row) +
                          static_cast<size_t>(w - 1 - i) * 4u;
      for (int c = 0; c < 3; ++c) {
        seam_snap[static_cast<size_t>(i) * 6u + static_cast<size_t>(c)] = Lp[c];
        seam_snap[static_cast<size_t>(i) * 6u + 3u + static_cast<size_t>(c)] = Rp[c];
      }
    }
    for (int i = 0; i < kSeam; ++i) {
      const float t = (static_cast<float>(i) + 0.5f) / static_cast<float>(kSeam);
      const float a = std::pow(0.5f * (1.f + std::cos(t * static_cast<float>(M_PI))), 0.65f);
      uint8_t* Lp =
          rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(row) + static_cast<size_t>(i) * 4u;
      uint8_t* Rp = rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(row) +
                    static_cast<size_t>(w - 1 - i) * 4u;
      for (int c = 0; c < 3; ++c) {
        const float L0 = seam_snap[static_cast<size_t>(i) * 6u + static_cast<size_t>(c)];
        const float R0 = seam_snap[static_cast<size_t>(i) * 6u + 3u + static_cast<size_t>(c)];
        const float mid = 0.5f * (L0 + R0);
        Lp[c] = static_cast<uint8_t>(std::clamp(L0 * (1.f - a) + mid * a + 0.5f, 0.f, 255.f));
        Rp[c] = static_cast<uint8_t>(std::clamp(R0 * (1.f - a) + mid * a + 0.5f, 0.f, 255.f));
      }
    }
    for (int i = 0; i < 3; ++i) {
      uint8_t* L = rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(row) +
                   static_cast<size_t>(i) * 4u;
      uint8_t* R = rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(row) +
                   static_cast<size_t>(w - 1 - i) * 4u;
      for (int c = 0; c < 3; ++c) {
        const uint8_t avg = static_cast<uint8_t>((static_cast<int>(L[c]) + static_cast<int>(R[c]) + 1) / 2);
        L[c] = R[c] = avg;
      }
    }
  }
}

}  // namespace

void SceneRenderer::init(XrVulkanApp& app, const std::string& shader_dir) {
  app_ = &app;
  device_ = app.device();
  phys_ = app.physical_device();
  shader_dir_ = shader_dir;
  create_descriptors();
  build_meshes();
  if (!app.swapchains().empty()) {
    create_pipelines(app.swapchains()[0].format);
  }
  cmds_.resize(2);
  VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ai.commandPool = app.command_pool();
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 2;
  VRP_CHECK(vkAllocateCommandBuffers(device_, &ai, cmds_.data()) == VK_SUCCESS, "scene cmds");

  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  VRP_CHECK(vkCreateFence(device_, &fci, nullptr, &submit_fence_) == VK_SUCCESS, "scene fence");

  load_milkyway_sky_texture();
}

void SceneRenderer::shutdown() {
  if (!device_) return;
  vkDeviceWaitIdle(device_);
  if (submit_fence_) {
    vkDestroyFence(device_, submit_fence_, nullptr);
    submit_fence_ = VK_NULL_HANDLE;
  }
  if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
  if (grid_pipeline_) vkDestroyPipeline(device_, grid_pipeline_, nullptr);
  if (star_pipeline_) vkDestroyPipeline(device_, star_pipeline_, nullptr);
  if (nebula_pipeline_) vkDestroyPipeline(device_, nebula_pipeline_, nullptr);
  if (moon_pipeline_) vkDestroyPipeline(device_, moon_pipeline_, nullptr);
  if (sun_pipeline_) vkDestroyPipeline(device_, sun_pipeline_, nullptr);
  if (milkyway_pipeline_) vkDestroyPipeline(device_, milkyway_pipeline_, nullptr);
  if (layout_) vkDestroyPipelineLayout(device_, layout_, nullptr);
  if (grid_layout_) vkDestroyPipelineLayout(device_, grid_layout_, nullptr);
  if (milkyway_layout_) vkDestroyPipelineLayout(device_, milkyway_layout_, nullptr);
  if (moon_layout_) vkDestroyPipelineLayout(device_, moon_layout_, nullptr);
  if (milkyway_pool_) {
    vkDestroyDescriptorPool(device_, milkyway_pool_, nullptr);
    milkyway_pool_ = VK_NULL_HANDLE;
    milkyway_dset_ = VK_NULL_HANDLE;
  }
  if (milkyway_dsl_) {
    vkDestroyDescriptorSetLayout(device_, milkyway_dsl_, nullptr);
    milkyway_dsl_ = VK_NULL_HANDLE;
  }
  if (milkyway_samp_) {
    vkDestroySampler(device_, milkyway_samp_, nullptr);
    milkyway_samp_ = VK_NULL_HANDLE;
  }
  if (milkyway_view_) {
    vkDestroyImageView(device_, milkyway_view_, nullptr);
    milkyway_view_ = VK_NULL_HANDLE;
  }
  if (milkyway_img_) {
    vkDestroyImage(device_, milkyway_img_, nullptr);
    milkyway_img_ = VK_NULL_HANDLE;
  }
  if (milkyway_img_mem_) {
    vkFreeMemory(device_, milkyway_img_mem_, nullptr);
    milkyway_img_mem_ = VK_NULL_HANDLE;
  }
  milkyway_sky_ok_ = false;
  if (vert_) vkDestroyShaderModule(device_, vert_, nullptr);
  if (frag_) vkDestroyShaderModule(device_, frag_, nullptr);
  if (grid_vert_) vkDestroyShaderModule(device_, grid_vert_, nullptr);
  if (grid_frag_) vkDestroyShaderModule(device_, grid_frag_, nullptr);
  if (star_vert_) vkDestroyShaderModule(device_, star_vert_, nullptr);
  if (star_frag_) vkDestroyShaderModule(device_, star_frag_, nullptr);
  if (nebula_vert_) vkDestroyShaderModule(device_, nebula_vert_, nullptr);
  if (nebula_frag_) vkDestroyShaderModule(device_, nebula_frag_, nullptr);
  if (moon_vert_) vkDestroyShaderModule(device_, moon_vert_, nullptr);
  if (moon_frag_) vkDestroyShaderModule(device_, moon_frag_, nullptr);
  if (sun_vert_) vkDestroyShaderModule(device_, sun_vert_, nullptr);
  if (sun_frag_) vkDestroyShaderModule(device_, sun_frag_, nullptr);
  if (milkyway_vert_) vkDestroyShaderModule(device_, milkyway_vert_, nullptr);
  if (milkyway_frag_) vkDestroyShaderModule(device_, milkyway_frag_, nullptr);
  if (dset_) {
    // freed with pool
  }
  if (pool_) vkDestroyDescriptorPool(device_, pool_, nullptr);
  if (dsl_) vkDestroyDescriptorSetLayout(device_, dsl_, nullptr);
  auto destroy_buf = [&](VkBuffer& b, VkDeviceMemory& m) {
    if (b) vkDestroyBuffer(device_, b, nullptr);
    if (m) vkFreeMemory(device_, m, nullptr);
    b = VK_NULL_HANDLE;
    m = VK_NULL_HANDLE;
  };
  if (hud_mapped_) {
    vkUnmapMemory(device_, vbo_hud_mem_);
    hud_mapped_ = nullptr;
  }
  destroy_buf(vbo_quad_, vbo_quad_mem_);
  destroy_buf(vbo_hud_, vbo_hud_mem_);
  destroy_buf(vbo_sphere_, vbo_sphere_mem_);
  destroy_buf(ibo_sphere_, ibo_sphere_mem_);
  destroy_buf(vbo_grid_, vbo_grid_mem_);
  destroy_buf(vbo_stars_, vbo_stars_mem_);
  destroy_buf(vbo_nebulae_, vbo_nebulae_mem_);
  destroy_buf(vbo_moon_, vbo_moon_mem_);
  destroy_buf(vbo_milkyway_, vbo_milkyway_mem_);
  destroy_buf(ibo_milkyway_, ibo_milkyway_mem_);
  pipeline_ = VK_NULL_HANDLE;
  grid_pipeline_ = VK_NULL_HANDLE;
  star_pipeline_ = VK_NULL_HANDLE;
  nebula_pipeline_ = VK_NULL_HANDLE;
  moon_pipeline_ = VK_NULL_HANDLE;
  sun_pipeline_ = VK_NULL_HANDLE;
  milkyway_pipeline_ = VK_NULL_HANDLE;
  layout_ = VK_NULL_HANDLE;
  grid_layout_ = VK_NULL_HANDLE;
  milkyway_layout_ = VK_NULL_HANDLE;
  moon_layout_ = VK_NULL_HANDLE;
}

void SceneRenderer::set_milkyway_gain(float gain_mul) {
  milkyway_gain_mul_ = std::clamp(gain_mul, 0.2f, 2.5f);
}

float SceneRenderer::milkyway_gain() const { return milkyway_gain_mul_; }

void SceneRenderer::set_sky_observer(float lat_deg, float lon_deg) {
  sky_lat_deg_ = std::clamp(lat_deg, -90.f, 90.f);
  sky_lon_deg_ = lon_deg;
  while (sky_lon_deg_ > 180.f) sky_lon_deg_ -= 360.f;
  while (sky_lon_deg_ < -180.f) sky_lon_deg_ += 360.f;
}

void SceneRenderer::load_milkyway_sky_texture() {
  milkyway_sky_ok_ = false;
  if (!device_ || !milkyway_dset_ || !app_) return;
  std::vector<uint8_t> rgba;
  int w = 0, h = 0;
  const std::string path = shader_dir_ + "/gaia_sky_edr3_equirect.jpg";
  if (!load_jpeg_rgba(path, rgba, w, h) || w <= 0 || h <= 0) {
    VRP_LOG("milkyway: Gaia sky texture missing (%s)", path.c_str());
    return;
  }
  // Blend the longitude join before upload (U wraps). No CPU denoise.
  seal_equirect_seam(rgba, w, h);

  const uint32_t uw = static_cast<uint32_t>(w);
  const uint32_t uh = static_cast<uint32_t>(h);
  uint32_t mip_levels = 1;
  {
    uint32_t mw = uw, mh = uh;
    while (mw > 1u || mh > 1u) {
      mw = std::max(1u, mw / 2u);
      mh = std::max(1u, mh / 2u);
      ++mip_levels;
    }
  }

  VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = VK_FORMAT_R8G8B8A8_UNORM;  // JPEG bytes are sRGB; linearize in shader
  ici.extent = {uw, uh, 1};
  ici.mipLevels = mip_levels;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VRP_CHECK(vkCreateImage(device_, &ici, nullptr, &milkyway_img_) == VK_SUCCESS, "mw image");
  VkMemoryRequirements req{};
  vkGetImageMemoryRequirements(device_, milkyway_img_, &req);
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VRP_CHECK(vkAllocateMemory(device_, &mai, nullptr, &milkyway_img_mem_) == VK_SUCCESS, "mw img mem");
  vkBindImageMemory(device_, milkyway_img_, milkyway_img_mem_, 0);

  VkBuffer staging = VK_NULL_HANDLE;
  VkDeviceMemory staging_mem = VK_NULL_HANDLE;
  const VkDeviceSize bytes = static_cast<VkDeviceSize>(rgba.size());
  {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VRP_CHECK(vkCreateBuffer(device_, &bci, nullptr, &staging) == VK_SUCCESS, "mw staging");
    VkMemoryRequirements breq{};
    vkGetBufferMemoryRequirements(device_, staging, &breq);
    VkMemoryAllocateInfo bmai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    bmai.allocationSize = breq.size;
    bmai.memoryTypeIndex =
        find_memory(breq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VRP_CHECK(vkAllocateMemory(device_, &bmai, nullptr, &staging_mem) == VK_SUCCESS, "mw staging mem");
    vkBindBufferMemory(device_, staging, staging_mem, 0);
    void* mapped = nullptr;
    vkMapMemory(device_, staging_mem, 0, bytes, 0, &mapped);
    std::memcpy(mapped, rgba.data(), rgba.size());
    vkUnmapMemory(device_, staging_mem);
  }

  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = app_->command_pool();
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = 1;
  VRP_CHECK(vkAllocateCommandBuffers(device_, &cai, &cmd) == VK_SUCCESS, "mw cmd");
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &bi);
  VkImageMemoryBarrier to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  to_dst.srcAccessMask = 0;
  to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  to_dst.image = milkyway_img_;
  to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mip_levels, 0, 1};
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &to_dst);
  VkBufferImageCopy copy{};
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = {uw, uh, 1};
  vkCmdCopyBufferToImage(cmd, staging, milkyway_img_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

  // mip0: TRANSFER_DST → TRANSFER_SRC for blit chain.
  {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.image = milkyway_img_;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
  }

  uint32_t src_w = uw, src_h = uh;
  for (uint32_t level = 1; level < mip_levels; ++level) {
    const uint32_t dst_w = std::max(1u, src_w / 2u);
    const uint32_t dst_h = std::max(1u, src_h / 2u);
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
    blit.srcOffsets[0] = {0, 0, 0};
    blit.srcOffsets[1] = {static_cast<int32_t>(src_w), static_cast<int32_t>(src_h), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
    blit.dstOffsets[0] = {0, 0, 0};
    blit.dstOffsets[1] = {static_cast<int32_t>(dst_w), static_cast<int32_t>(dst_h), 1};
    vkCmdBlitImage(cmd, milkyway_img_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, milkyway_img_,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    // Promote this level to SRC for the next blit (last level goes to SAMPLE below).
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.image = milkyway_img_;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    src_w = dst_w;
    src_h = dst_h;
  }

  // All mips are TRANSFER_SRC → SHADER_READ_ONLY.
  {
    VkImageMemoryBarrier to_sample{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_sample.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    to_sample.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    to_sample.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_sample.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_sample.image = milkyway_img_;
    to_sample.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mip_levels, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &to_sample);
  }
  vkEndCommandBuffer(cmd);
  {
    std::lock_guard<std::mutex> qlock(app_->queue_mutex());
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VRP_CHECK(vkQueueSubmit(app_->queue(), 1, &si, VK_NULL_HANDLE) == VK_SUCCESS, "mw upload");
    vkQueueWaitIdle(app_->queue());
  }
  vkFreeCommandBuffers(device_, app_->command_pool(), 1, &cmd);
  vkDestroyBuffer(device_, staging, nullptr);
  vkFreeMemory(device_, staging_mem, nullptr);

  VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  ivci.image = milkyway_img_;
  ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  ivci.format = VK_FORMAT_R8G8B8A8_UNORM;
  ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mip_levels, 0, 1};
  VRP_CHECK(vkCreateImageView(device_, &ivci, nullptr, &milkyway_view_) == VK_SUCCESS, "mw view");

  VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sci.magFilter = VK_FILTER_LINEAR;
  sci.minFilter = VK_FILTER_LINEAR;
  sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sci.minLod = 0.f;
  sci.maxLod = static_cast<float>(mip_levels);
  VRP_CHECK(vkCreateSampler(device_, &sci, nullptr, &milkyway_samp_) == VK_SUCCESS, "mw samp");

  VkDescriptorImageInfo ii{};
  ii.sampler = milkyway_samp_;
  ii.imageView = milkyway_view_;
  ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = milkyway_dset_;
  write.dstBinding = 0;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  write.pImageInfo = &ii;
  vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
  milkyway_sky_ok_ = true;
  VRP_LOG("milkyway: Gaia sky %dx%d (%u mips, seam seal)", w, h, mip_levels);
}

void SceneRenderer::set_projection(ProjectionMode mode, StereoLayout stereo, float flat_fov_deg,
                                   float distance) {
  mode_ = mode;
  stereo_ = stereo;
  flat_fov_deg_ = flat_fov_deg;
  distance_ = distance;
}

void SceneRenderer::set_content_aspect(float width_over_height) {
  if (width_over_height > 0.05f && width_over_height < 50.f) {
    content_aspect_ = width_over_height;
  }
}

void SceneRenderer::set_view_offset(float yaw_rad, float pitch_rad) {
  look_yaw_ = yaw_rad;
  look_pitch_ = pitch_rad;
}

void SceneRenderer::set_comfort_grid(bool on) { comfort_grid_ = on; }

void SceneRenderer::set_texture(VkImageView view, VkSampler sampler) {
  if (!view || !sampler || !dset_) return;
  queued_tex_ = QueuedTex::None;
  nv12_ = false;
  nv12_full_range_ = false;
  rgba_gamma_ = false;
  VkDescriptorImageInfo ii[2]{};
  for (int i = 0; i < 2; ++i) {
    ii[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ii[i].imageView = view;
    ii[i].sampler = sampler;
  }
  VkWriteDescriptorSet w[2]{};
  for (int i = 0; i < 2; ++i) {
    w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[i].dstSet = dset_;
    w[i].dstBinding = static_cast<uint32_t>(i);
    w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[i].descriptorCount = 1;
    w[i].pImageInfo = &ii[i];
  }
  vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
}

void SceneRenderer::set_texture_gamma(VkImageView view, VkSampler sampler) {
  set_texture(view, sampler);
  rgba_gamma_ = true;
}

void SceneRenderer::set_nv12_texture(VkImageView y, VkImageView uv, VkSampler sampler, bool full_range) {
  if (!y || !uv || !sampler || !dset_) return;
  queued_tex_ = QueuedTex::None;
  nv12_ = true;
  nv12_full_range_ = full_range;
  rgba_gamma_ = false;
  VkDescriptorImageInfo ii[2]{};
  // CUDA LINEAR path keeps images in GENERAL after the one-time transition.
  ii[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  ii[0].imageView = y;
  ii[0].sampler = sampler;
  ii[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  ii[1].imageView = uv;
  ii[1].sampler = sampler;
  VkWriteDescriptorSet w[2]{};
  for (int i = 0; i < 2; ++i) {
    w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[i].dstSet = dset_;
    w[i].dstBinding = static_cast<uint32_t>(i);
    w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[i].descriptorCount = 1;
    w[i].pImageInfo = &ii[i];
  }
  vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
}

void SceneRenderer::queue_texture(VkImageView view, VkSampler sampler) {
  if (!view || !sampler) return;
  queued_tex_ = QueuedTex::Rgba;
  queued_y_ = view;
  queued_uv_ = view;
  queued_samp_ = sampler;
  queued_nv12_full_ = false;
}

void SceneRenderer::queue_texture_gamma(VkImageView view, VkSampler sampler) {
  if (!view || !sampler) return;
  queued_tex_ = QueuedTex::RgbaGamma;
  queued_y_ = view;
  queued_uv_ = view;
  queued_samp_ = sampler;
  queued_nv12_full_ = false;
}

void SceneRenderer::queue_nv12_texture(VkImageView y, VkImageView uv, VkSampler sampler,
                                       bool full_range) {
  if (!y || !uv || !sampler) return;
  queued_tex_ = QueuedTex::Nv12;
  queued_y_ = y;
  queued_uv_ = uv;
  queued_samp_ = sampler;
  queued_nv12_full_ = full_range;
}

void SceneRenderer::flush_queued_texture() {
  if (queued_tex_ == QueuedTex::None) return;
  const QueuedTex kind = queued_tex_;
  const VkImageView y = queued_y_;
  const VkImageView uv = queued_uv_;
  const VkSampler samp = queued_samp_;
  const bool full = queued_nv12_full_;
  queued_tex_ = QueuedTex::None;
  if (kind == QueuedTex::Nv12) {
    set_nv12_texture(y, uv, samp, full);
  } else if (kind == QueuedTex::RgbaGamma) {
    set_texture_gamma(y, samp);
  } else {
    set_texture(y, samp);
  }
}

void SceneRenderer::set_hud_texture(VkImageView view, VkSampler sampler, bool visible) {
  hud_visible_ = visible && view && sampler && hud_dset_;
  if (!hud_visible_) return;
  VkDescriptorImageInfo ii[2]{};
  for (int i = 0; i < 2; ++i) {
    ii[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ii[i].imageView = view;
    ii[i].sampler = sampler;
  }
  VkWriteDescriptorSet w[2]{};
  for (int i = 0; i < 2; ++i) {
    w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[i].dstSet = hud_dset_;
    w[i].dstBinding = static_cast<uint32_t>(i);
    w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[i].descriptorCount = 1;
    w[i].pImageInfo = &ii[i];
  }
  vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
}

void SceneRenderer::set_thumb_texture(VkImageView view, VkSampler sampler, bool visible) {
  thumb_visible_ = visible && view && sampler && thumb_dset_;
  if (!thumb_visible_) return;
  VkDescriptorImageInfo ii[2]{};
  for (int i = 0; i < 2; ++i) {
    ii[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ii[i].imageView = view;
    ii[i].sampler = sampler;
  }
  VkWriteDescriptorSet w[2]{};
  for (int i = 0; i < 2; ++i) {
    w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[i].dstSet = thumb_dset_;
    w[i].dstBinding = static_cast<uint32_t>(i);
    w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[i].descriptorCount = 1;
    w[i].pImageInfo = &ii[i];
  }
  vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
}

void SceneRenderer::set_thumb_format(ProjectionMode mode, StereoLayout stereo) {
  thumb_mode_ = mode;
  thumb_stereo_ = stereo;
}

void SceneRenderer::set_thumb_pose(float world_half_w, float pixel_h_over_w, float x_m, float y_m,
                                   float distance_m) {
  constexpr float kMeshHalfW = 16.f / 9.f;
  if (world_half_w > 0.02f) {
    thumb_scale_x_ = world_half_w / kMeshHalfW;
    thumb_scale_y_ = world_half_w * std::max(0.2f, pixel_h_over_w);
  }
  thumb_x_ = x_m;
  thumb_y_ = y_m;
  if (distance_m > 0.3f) thumb_distance_ = distance_m;
}

void SceneRenderer::set_hud_mesh(const HudVertex* vertices, uint32_t count) {
  if (!vertices || count < 6) {
    hud_pending_.clear();
    hud_pending_dirty_ = true;
    return;
  }
  if (count > hud_vert_cap_) count = hud_vert_cap_;
  hud_pending_.assign(vertices, vertices + count);
  hud_pending_dirty_ = true;
}

void SceneRenderer::set_hud_layout(float half_width_m, float distance_m, float aspect_h_over_w,
                                   float y_offset_m) {
  if (half_width_m > 0.05f) hud_half_w_ = half_width_m;
  if (distance_m > 0.3f) hud_distance_ = distance_m;
  if (aspect_h_over_w > 0.2f) hud_aspect_ = aspect_h_over_w;
  hud_y_ = y_offset_m;
}

bool SceneRenderer::wait_previous_submit(uint64_t timeout_ns) {
  if (!submit_fence_ || !device_) return true;
  const VkResult r = vkWaitForFences(device_, 1, &submit_fence_, VK_TRUE, timeout_ns);
  return r == VK_SUCCESS;
}

uint32_t SceneRenderer::find_memory(uint32_t bits, VkMemoryPropertyFlags flags) const {
  VkPhysicalDeviceMemoryProperties mp{};
  vkGetPhysicalDeviceMemoryProperties(phys_, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags) == flags) return i;
  }
  vrp_fatal("memory type");
}

VkShaderModule SceneRenderer::load_shader(const std::string& path) {
  auto code = read_spv(path);
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = code.size() * 4;
  ci.pCode = code.data();
  VkShaderModule mod = VK_NULL_HANDLE;
  VRP_CHECK(vkCreateShaderModule(device_, &ci, nullptr, &mod) == VK_SUCCESS, path);
  return mod;
}

void SceneRenderer::create_descriptors() {
  VkDescriptorSetLayoutBinding b[2]{};
  for (int i = 0; i < 2; ++i) {
    b[i].binding = static_cast<uint32_t>(i);
    b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[i].descriptorCount = 1;
    b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  lci.bindingCount = 2;
  lci.pBindings = b;
  VRP_CHECK(vkCreateDescriptorSetLayout(device_, &lci, nullptr, &dsl_) == VK_SUCCESS, "dsl");

  VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16};
  VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pci.poolSizeCount = 1;
  pci.pPoolSizes = &ps;
  pci.maxSets = 4;
  VRP_CHECK(vkCreateDescriptorPool(device_, &pci, nullptr, &pool_) == VK_SUCCESS, "dpool");

  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = pool_;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &dsl_;
  VRP_CHECK(vkAllocateDescriptorSets(device_, &ai, &dset_) == VK_SUCCESS, "dset");
  VRP_CHECK(vkAllocateDescriptorSets(device_, &ai, &hud_dset_) == VK_SUCCESS, "hud dset");
  VRP_CHECK(vkAllocateDescriptorSets(device_, &ai, &thumb_dset_) == VK_SUCCESS, "thumb dset");

  // Milky Way Gaia equirect texture (bound after load_milkyway_sky_texture).
  {
    VkDescriptorSetLayoutBinding samp{};
    samp.binding = 0;
    samp.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samp.descriptorCount = 1;
    samp.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo mw_lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    mw_lci.bindingCount = 1;
    mw_lci.pBindings = &samp;
    VRP_CHECK(vkCreateDescriptorSetLayout(device_, &mw_lci, nullptr, &milkyway_dsl_) == VK_SUCCESS,
              "mw dsl");

    VkDescriptorPoolSize mw_ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo mw_pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    mw_pci.poolSizeCount = 1;
    mw_pci.pPoolSizes = &mw_ps;
    mw_pci.maxSets = 1;
    VRP_CHECK(vkCreateDescriptorPool(device_, &mw_pci, nullptr, &milkyway_pool_) == VK_SUCCESS,
              "mw dpool");

    VkDescriptorSetAllocateInfo mw_ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    mw_ai.descriptorPool = milkyway_pool_;
    mw_ai.descriptorSetCount = 1;
    mw_ai.pSetLayouts = &milkyway_dsl_;
    VRP_CHECK(vkAllocateDescriptorSets(device_, &mw_ai, &milkyway_dset_) == VK_SUCCESS, "mw dset");
  }
}

void SceneRenderer::build_meshes() {
  // Quad in XY, facing -Z (OpenXR forward). +X = right.
  const float aspect = 16.f / 9.f;
  const float h = 1.0f;
  const float w = h * aspect;
  Vertex quad[4] = {
      {-w, -h, 0, 0, 1},
      {w, -h, 0, 1, 1},
      {w, h, 0, 1, 0},
      {-w, h, 0, 0, 0},
  };
  // two triangles as triangle list with duplicated verts via indexless draw of 6
  Vertex quad_tris[6] = {quad[0], quad[1], quad[2], quad[0], quad[2], quad[3]};

  auto upload = [&](const void* data, VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buf,
                    VkDeviceMemory& mem) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage;
    VRP_CHECK(vkCreateBuffer(device_, &bci, nullptr, &buf) == VK_SUCCESS, "mesh buffer");
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_, buf, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex =
        find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VRP_CHECK(vkAllocateMemory(device_, &mai, nullptr, &mem) == VK_SUCCESS, "mesh mem");
    vkBindBufferMemory(device_, buf, mem, 0);
    void* mapped = nullptr;
    vkMapMemory(device_, mem, 0, size, 0, &mapped);
    std::memcpy(mapped, data, static_cast<size_t>(size));
    vkUnmapMemory(device_, mem);
  };

  upload(quad_tris, sizeof(quad_tris), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vbo_quad_, vbo_quad_mem_);

  // HUD sprites. Host-visible so a marquee or seek bar can move without a new texture.
  constexpr uint32_t kHudQuads = 2048;
  hud_vert_cap_ = kHudQuads * 6;
  {
    const VkDeviceSize size = static_cast<VkDeviceSize>(hud_vert_cap_) * sizeof(HudVertex);
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    VRP_CHECK(vkCreateBuffer(device_, &bci, nullptr, &vbo_hud_) == VK_SUCCESS, "hud mesh");
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_, vbo_hud_, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex =
        find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VRP_CHECK(vkAllocateMemory(device_, &mai, nullptr, &vbo_hud_mem_) == VK_SUCCESS, "hud mesh mem");
    vkBindBufferMemory(device_, vbo_hud_, vbo_hud_mem_, 0);
    VRP_CHECK(vkMapMemory(device_, vbo_hud_mem_, 0, size, 0, &hud_mapped_) == VK_SUCCESS, "hud mesh map");
  }

  // Equirect sphere: Y-up, -Z forward at u=0.5 (video center in front at session start).
  const int stacks = 48, slices = 96;
  const float radius = 10.f;
  std::vector<Vertex> sphere_v;
  std::vector<uint32_t> sphere_i;
  for (int i = 0; i <= stacks; ++i) {
    float v = static_cast<float>(i) / stacks;
    float phi = v * static_cast<float>(M_PI);  // 0..π (north pole → south)
    for (int j = 0; j <= slices; ++j) {
      float u = static_cast<float>(j) / slices;
      float theta = (u - 0.5f) * static_cast<float>(2.0 * M_PI);  // -π..π, 0 at center
      const float sp = std::sin(phi);
      const float x = radius * std::sin(theta) * sp;   // +X = right
      const float y = radius * std::cos(phi);
      const float z = -radius * std::cos(theta) * sp;  // u=0.5 → -Z forward
      sphere_v.push_back({x, y, z, u, v});
    }
  }
  for (int i = 0; i < stacks; ++i) {
    for (int j = 0; j < slices; ++j) {
      uint32_t a = i * (slices + 1) + j;
      uint32_t b = a + slices + 1;
      // Inward-facing winding (viewed from origin)
      sphere_i.push_back(a);
      sphere_i.push_back(b);
      sphere_i.push_back(a + 1);
      sphere_i.push_back(b);
      sphere_i.push_back(b + 1);
      sphere_i.push_back(a + 1);
    }
  }
  sphere_index_count_ = static_cast<uint32_t>(sphere_i.size());
  upload(sphere_v.data(), sphere_v.size() * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vbo_sphere_,
         vbo_sphere_mem_);
  upload(sphere_i.data(), sphere_i.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, ibo_sphere_,
         ibo_sphere_mem_);

  const std::vector<GridVert> grid = build_comfort_grid();
  grid_vert_count_ = static_cast<uint32_t>(grid.size());
  if (!grid.empty()) {
    upload(grid.data(), grid.size() * sizeof(GridVert), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vbo_grid_, vbo_grid_mem_);
  }

  // Galactic-plane glow sphere (outside stars). Soft band in milkyway.frag.
  {
    auto [mw_v, mw_i] = starfield::build_sky_sphere(98.f, 256, 128);
    milkyway_index_count_ = static_cast<uint32_t>(mw_i.size());
    if (!mw_v.empty() && !mw_i.empty()) {
      upload(mw_v.data(), mw_v.size() * sizeof(starfield::SkyVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
             vbo_milkyway_, vbo_milkyway_mem_);
      upload(mw_i.data(), mw_i.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, ibo_milkyway_,
             ibo_milkyway_mem_);
    }
  }

  // Stars as soft discs; outside the comfort grid.
  starfield::MeshOptions star_opt;
  star_opt.radius = 95.f;
  const auto catalog = starfield::load_catalog(shader_dir_ + "/stars_wikidata.bin");
  const std::vector<StarVert> stars = starfield::build_mesh(catalog, star_opt);
  star_vert_count_ = static_cast<uint32_t>(stars.size());
  if (!stars.empty()) {
    upload(stars.data(), stars.size() * sizeof(StarVert), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vbo_stars_,
           vbo_stars_mem_);
  }

  // Nebulae: larger colored soft discs (behind stars).
  const auto nebulae = starfield::load_deepsky_catalog(shader_dir_ + "/nebulae_wikidata.bin");
  const std::vector<StarVert> neb_mesh = starfield::build_mesh(nebulae, starfield::nebula_mesh_options());
  nebula_vert_count_ = static_cast<uint32_t>(neb_mesh.size());
  if (!neb_mesh.empty()) {
    upload(neb_mesh.data(), neb_mesh.size() * sizeof(StarVert), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vbo_nebulae_,
           vbo_nebulae_mem_);
  }

  // Unit quad corners for the Moon billboard (−1..1).
  const float moon_quad[] = {-1.f, -1.f, 1.f, -1.f, 1.f, 1.f, -1.f, -1.f, 1.f, 1.f, -1.f, 1.f};
  upload(moon_quad, sizeof(moon_quad), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vbo_moon_, vbo_moon_mem_);
}

void SceneRenderer::create_pipelines(VkFormat /*format*/) {
  vert_ = load_shader(shader_dir_ + "/scene.vert.spv");
  frag_ = load_shader(shader_dir_ + "/scene.frag.spv");

  VkPushConstantRange pcr{};
  pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  pcr.size = sizeof(PushConstants);

  VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.setLayoutCount = 1;
  plci.pSetLayouts = &dsl_;
  plci.pushConstantRangeCount = 1;
  plci.pPushConstantRanges = &pcr;
  VRP_CHECK(vkCreatePipelineLayout(device_, &plci, nullptr, &layout_) == VK_SUCCESS, "pipeline layout");

  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vert_;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = frag_;
  stages[1].pName = "main";

  VkVertexInputBindingDescription bind{};
  bind.binding = 0;
  bind.stride = sizeof(Vertex);
  bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  VkVertexInputAttributeDescription attrs[2]{};
  attrs[0].location = 0;
  attrs[0].binding = 0;
  attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
  attrs[0].offset = offsetof(Vertex, x);
  attrs[1].location = 1;
  attrs[1].binding = 0;
  attrs[1].format = VK_FORMAT_R32G32_SFLOAT;
  attrs[1].offset = offsetof(Vertex, u);

  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vi.vertexBindingDescriptionCount = 1;
  vi.pVertexBindingDescriptions = &bind;
  vi.vertexAttributeDescriptionCount = 2;
  vi.pVertexAttributeDescriptions = attrs;

  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = 1;
  vp.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rs.lineWidth = 1.f;

  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineColorBlendAttachmentState blend{};
  blend.blendEnable = VK_TRUE;
  blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blend.colorBlendOp = VK_BLEND_OP_ADD;
  blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blend.alphaBlendOp = VK_BLEND_OP_ADD;
  blend.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1;
  cb.pAttachments = &blend;

  VkDynamicState dyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dyn.dynamicStateCount = 2;
  dyn.pDynamicStates = dyn_states;

  VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gp.stageCount = 2;
  gp.pStages = stages;
  gp.pVertexInputState = &vi;
  gp.pInputAssemblyState = &ia;
  gp.pViewportState = &vp;
  gp.pRasterizationState = &rs;
  gp.pMultisampleState = &ms;
  gp.pColorBlendState = &cb;
  gp.pDynamicState = &dyn;
  gp.layout = layout_;
  gp.renderPass = app_->color_pass();
  gp.subpass = 0;
  VRP_CHECK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline_) == VK_SUCCESS,
            "pipeline");

  grid_vert_ = load_shader(shader_dir_ + "/grid.vert.spv");
  grid_frag_ = load_shader(shader_dir_ + "/grid.frag.spv");
  VkPushConstantRange grid_pcr{};
  grid_pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  grid_pcr.size = sizeof(Mat4);
  VkPipelineLayoutCreateInfo grid_pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  grid_pl.pushConstantRangeCount = 1;
  grid_pl.pPushConstantRanges = &grid_pcr;
  VRP_CHECK(vkCreatePipelineLayout(device_, &grid_pl, nullptr, &grid_layout_) == VK_SUCCESS, "grid layout");

  VkPipelineShaderStageCreateInfo grid_stages[2]{};
  grid_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  grid_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  grid_stages[0].module = grid_vert_;
  grid_stages[0].pName = "main";
  grid_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  grid_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  grid_stages[1].module = grid_frag_;
  grid_stages[1].pName = "main";

  VkVertexInputBindingDescription grid_bind{};
  grid_bind.binding = 0;
  grid_bind.stride = sizeof(GridVert);
  grid_bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  VkVertexInputAttributeDescription grid_attrs[2]{};
  grid_attrs[0].location = 0;
  grid_attrs[0].binding = 0;
  grid_attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
  grid_attrs[0].offset = offsetof(GridVert, x);
  grid_attrs[1].location = 1;
  grid_attrs[1].binding = 0;
  grid_attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
  grid_attrs[1].offset = offsetof(GridVert, r);

  VkPipelineVertexInputStateCreateInfo grid_vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  grid_vi.vertexBindingDescriptionCount = 1;
  grid_vi.pVertexBindingDescriptions = &grid_bind;
  grid_vi.vertexAttributeDescriptionCount = 2;
  grid_vi.pVertexAttributeDescriptions = grid_attrs;

  VkPipelineInputAssemblyStateCreateInfo grid_ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  grid_ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkPipelineRasterizationStateCreateInfo grid_rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  grid_rs.polygonMode = VK_POLYGON_MODE_FILL;
  grid_rs.cullMode = VK_CULL_MODE_NONE;
  grid_rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  grid_rs.lineWidth = 1.f;

  VkPipelineColorBlendAttachmentState grid_blend{};
  grid_blend.blendEnable = VK_TRUE;
  grid_blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  grid_blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  grid_blend.colorBlendOp = VK_BLEND_OP_ADD;
  grid_blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  grid_blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  grid_blend.alphaBlendOp = VK_BLEND_OP_ADD;
  grid_blend.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo grid_cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  grid_cb.attachmentCount = 1;
  grid_cb.pAttachments = &grid_blend;

  VkGraphicsPipelineCreateInfo grid_gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  grid_gp.stageCount = 2;
  grid_gp.pStages = grid_stages;
  grid_gp.pVertexInputState = &grid_vi;
  grid_gp.pInputAssemblyState = &grid_ia;
  grid_gp.pViewportState = &vp;
  grid_gp.pRasterizationState = &grid_rs;
  grid_gp.pMultisampleState = &ms;
  grid_gp.pColorBlendState = &grid_cb;
  grid_gp.pDynamicState = &dyn;
  grid_gp.layout = grid_layout_;
  grid_gp.renderPass = app_->color_pass();
  grid_gp.subpass = 0;
  VRP_CHECK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &grid_gp, nullptr, &grid_pipeline_) == VK_SUCCESS,
            "grid pipeline");

  // Starfield: soft discs with catalog RGB, additive blend, black sky behind video.
  star_vert_ = load_shader(shader_dir_ + "/star.vert.spv");
  star_frag_ = load_shader(shader_dir_ + "/star.frag.spv");
  VkPipelineShaderStageCreateInfo star_stages[2]{};
  star_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  star_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  star_stages[0].module = star_vert_;
  star_stages[0].pName = "main";
  star_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  star_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  star_stages[1].module = star_frag_;
  star_stages[1].pName = "main";

  VkVertexInputBindingDescription star_bind{};
  star_bind.binding = 0;
  star_bind.stride = sizeof(StarVert);
  star_bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  VkVertexInputAttributeDescription star_attrs[3]{};
  star_attrs[0].location = 0;
  star_attrs[0].binding = 0;
  star_attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
  star_attrs[0].offset = offsetof(StarVert, x);
  star_attrs[1].location = 1;
  star_attrs[1].binding = 0;
  star_attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
  star_attrs[1].offset = offsetof(StarVert, r);
  star_attrs[2].location = 2;
  star_attrs[2].binding = 0;
  star_attrs[2].format = VK_FORMAT_R32G32_SFLOAT;
  star_attrs[2].offset = offsetof(StarVert, u);
  VkPipelineVertexInputStateCreateInfo star_vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  star_vi.vertexBindingDescriptionCount = 1;
  star_vi.pVertexBindingDescriptions = &star_bind;
  star_vi.vertexAttributeDescriptionCount = 3;
  star_vi.pVertexAttributeDescriptions = star_attrs;

  VkPipelineColorBlendAttachmentState star_blend{};
  star_blend.blendEnable = VK_TRUE;
  star_blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
  star_blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
  star_blend.colorBlendOp = VK_BLEND_OP_ADD;
  star_blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  star_blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  star_blend.alphaBlendOp = VK_BLEND_OP_ADD;
  star_blend.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo star_cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  star_cb.attachmentCount = 1;
  star_cb.pAttachments = &star_blend;

  VkGraphicsPipelineCreateInfo star_gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  star_gp.stageCount = 2;
  star_gp.pStages = star_stages;
  star_gp.pVertexInputState = &star_vi;
  star_gp.pInputAssemblyState = &grid_ia;
  star_gp.pViewportState = &vp;
  star_gp.pRasterizationState = &grid_rs;
  star_gp.pMultisampleState = &ms;
  star_gp.pColorBlendState = &star_cb;
  star_gp.pDynamicState = &dyn;
  star_gp.layout = grid_layout_;
  star_gp.renderPass = app_->color_pass();
  star_gp.subpass = 0;
  VRP_CHECK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &star_gp, nullptr, &star_pipeline_) == VK_SUCCESS,
            "star pipeline");

  nebula_vert_ = load_shader(shader_dir_ + "/nebula.vert.spv");
  nebula_frag_ = load_shader(shader_dir_ + "/nebula.frag.spv");
  VkPipelineShaderStageCreateInfo neb_stages[2]{};
  neb_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  neb_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  neb_stages[0].module = nebula_vert_;
  neb_stages[0].pName = "main";
  neb_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  neb_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  neb_stages[1].module = nebula_frag_;
  neb_stages[1].pName = "main";
  VkGraphicsPipelineCreateInfo neb_gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  neb_gp.stageCount = 2;
  neb_gp.pStages = neb_stages;
  neb_gp.pVertexInputState = &star_vi;
  neb_gp.pInputAssemblyState = &grid_ia;
  neb_gp.pViewportState = &vp;
  neb_gp.pRasterizationState = &grid_rs;
  neb_gp.pMultisampleState = &ms;
  neb_gp.pColorBlendState = &star_cb;
  neb_gp.pDynamicState = &dyn;
  neb_gp.layout = grid_layout_;
  neb_gp.renderPass = app_->color_pass();
  neb_gp.subpass = 0;
  VRP_CHECK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &neb_gp, nullptr, &nebula_pipeline_) == VK_SUCCESS,
            "nebula pipeline");

  milkyway_vert_ = load_shader(shader_dir_ + "/milkyway.vert.spv");
  milkyway_frag_ = load_shader(shader_dir_ + "/milkyway.frag.spv");
  struct MilkywayPush {
    float mvp[16];
    float gain_mul;
    uint32_t pad[3];
  };
  VkPushConstantRange mw_pcr{};
  mw_pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  mw_pcr.size = sizeof(MilkywayPush);
  VkPipelineLayoutCreateInfo mw_pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  mw_pl.setLayoutCount = 1;
  mw_pl.pSetLayouts = &milkyway_dsl_;
  mw_pl.pushConstantRangeCount = 1;
  mw_pl.pPushConstantRanges = &mw_pcr;
  VRP_CHECK(vkCreatePipelineLayout(device_, &mw_pl, nullptr, &milkyway_layout_) == VK_SUCCESS, "mw layout");

  VkPipelineShaderStageCreateInfo mw_stages[2]{};
  mw_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  mw_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  mw_stages[0].module = milkyway_vert_;
  mw_stages[0].pName = "main";
  mw_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  mw_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  mw_stages[1].module = milkyway_frag_;
  mw_stages[1].pName = "main";

  VkVertexInputBindingDescription mw_bind{};
  mw_bind.binding = 0;
  mw_bind.stride = sizeof(starfield::SkyVertex);
  mw_bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  VkVertexInputAttributeDescription mw_attr{};
  mw_attr.location = 0;
  mw_attr.binding = 0;
  mw_attr.format = VK_FORMAT_R32G32B32_SFLOAT;
  mw_attr.offset = 0;
  VkPipelineVertexInputStateCreateInfo mw_vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  mw_vi.vertexBindingDescriptionCount = 1;
  mw_vi.pVertexBindingDescriptions = &mw_bind;
  mw_vi.vertexAttributeDescriptionCount = 1;
  mw_vi.pVertexAttributeDescriptions = &mw_attr;

  VkGraphicsPipelineCreateInfo mw_gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  mw_gp.stageCount = 2;
  mw_gp.pStages = mw_stages;
  mw_gp.pVertexInputState = &mw_vi;
  mw_gp.pInputAssemblyState = &grid_ia;
  mw_gp.pViewportState = &vp;
  mw_gp.pRasterizationState = &grid_rs;
  mw_gp.pMultisampleState = &ms;
  mw_gp.pColorBlendState = &star_cb;
  mw_gp.pDynamicState = &dyn;
  mw_gp.layout = milkyway_layout_;
  mw_gp.renderPass = app_->color_pass();
  mw_gp.subpass = 0;
  VRP_CHECK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &mw_gp, nullptr, &milkyway_pipeline_) == VK_SUCCESS,
            "milkyway pipeline");

  moon_vert_ = load_shader(shader_dir_ + "/moon.vert.spv");
  moon_frag_ = load_shader(shader_dir_ + "/moon.frag.spv");
  struct MoonPush {
    float mvp[16];
    float moon_dir[4];
    float sun_dir[4];
  };
  VkPushConstantRange moon_pcr{};
  moon_pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  moon_pcr.size = sizeof(MoonPush);
  VkPipelineLayoutCreateInfo moon_plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  moon_plci.pushConstantRangeCount = 1;
  moon_plci.pPushConstantRanges = &moon_pcr;
  VRP_CHECK(vkCreatePipelineLayout(device_, &moon_plci, nullptr, &moon_layout_) == VK_SUCCESS, "moon layout");

  VkPipelineShaderStageCreateInfo moon_stages[2]{};
  moon_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  moon_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  moon_stages[0].module = moon_vert_;
  moon_stages[0].pName = "main";
  moon_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  moon_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  moon_stages[1].module = moon_frag_;
  moon_stages[1].pName = "main";

  VkVertexInputBindingDescription moon_bind{};
  moon_bind.binding = 0;
  moon_bind.stride = sizeof(float) * 2;
  moon_bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  VkVertexInputAttributeDescription moon_attr{};
  moon_attr.location = 0;
  moon_attr.binding = 0;
  moon_attr.format = VK_FORMAT_R32G32_SFLOAT;
  moon_attr.offset = 0;
  VkPipelineVertexInputStateCreateInfo moon_vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  moon_vi.vertexBindingDescriptionCount = 1;
  moon_vi.pVertexBindingDescriptions = &moon_bind;
  moon_vi.vertexAttributeDescriptionCount = 1;
  moon_vi.pVertexAttributeDescriptions = &moon_attr;

  VkPipelineColorBlendAttachmentState moon_blend{};
  moon_blend.blendEnable = VK_TRUE;
  moon_blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  moon_blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  moon_blend.colorBlendOp = VK_BLEND_OP_ADD;
  moon_blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  moon_blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  moon_blend.alphaBlendOp = VK_BLEND_OP_ADD;
  moon_blend.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo moon_cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  moon_cb.attachmentCount = 1;
  moon_cb.pAttachments = &moon_blend;

  VkGraphicsPipelineCreateInfo moon_gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  moon_gp.stageCount = 2;
  moon_gp.pStages = moon_stages;
  moon_gp.pVertexInputState = &moon_vi;
  moon_gp.pInputAssemblyState = &grid_ia;
  moon_gp.pViewportState = &vp;
  moon_gp.pRasterizationState = &grid_rs;
  moon_gp.pMultisampleState = &ms;
  moon_gp.pColorBlendState = &moon_cb;
  moon_gp.pDynamicState = &dyn;
  moon_gp.layout = moon_layout_;
  moon_gp.renderPass = app_->color_pass();
  moon_gp.subpass = 0;
  VRP_CHECK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &moon_gp, nullptr, &moon_pipeline_) == VK_SUCCESS,
            "moon pipeline");

  sun_vert_ = load_shader(shader_dir_ + "/sun.vert.spv");
  sun_frag_ = load_shader(shader_dir_ + "/sun.frag.spv");
  VkPipelineShaderStageCreateInfo sun_stages[2]{};
  sun_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  sun_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  sun_stages[0].module = sun_vert_;
  sun_stages[0].pName = "main";
  sun_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  sun_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  sun_stages[1].module = sun_frag_;
  sun_stages[1].pName = "main";
  VkGraphicsPipelineCreateInfo sun_gp = moon_gp;
  sun_gp.pStages = sun_stages;
  sun_gp.pColorBlendState = &star_cb;  // additive glow
  VRP_CHECK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &sun_gp, nullptr, &sun_pipeline_) == VK_SUCCESS,
            "sun pipeline");
}

void SceneRenderer::draw_view(uint32_t view_index, const XrView& view, ViewSwapchain& sc,
                              uint32_t image_index, VkCommandBuffer cmd) {
  auto& img = sc.images[image_index];

  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &bi);

  VkClearValue clear{};
  clear.color = {{0.f, 0.f, 0.f, 1.f}};
  VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rp.renderPass = app_->color_pass();
  rp.framebuffer = img.framebuffer;
  rp.renderArea.extent = {static_cast<uint32_t>(sc.width), static_cast<uint32_t>(sc.height)};
  rp.clearValueCount = 1;
  rp.pClearValues = &clear;
  vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

  VkViewport viewport{};
  viewport.width = static_cast<float>(sc.width);
  viewport.height = static_cast<float>(sc.height);
  viewport.maxDepth = 1.f;
  VkRect2D scissor{};
  scissor.extent = rp.renderArea.extent;
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  vkCmdSetScissor(cmd, 0, 1, &scissor);

  const Mat4 head = pose_to_matrix(view.pose);
  Mat4 view_mat = invert_rigid(head);
  // Stick look turns the picture around the head. The HUD stays on the real pose.
  Mat4 content_view = view_mat;
  if (look_yaw_ != 0.f || look_pitch_ != 0.f) {
    Mat4 turned = Mat4::mul(Mat4::mul(rot_y(look_yaw_), rot_x(look_pitch_)), head);
    turned.m[12] = head.m[12];
    turned.m[13] = head.m[13];
    turned.m[14] = head.m[14];
    content_view = invert_rigid(turned);
  }
  Mat4 proj = projection_fov(view.fov, 0.05f, 100.f);

  // Celestial background: Earth-horizon frame (LST + latitude), then content look.
  const auto unix_sec =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
  Mat4 sky_rot = Mat4::identity();
  starfield::earth_sky_matrix(static_cast<std::int64_t>(unix_sec), sky_lat_deg_, sky_lon_deg_, sky_rot.m);
  Mat4 sky_trans = Mat4::identity();
  sky_trans.m[12] = head.m[12];
  sky_trans.m[13] = head.m[13];
  sky_trans.m[14] = head.m[14];
  const Mat4 sky_model = Mat4::mul(sky_trans, sky_rot);
  const Mat4 sky_mvp = Mat4::mul(proj, Mat4::mul(content_view, sky_model));
  if (milkyway_sky_ok_ && milkyway_pipeline_ && milkyway_layout_ && milkyway_dset_ && vbo_milkyway_ &&
      ibo_milkyway_ && milkyway_index_count_ >= 3) {
    struct MilkywayPush {
      float mvp[16];
      float gain_mul;
      uint32_t pad[3];
    } mw_pc{};
    std::memcpy(mw_pc.mvp, &sky_mvp, sizeof(mw_pc.mvp));
    mw_pc.gain_mul = milkyway_gain_mul_;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, milkyway_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, milkyway_layout_, 0, 1, &milkyway_dset_, 0,
                            nullptr);
    vkCmdPushConstants(cmd, milkyway_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(mw_pc), &mw_pc);
    VkDeviceSize mw_off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vbo_milkyway_, &mw_off);
    vkCmdBindIndexBuffer(cmd, ibo_milkyway_, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, milkyway_index_count_, 1, 0, 0, 0);
  }
  if (nebula_pipeline_ && vbo_nebulae_ && nebula_vert_count_ >= 6) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, nebula_pipeline_);
    vkCmdPushConstants(cmd, grid_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(sky_mvp), &sky_mvp);
    VkDeviceSize neb_off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vbo_nebulae_, &neb_off);
    vkCmdDraw(cmd, nebula_vert_count_, 1, 0, 0);
  }
  if (star_pipeline_ && vbo_stars_ && star_vert_count_ >= 6) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, star_pipeline_);
    vkCmdPushConstants(cmd, grid_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(sky_mvp), &sky_mvp);
    VkDeviceSize star_off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vbo_stars_, &star_off);
    vkCmdDraw(cmd, star_vert_count_, 1, 0, 0);
  }
  if ((moon_pipeline_ || sun_pipeline_) && moon_layout_ && vbo_moon_) {
    const starfield::MoonState moon = starfield::moon_at_unix(static_cast<std::int64_t>(unix_sec));
    struct MoonPush {
      float mvp[16];
      float body_dir[4];
      float sun_dir[4];
    };

    // Always draw (including below local horizon) so ephemeris / polarity can be checked.
    if (sun_pipeline_) {
      constexpr float kSunAngDeg = 0.53f;
      const float half_ang = 0.5f * kSunAngDeg * 2.8f * static_cast<float>(M_PI) / 180.f;  // room for soft halo
      const float half_size = 93.f * std::tan(half_ang);
      MoonPush sun_pc{};
      std::memcpy(sun_pc.mvp, &sky_mvp, sizeof(sun_pc.mvp));
      sun_pc.body_dir[0] = moon.sun_x;
      sun_pc.body_dir[1] = moon.sun_y;
      sun_pc.body_dir[2] = moon.sun_z;
      sun_pc.body_dir[3] = half_size;
      sun_pc.sun_dir[0] = sun_pc.sun_dir[1] = sun_pc.sun_dir[2] = sun_pc.sun_dir[3] = 0.f;
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sun_pipeline_);
      vkCmdPushConstants(cmd, moon_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(sun_pc), &sun_pc);
      VkDeviceSize sun_off = 0;
      vkCmdBindVertexBuffers(cmd, 0, 1, &vbo_moon_, &sun_off);
      vkCmdDraw(cmd, 6, 1, 0, 0);
    }

    if (moon_pipeline_) {
      const float half_ang = 0.5f * moon.ang_diam_deg * 1.55f * static_cast<float>(M_PI) / 180.f;
      const float half_size = 92.f * std::tan(half_ang);
      MoonPush moon_pc{};
      std::memcpy(moon_pc.mvp, &sky_mvp, sizeof(moon_pc.mvp));
      moon_pc.body_dir[0] = moon.x;
      moon_pc.body_dir[1] = moon.y;
      moon_pc.body_dir[2] = moon.z;
      moon_pc.body_dir[3] = half_size;
      moon_pc.sun_dir[0] = moon.sun_x;
      moon_pc.sun_dir[1] = moon.sun_y;
      moon_pc.sun_dir[2] = moon.sun_z;
      moon_pc.sun_dir[3] = 0.f;
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, moon_pipeline_);
      vkCmdPushConstants(cmd, moon_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(moon_pc), &moon_pc);
      VkDeviceSize moon_off = 0;
      vkCmdBindVertexBuffers(cmd, 0, 1, &vbo_moon_, &moon_off);
      vkCmdDraw(cmd, 6, 1, 0, 0);
    }
  }

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &dset_, 0, nullptr);

  Mat4 model = Mat4::identity();

  if (mode_ == ProjectionMode::Flat) {
    // FOV defines horizontal half-extent; height follows content aspect.
    const float half_w =
        std::tan(flat_fov_deg_ * 0.5f * static_cast<float>(M_PI) / 180.f) * distance_;
    const float aspect = std::max(0.1f, content_aspect_);
    const float half_h = half_w / aspect;
    // Mesh is built as half-size (aspect×1) with aspect=16/9.
    constexpr float mesh_half_w = 16.f / 9.f;
    constexpr float mesh_half_h = 1.f;
    model.m[0] = half_w / mesh_half_w;
    model.m[5] = half_h / mesh_half_h;
    // −Z = true north after facing-north recenter (sky earth_sky_matrix).
    model.m[14] = -distance_;
  }

  PushConstants pc{};
  pc.mvp = Mat4::mul(proj, Mat4::mul(content_view, model));
  pc.viewIndex = static_cast<int32_t>(view_index);
  pc.stereoMode = static_cast<int32_t>(stereo_);
  pc.is180 = (mode_ == ProjectionMode::Deg180) ? 1 : 0;
  if (!nv12_) {
    pc.nv12 = rgba_gamma_ ? 3 : 0;  // 3 = gamma RGBA (FSR) → linearize in shader
  } else {
    pc.nv12 = nv12_full_range_ ? 2 : 1;
  }
  vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                     sizeof(pc), &pc);

  if (mode_ == ProjectionMode::Flat) {
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vbo_quad_, &off);
    vkCmdDraw(cmd, 6, 1, 0, 0);
  } else {
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vbo_sphere_, &off);
    vkCmdBindIndexBuffer(cmd, ibo_sphere_, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, sphere_index_count_, 1, 0, 0, 0);
  }

  // The cage turns with the picture. The eye stays at the center.
  if (comfort_grid_ && grid_pipeline_ && vbo_grid_ && grid_vert_count_ >= 6) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, grid_pipeline_);
    Mat4 grid_model = Mat4::identity();
    grid_model.m[12] = head.m[12];
    grid_model.m[13] = head.m[13];
    grid_model.m[14] = head.m[14];
    const Mat4 grid_mvp = Mat4::mul(proj, Mat4::mul(content_view, grid_model));
    vkCmdPushConstants(cmd, grid_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(grid_mvp), &grid_mvp);
    VkDeviceSize grid_off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vbo_grid_, &grid_off);
    vkCmdDraw(cmd, grid_vert_count_, 1, 0, 0);
    // HUD vertices are a different layout. Leave the picture pipeline bound for them.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
  }

  // Head-locked-ish HUD: fixed in app space in front of origin (after recenter = forward).
  if (hud_visible_ && hud_dset_ && vbo_hud_ && hud_vert_count_ >= 6) {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &hud_dset_, 0, nullptr);
    Mat4 hud_model = Mat4::identity();
    const float half_h = hud_half_w_ * hud_aspect_;
    hud_model.m[0] = hud_half_w_;
    hud_model.m[5] = half_h;
    hud_model.m[13] = hud_y_;
    hud_model.m[14] = -hud_distance_;
    PushConstants hpc{};
    hpc.mvp = Mat4::mul(proj, Mat4::mul(view_mat, hud_model));
    hpc.viewIndex = 0;
    hpc.stereoMode = 0;
    hpc.is180 = 0;
    hpc.nv12 = 0;
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(hpc), &hpc);
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vbo_hud_, &off);
    vkCmdDraw(cmd, hud_vert_count_, 1, 0, 0);
  }

  if (thumb_visible_ && thumb_dset_) {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &thumb_dset_, 0, nullptr);
    // Quad normal is +Z. Turn that face toward this eye.
    const float px = thumb_x_;
    const float py = thumb_y_;
    const float pz = -thumb_distance_;
    float fx = view.pose.position.x - px;
    float fy = view.pose.position.y - py;
    float fz = view.pose.position.z - pz;
    const float flen = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (flen > 1e-4f) {
      fx /= flen;
      fy /= flen;
      fz /= flen;
    } else {
      fx = 0.f;
      fy = 0.f;
      fz = 1.f;
    }
    float rx = fz;
    float ry = 0.f;
    float rz = -fx;
    float rlen = std::sqrt(rx * rx + rz * rz);
    if (rlen < 1e-4f) {
      rx = 1.f;
      ry = 0.f;
      rz = 0.f;
    } else {
      rx /= rlen;
      rz /= rlen;
    }
    const float ux = fy * rz - fz * ry;
    const float uy = fz * rx - fx * rz;
    const float uz = fx * ry - fy * rx;
    Mat4 scale = Mat4::identity();
    scale.m[0] = thumb_scale_x_;
    scale.m[5] = thumb_scale_y_;
    Mat4 rot = Mat4::identity();
    rot.m[0] = rx;
    rot.m[1] = ry;
    rot.m[2] = rz;
    rot.m[4] = ux;
    rot.m[5] = uy;
    rot.m[6] = uz;
    rot.m[8] = fx;
    rot.m[9] = fy;
    rot.m[10] = fz;
    Mat4 trans = Mat4::identity();
    trans.m[12] = px;
    trans.m[13] = py;
    trans.m[14] = pz;
    const Mat4 thumb_model = Mat4::mul(trans, Mat4::mul(rot, scale));
    PushConstants tpc{};
    tpc.mvp = Mat4::mul(proj, Mat4::mul(view_mat, thumb_model));
    const bool thumb_stereo = thumb_stereo_ != StereoLayout::Mono;
    const bool thumb_project = thumb_mode_ != ProjectionMode::Flat;
    tpc.viewIndex = thumb_stereo ? static_cast<int32_t>(view_index) : 0;
    tpc.stereoMode = thumb_stereo ? static_cast<int32_t>(thumb_stereo_) : 0;
    tpc.is180 = (thumb_mode_ == ProjectionMode::Deg180) ? 1 : 0;
    tpc.nv12 = 0;
    tpc.project = thumb_project ? 1 : 0;
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(tpc), &tpc);
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vbo_quad_, &off);
    vkCmdDraw(cmd, 6, 1, 0, 0);
  }

  vkCmdEndRenderPass(cmd);
  vkEndCommandBuffer(cmd);
}

void SceneRenderer::render_frame(XrVulkanApp& app, const XrVulkanApp::FrameInfo& frame,
                                 std::vector<XrCompositionLayerProjectionView>& proj_views,
                                 XrCompositionLayerProjection& layer) {
  if (submit_fence_) {
    // Must finish the previous eye submit before rewriting its command buffers.
    // Never skip the layer: empty end_frame causes reprojection ghosting on head turn.
    vkWaitForFences(device_, 1, &submit_fence_, VK_TRUE, UINT64_MAX);
    vkResetFences(device_, 1, &submit_fence_);
  }
  // Descriptors are idle now — apply any video rebind staged from the XR loop.
  flush_queued_texture();

  if (hud_pending_dirty_ && hud_mapped_) {
    hud_vert_count_ = static_cast<uint32_t>(hud_pending_.size());
    if (hud_vert_count_ > hud_vert_cap_) hud_vert_count_ = hud_vert_cap_;
    if (hud_vert_count_ > 0) {
      std::memcpy(hud_mapped_, hud_pending_.data(), static_cast<size_t>(hud_vert_count_) * sizeof(HudVertex));
    }
    hud_pending_dirty_ = false;
  }

  proj_views.resize(frame.view_count);
  auto& swapchains = app.swapchains();
  std::array<uint32_t, 2> image_indices{};

  for (uint32_t i = 0; i < frame.view_count; ++i) {
    auto& sc = swapchains[i];
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    VRP_CHECK(XR_SUCCEEDED(xrAcquireSwapchainImage(sc.handle, &ai, &image_indices[i])), "acquire");
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    VRP_CHECK(XR_SUCCEEDED(xrWaitSwapchainImage(sc.handle, &wi)), "wait");
  }

  for (uint32_t i = 0; i < frame.view_count; ++i) {
    draw_view(i, frame.views[i], swapchains[i], image_indices[i], cmds_[i]);
  }

  // One submit for both eyes. Do NOT vkQueueWaitIdle here — that misses HMD
  // predictedDisplayTime and causes mid-frame tearing / reprojection artifacts.
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = frame.view_count;
  si.pCommandBuffers = cmds_.data();
  {
    std::lock_guard<std::mutex> qlock(app.queue_mutex());
    VRP_CHECK(vkQueueSubmit(app.queue(), 1, &si, submit_fence_) == VK_SUCCESS, "eye submit");
  }

  for (uint32_t i = 0; i < frame.view_count; ++i) {
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    VRP_CHECK(XR_SUCCEEDED(xrReleaseSwapchainImage(swapchains[i].handle, &ri)), "release");

    proj_views[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    proj_views[i].pose = frame.views[i].pose;
    proj_views[i].fov = frame.views[i].fov;
    proj_views[i].subImage.swapchain = swapchains[i].handle;
    proj_views[i].subImage.imageRect.offset = {0, 0};
    proj_views[i].subImage.imageRect.extent = {swapchains[i].width, swapchains[i].height};
  }

  layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  layer.space = app.app_space();
  layer.viewCount = static_cast<uint32_t>(proj_views.size());
  layer.views = proj_views.data();
}

}  // namespace vrp
