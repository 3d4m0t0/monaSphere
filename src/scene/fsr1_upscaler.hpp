#pragma once

#include "options.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <mutex>
#include <string>

namespace vrp {

/** AMD FidelityFX Super Resolution 1 (EASU + RCAS) on the video texture. */
class Fsr1Upscaler {
 public:
  void init(VkPhysicalDevice phys, VkDevice device, VkQueue queue, uint32_t queue_family,
            std::mutex* queue_mu, const std::string& shader_dir);
  void shutdown();

  void set_mode(FsrMode mode) { mode_ = mode; }
  FsrMode mode() const { return mode_; }
  bool enabled() const { return mode_ != FsrMode::Off; }

  /**
   * Upscale current video frame into an internal RGBA texture.
   * Returns true when output() should be bound for scene sampling.
   * nv12: 0=RGBA src_y only, 1=limited, 2=full-range.
   */
  bool process(VkImageView src_y, VkImageView src_uv, VkSampler src_sampler, int src_w, int src_h,
               int nv12);

  VkImageView output_view() const { return out_view_; }
  VkSampler output_sampler() const { return sampler_; }
  int output_width() const { return out_w_; }
  int output_height() const { return out_h_; }

 private:
  struct Rt {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    int w = 0;
    int h = 0;
  };

  struct FsrConsts {
    uint32_t const0[4];
    uint32_t const1[4];
    uint32_t const2[4];
    uint32_t const3[4];
    uint32_t sample[4];
  };

  bool ensure_rt(Rt& rt, int w, int h, VkImageUsageFlags usage);
  void destroy_rt(Rt& rt);
  uint32_t find_memory(uint32_t bits, VkMemoryPropertyFlags flags) const;
  VkShaderModule load_shader(const std::string& path) const;
  void create_pipelines();
  void destroy_pipelines();
  void update_blit_set(VkImageView y, VkImageView uv, VkSampler samp, int nv12);
  void update_fsr_set(VkDescriptorSet set, VkBuffer ubo, VkImageView in_view, VkImageView out_view);
  void barrier_rw(VkCommandBuffer cmd, VkImage image, VkAccessFlags src, VkAccessFlags dst,
                  VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage,
                  VkImageLayout layout);

  VkPhysicalDevice phys_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t queue_family_ = 0;
  std::mutex* queue_mu_ = nullptr;
  std::string shader_dir_;

  FsrMode mode_ = FsrMode::Off;

  VkDescriptorSetLayout blit_dsl_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout fsr_dsl_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  VkDescriptorSet blit_set_ = VK_NULL_HANDLE;
  VkDescriptorSet easu_set_ = VK_NULL_HANDLE;
  VkDescriptorSet rcas_set_ = VK_NULL_HANDLE;

  VkPipelineLayout blit_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout fsr_layout_ = VK_NULL_HANDLE;
  VkPipeline blit_pipe_ = VK_NULL_HANDLE;
  VkPipeline easu_pipe_ = VK_NULL_HANDLE;
  VkPipeline rcas_pipe_ = VK_NULL_HANDLE;
  VkShaderModule blit_cs_ = VK_NULL_HANDLE;
  VkShaderModule easu_cs_ = VK_NULL_HANDLE;
  VkShaderModule rcas_cs_ = VK_NULL_HANDLE;

  VkSampler sampler_ = VK_NULL_HANDLE;
  VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;

  VkBuffer blit_ubo_ = VK_NULL_HANDLE;
  VkDeviceMemory blit_ubo_mem_ = VK_NULL_HANDLE;
  void* blit_ubo_map_ = nullptr;

  VkBuffer easu_ubo_ = VK_NULL_HANDLE;
  VkDeviceMemory easu_ubo_mem_ = VK_NULL_HANDLE;
  void* easu_ubo_map_ = nullptr;

  VkBuffer rcas_ubo_ = VK_NULL_HANDLE;
  VkDeviceMemory rcas_ubo_mem_ = VK_NULL_HANDLE;
  void* rcas_ubo_map_ = nullptr;

  Rt src_rt_{};   // video-res RGBA
  Rt easu_rt_{};  // upscaled
  Rt out_rt_{};   // after RCAS
  VkImageView out_view_ = VK_NULL_HANDLE;
  int out_w_ = 0;
  int out_h_ = 0;
};

}  // namespace vrp
