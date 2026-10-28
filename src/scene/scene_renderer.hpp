#pragma once

#include "options.hpp"
#include "ui/hud_mesh.hpp"
#include "video/video_decoder.hpp"
#include "xr/xr_vulkan_app.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace vrp {

class SceneRenderer {
 public:
  void init(XrVulkanApp& app, const std::string& shader_dir);
  void shutdown();

  void set_projection(ProjectionMode mode, StereoLayout stereo, float flat_fov_deg, float distance);
  /** Content width/height for Flat screen sizing (default 16:9). */
  void set_content_aspect(float width_over_height);
  /** Extra yaw/pitch (radians) around the head. HUD stays locked to the real pose. */
  void set_view_offset(float yaw_rad, float pitch_rad);
  /** Latitude/longitude cage locked to the picture, drawn while the stick is turning the view. */
  void set_comfort_grid(bool on);
  void set_texture(VkImageView view, VkSampler sampler);
  /** RGBA video that is still gamma-encoded (e.g. FSR1 output). Linearized like NV12. */
  void set_texture_gamma(VkImageView view, VkSampler sampler);
  /**
   * Flat content cursor overlay (u/v in 0..1). Used when ScreenCast cannot
   * embed the DE cursor (AvailableCursorModes empty → Hidden).
   */
  void set_content_cursor(float u, float v, bool on) {
    content_cursor_u_ = u;
    content_cursor_v_ = v;
    content_cursor_on_ = on;
  }
  void set_nv12_texture(VkImageView y, VkImageView uv, VkSampler sampler, bool full_range = false);
  /**
   * Stage a video descriptor update applied after the previous eye submit finishes
   * (inside render_frame). Safe from the XR loop without an extra fence wait.
   */
  void queue_texture(VkImageView view, VkSampler sampler);
  void queue_texture_gamma(VkImageView view, VkSampler sampler);
  void queue_nv12_texture(VkImageView y, VkImageView uv, VkSampler sampler, bool full_range = false);
  /** HUD atlas (RGBA). Empty view clears overlay. */
  void set_hud_texture(VkImageView view, VkSampler sampler, bool visible);
  /**
   * Quads that sample the HUD atlas, in the 16:9 panel's local space.
   * Copied to the GPU after the previous eye submit finishes.
   */
  void set_hud_mesh(const HudVertex* vertices, uint32_t count);
  /** World size of the HUD quad. y_offset_m shifts the quad up. */
  void set_hud_layout(float half_width_m, float distance_m, float aspect_h_over_w = 0.f,
                      float y_offset_m = 0.f);
  /**
   * Thumbnail card in front of the file dialog.
   * world_half_w is meters. pixel_h_over_w is the card's height/width.
   */
  void set_thumb_texture(VkImageView view, VkSampler sampler, bool visible);
  void set_thumb_pose(float world_half_w, float pixel_h_over_w, float x_m, float y_m, float distance_m);
  /** File-dialog preview. Flat stays a frame card. 180/360 projects a square window. */
  void set_thumb_format(ProjectionMode mode, StereoLayout stereo);
  /** Wait for previous eye submit. Returns false on timeout (do not rewrite descriptors). */
  bool wait_previous_submit(uint64_t timeout_ns = 8'000'000ull);

  /** Milky Way intensity multiplier (□ / X cycles). Sky comes from Gaia equirect JPEG. */
  void set_milkyway_gain(float gain_mul);
  float milkyway_gain() const;

  /** Observer geodetic position for Earth-horizon sky (degrees). Default Tokyo. */
  void set_sky_observer(float lat_deg, float lon_deg);
  float sky_lat_deg() const { return sky_lat_deg_; }
  float sky_lon_deg() const { return sky_lon_deg_; }

  // Render both eye swapchains for the current frame; fills projection layers.
  void render_frame(XrVulkanApp& app, const XrVulkanApp::FrameInfo& frame,
                    std::vector<XrCompositionLayerProjectionView>& proj_views,
                    XrCompositionLayerProjection& layer);

 private:
  struct Vertex {
    float x, y, z;
    float u, v;
  };

  void create_pipelines(VkFormat format);
  void build_meshes();
  void create_descriptors();
  void flush_queued_texture();
  uint32_t find_memory(uint32_t bits, VkMemoryPropertyFlags flags) const;
  VkShaderModule load_shader(const std::string& path);
  void draw_view(uint32_t view_index, const XrView& view, ViewSwapchain& sc, uint32_t image_index,
                 VkCommandBuffer cmd);

  XrVulkanApp* app_ = nullptr;
  VkDevice device_ = VK_NULL_HANDLE;
  VkPhysicalDevice phys_ = VK_NULL_HANDLE;

  ProjectionMode mode_ = ProjectionMode::Flat;
  StereoLayout stereo_ = StereoLayout::Mono;
  float flat_fov_deg_ = 70.f;
  float distance_ = 4.f;
  float content_aspect_ = 16.f / 9.f;
  float look_yaw_ = 0.f;
  float look_pitch_ = 0.f;
  bool comfort_grid_ = false;
  bool content_cursor_on_ = false;
  float content_cursor_u_ = 0.5f;
  float content_cursor_v_ = 0.5f;

  VkPipeline grid_pipeline_ = VK_NULL_HANDLE;
  VkPipelineLayout grid_layout_ = VK_NULL_HANDLE;
  VkShaderModule grid_vert_ = VK_NULL_HANDLE;
  VkShaderModule grid_frag_ = VK_NULL_HANDLE;
  VkBuffer vbo_grid_ = VK_NULL_HANDLE;
  VkDeviceMemory vbo_grid_mem_ = VK_NULL_HANDLE;
  uint32_t grid_vert_count_ = 0;

