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
  void set_texture(VkImageView view, VkSampler sampler);
  /** RGBA video that is still gamma-encoded (e.g. FSR1 output). Linearized like NV12. */
  void set_texture_gamma(VkImageView view, VkSampler sampler);
  void set_nv12_texture(VkImageView y, VkImageView uv, VkSampler sampler, bool full_range = false);
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
  /** Wait for previous eye submit. Returns false on timeout (do not rewrite descriptors). */
  bool wait_previous_submit(uint64_t timeout_ns = 8'000'000ull);

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
  bool hud_visible_ = false;
  float hud_distance_ = 1.5f;
  float hud_half_w_ = 0.55f;
  float hud_aspect_ = 1.05f;
  float hud_y_ = 0.f;
  bool thumb_visible_ = false;
  float thumb_scale_x_ = 0.16f;
  float thumb_scale_y_ = 0.16f;
  float thumb_x_ = 0.f;
  float thumb_y_ = 0.f;
  float thumb_distance_ = 1.32f;
  VkFence submit_fence_ = VK_NULL_HANDLE;
};

}  // namespace vrp
