#pragma once

#include "common.hpp"

#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

struct AVFrame;

namespace vrp {

/**
 * VA-API surface → DRM PRIME dma-buf → sampled R8 + R8G8 images.
 * Same ping-pong contract as CudaNv12Texture: the decode thread imports,
 * the XR thread only rebinds views. No per-frame copy on the graphics queue.
 * If the modifier cannot be sampled as NV12 planes, try_import returns false
 * and the decoder keeps the CPU RGBA path.
 */
class VaapiNv12Texture {
 public:
  VaapiNv12Texture() = default;
  ~VaapiNv12Texture();

  VaapiNv12Texture(const VaapiNv12Texture&) = delete;
  VaapiNv12Texture& operator=(const VaapiNv12Texture&) = delete;

  /** True when the device can import dma-bufs with DRM format modifiers. */
  bool init(VkPhysicalDevice phys, VkDevice device, VkQueue queue, uint32_t queue_family,
            std::mutex* queue_mu, bool dma_buf_import);
  void shutdown();

  bool enabled() const { return enabled_; }
  bool full_range() const { return full_range_; }
  bool has_valid_frame() const { return valid_.load(std::memory_order_acquire); }
  int width() const { return width_; }
  int height() const { return height_; }

  VkImageView y_view() const;
  VkImageView uv_view() const;
  VkSampler sampler() const { return sampler_; }

  /** Decode thread. Maps a VAAPI frame to DRM PRIME and imports it. */
  bool try_import(const AVFrame* vaapi_frame, bool full_range_hint);

  /** True when the display slot changed since the last poll (XR thread). */
  bool poll_present();

  /** Drop imported images and held frames. Decode thread must be stopped. */
  void reset();

  const std::string& last_error() const { return last_error_; }

 private:
  struct Plane {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
  };
  struct Slot {
    Plane y;
    Plane uv;
    void* held = nullptr;  // AVFrame* keeping the VA surface alive
  };

  bool enabled_ = false;
  bool full_range_ = false;
  bool dying_ = false;
  int width_ = 0;
  int height_ = 0;
  std::string last_error_;

  VkPhysicalDevice phys_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t queue_family_ = 0;
  std::mutex* queue_mu_ = nullptr;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
  VkSampler sampler_ = VK_NULL_HANDLE;
  PFN_vkGetMemoryFdPropertiesKHR get_fd_props_ = nullptr;

  Slot slots_[3]{};
  std::atomic<int> display_slot_{0};
  int prev_slot_ = 0;
  std::atomic<bool> valid_{false};
  std::atomic<uint64_t> present_gen_{0};
  uint64_t bound_gen_ = 0;
  bool import_logged_ = false;

  void set_error(const std::string& e);
  void destroy_plane(Plane& p);
  void destroy_slot(Slot& slot);
  bool wait_fence();
  bool modifier_ok(uint64_t modifier, VkFormat format, int width, int height) const;
  bool import_plane(Plane& plane, int fd, uint64_t modifier, int width, int height, VkFormat format,
                    VkDeviceSize offset, VkDeviceSize row_pitch, VkDeviceSize size);
  bool transition_slot(const Slot& slot);
};
}  // namespace vrp
