#pragma once

#include "capture/pipewire_capture.hpp"
#include "common.hpp"

#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace vrp {

/**
 * Single-plane PipeWire ScreenCast dma-buf (BGRA/XRGB/…) → sampled VkImage.
 * Same ping-pong contract as VaapiNv12Texture: capture/import thread owns
 * try_import; XR thread only poll_present + view/sampler. On modifier or
 * format failure, try_import returns false and the host keeps CPU RGBA.
 */
class PipewireDmaTexture {
 public:
  PipewireDmaTexture() = default;
  ~PipewireDmaTexture();

  PipewireDmaTexture(const PipewireDmaTexture&) = delete;
  PipewireDmaTexture& operator=(const PipewireDmaTexture&) = delete;

  bool init(VkPhysicalDevice phys, VkDevice device, VkQueue queue, uint32_t queue_family,
            std::mutex* queue_mu, bool dma_buf_import);
  void shutdown();

  bool enabled() const { return enabled_; }
  bool has_valid_frame() const { return valid_.load(std::memory_order_acquire); }
  int width() const { return width_; }
  int height() const { return height_; }

  VkImageView view() const;
  VkSampler sampler() const { return sampler_; }

  /** Import a dma-buf frame. Does not take ownership of `frame.fd` (dups internally). */
  bool try_import(const PipewireDmaFrame& frame);

  bool poll_present();
  void reset();

  const std::string& last_error() const { return last_error_; }

 private:
  struct Slot {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
  };

  bool enabled_ = false;
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
  void destroy_slot(Slot& slot);
  bool wait_fence();
  bool modifier_ok(uint64_t modifier, VkFormat format, int width, int height) const;
  bool import_image(Slot& slot, int fd, uint64_t modifier, int width, int height, VkFormat format,
                    VkDeviceSize offset, VkDeviceSize row_pitch, VkDeviceSize size);
  bool transition_slot(const Slot& slot);
  static bool map_drm_to_vk(uint32_t drm_fourcc, VkFormat& out_format);
};

}  // namespace vrp