  VkPipeline star_pipeline_ = VK_NULL_HANDLE;
  VkShaderModule star_vert_ = VK_NULL_HANDLE;
  VkShaderModule star_frag_ = VK_NULL_HANDLE;
  VkBuffer vbo_stars_ = VK_NULL_HANDLE;
  VkDeviceMemory vbo_stars_mem_ = VK_NULL_HANDLE;
  uint32_t star_vert_count_ = 0;

  VkPipeline nebula_pipeline_ = VK_NULL_HANDLE;
  VkShaderModule nebula_vert_ = VK_NULL_HANDLE;
  VkShaderModule nebula_frag_ = VK_NULL_HANDLE;
  VkBuffer vbo_nebulae_ = VK_NULL_HANDLE;
  VkDeviceMemory vbo_nebulae_mem_ = VK_NULL_HANDLE;
  uint32_t nebula_vert_count_ = 0;

  VkPipeline moon_pipeline_ = VK_NULL_HANDLE;
  VkPipeline sun_pipeline_ = VK_NULL_HANDLE;
  VkPipelineLayout moon_layout_ = VK_NULL_HANDLE;
  VkShaderModule moon_vert_ = VK_NULL_HANDLE;
  VkShaderModule moon_frag_ = VK_NULL_HANDLE;
  VkShaderModule sun_vert_ = VK_NULL_HANDLE;
  VkShaderModule sun_frag_ = VK_NULL_HANDLE;
  VkBuffer vbo_moon_ = VK_NULL_HANDLE;
  VkDeviceMemory vbo_moon_mem_ = VK_NULL_HANDLE;

  VkPipeline milkyway_pipeline_ = VK_NULL_HANDLE;
  VkPipelineLayout milkyway_layout_ = VK_NULL_HANDLE;
  VkShaderModule milkyway_vert_ = VK_NULL_HANDLE;
  VkShaderModule milkyway_frag_ = VK_NULL_HANDLE;
  VkBuffer vbo_milkyway_ = VK_NULL_HANDLE;
  VkDeviceMemory vbo_milkyway_mem_ = VK_NULL_HANDLE;
  VkBuffer ibo_milkyway_ = VK_NULL_HANDLE;
  VkDeviceMemory ibo_milkyway_mem_ = VK_NULL_HANDLE;
  uint32_t milkyway_index_count_ = 0;
  float milkyway_gain_mul_ = 1.f;
  float sky_lat_deg_ = 35.68f;   // Tokyo default
  float sky_lon_deg_ = 139.65f;
  bool milkyway_sky_ok_ = false;
  VkDescriptorSetLayout milkyway_dsl_ = VK_NULL_HANDLE;
  VkDescriptorPool milkyway_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet milkyway_dset_ = VK_NULL_HANDLE;
  VkImage milkyway_img_ = VK_NULL_HANDLE;
  VkDeviceMemory milkyway_img_mem_ = VK_NULL_HANDLE;
  VkImageView milkyway_view_ = VK_NULL_HANDLE;
  VkSampler milkyway_samp_ = VK_NULL_HANDLE;
  void load_milkyway_sky_texture();

  VkDescriptorSetLayout dsl_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  VkDescriptorSet dset_ = VK_NULL_HANDLE;
  VkDescriptorSet hud_dset_ = VK_NULL_HANDLE;
  VkDescriptorSet thumb_dset_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  VkShaderModule vert_ = VK_NULL_HANDLE;
  VkShaderModule frag_ = VK_NULL_HANDLE;

  VkBuffer vbo_quad_ = VK_NULL_HANDLE;
  VkDeviceMemory vbo_quad_mem_ = VK_NULL_HANDLE;
  VkBuffer vbo_hud_ = VK_NULL_HANDLE;
  VkDeviceMemory vbo_hud_mem_ = VK_NULL_HANDLE;
  void* hud_mapped_ = nullptr;
  uint32_t hud_vert_cap_ = 0;
  uint32_t hud_vert_count_ = 0;
  std::vector<HudVertex> hud_pending_;
  bool hud_pending_dirty_ = false;
  VkBuffer vbo_sphere_ = VK_NULL_HANDLE;
  VkDeviceMemory vbo_sphere_mem_ = VK_NULL_HANDLE;
  VkBuffer ibo_sphere_ = VK_NULL_HANDLE;
  VkDeviceMemory ibo_sphere_mem_ = VK_NULL_HANDLE;
  uint32_t sphere_index_count_ = 0;

  std::vector<VkCommandBuffer> cmds_;
  std::string shader_dir_;
  bool nv12_ = false;
  bool nv12_full_range_ = false;
  bool rgba_gamma_ = false;  // FSR / CPU video: gamma RGB needing srgb_to_linear
  enum class QueuedTex : uint8_t { None, Rgba, RgbaGamma, Nv12 };
  QueuedTex queued_tex_ = QueuedTex::None;
  VkImageView queued_y_ = VK_NULL_HANDLE;
  VkImageView queued_uv_ = VK_NULL_HANDLE;
  VkSampler queued_samp_ = VK_NULL_HANDLE;
  bool queued_nv12_full_ = false;
  bool hud_visible_ = false;
  float hud_distance_ = 1.5f;
  float hud_half_w_ = 0.55f;
  float hud_aspect_ = 1.05f;
  float hud_y_ = 0.f;
  bool thumb_visible_ = false;
  ProjectionMode thumb_mode_ = ProjectionMode::Flat;
  StereoLayout thumb_stereo_ = StereoLayout::Mono;
  float thumb_scale_x_ = 0.16f;
  float thumb_scale_y_ = 0.16f;
  float thumb_x_ = 0.f;
  float thumb_y_ = 0.f;
  float thumb_distance_ = 1.32f;
  VkFence submit_fence_ = VK_NULL_HANDLE;
};

}  // namespace vrp
