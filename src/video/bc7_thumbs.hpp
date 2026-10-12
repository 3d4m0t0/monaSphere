#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace vrp {

/** NVIDIA seek stills: BC1 images kept in VRAM and sampled directly. */
class Bc7ThumbCache {
 public:
  /**
   * `queue` is the compute queue. `graphics_queue` is the eye queue that samples the images.
   * When they differ, compress waits on `graphics_queue` before publishing a view.
   */
  bool init(VkPhysicalDevice phys, VkDevice device, VkQueue queue, VkQueue graphics_queue,
            uint32_t queue_family, std::mutex* graphics_mu, const std::string& shader_dir);
  void shutdown();

  bool ok() const { return ok_; }
  /**
   * Retire the in-flight compress and start a new build generation.
   * Images stay alive until release_images(); the current eye frame may still sample them.
   */
  uint64_t begin_build();
  /** Destroy cached images. Call only after the graphics queue has finished the frame that sampled them. */
  void release_images();

  /**
   * Compress one tightly packed RGBA still into slot `index`.
   * `build` is the value from begin_build. A stale build returns false.
   */
  bool compress(int index, const uint8_t* rgba, int width, int height, double interval_sec, uint64_t build);

  int index_for(double sec) const;
  VkImageView view(int index) const;
  VkSampler sampler() const { return sampler_; }

 private:
  struct Slot {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    int width = 0;
    int height = 0;
  };

  VkPhysicalDevice phys_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  VkQueue graphics_queue_ = VK_NULL_HANDLE;
  uint32_t queue_family_ = 0;
  std::mutex* graphics_mu_ = nullptr;
  bool same_queue_ = true;

  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;
  VkCommandBuffer handoff_cmd_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
  VkFence handoff_fence_ = VK_NULL_HANDLE;
  VkSemaphore handoff_sem_ = VK_NULL_HANDLE;
  VkSampler sampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout dsl_ = VK_NULL_HANDLE;
  VkDescriptorPool dset_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet dset_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipeline pipe_ = VK_NULL_HANDLE;
  VkShaderModule shader_ = VK_NULL_HANDLE;

  VkBuffer src_ = VK_NULL_HANDLE;
  VkDeviceMemory src_mem_ = VK_NULL_HANDLE;
  VkDeviceSize src_size_ = 0;
  void* src_mapped_ = nullptr;
  VkBuffer dst_ = VK_NULL_HANDLE;
  VkDeviceMemory dst_mem_ = VK_NULL_HANDLE;
  VkDeviceSize dst_size_ = 0;

  std::vector<Slot> slots_;
  int ready_ = 0;
  uint64_t build_ = 0;
  double interval_sec_ = 0;
  bool ok_ = false;
  mutable std::mutex mu_;

  uint32_t find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags props) const;
  bool ensure_src(VkDeviceSize bytes);
  bool ensure_dst(VkDeviceSize bytes);
  bool ensure_slot(int index, int blocks_x, int blocks_y);
  void destroy_slot(Slot& s);
  void write_descriptors();
};

}  // namespace vrp
