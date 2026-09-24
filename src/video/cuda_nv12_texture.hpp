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
 * NV12 ping-pong: CUDA writes directly into exportable LINEAR VkImages (Y R8 + UV R8G8).
 * No vkCmdCopyBufferToImage on the graphics queue — avoids Monado wait_semaphore stalls.
 * Decode thread copies; XR only polls and rebinds descriptors.
 */
class CudaNv12Texture {
 public:
  bool init(VkPhysicalDevice phys, VkDevice device, VkQueue queue, uint32_t queue_family,
            std::mutex* queue_mu, bool external_memory_fd);
  void shutdown();

  bool enabled() const { return enabled_; }

  bool try_copy_from_cuda(const AVFrame* cuda_frame, void* cu_context, void* cu_stream,
                          bool full_range_hint = false);
  bool poll_present();

  VkImageView y_view() const {
    return slots_[display_slot_.load(std::memory_order_acquire)].y.view;
  }
  VkImageView uv_view() const {
    return slots_[display_slot_.load(std::memory_order_acquire)].uv.view;
  }
  VkSampler sampler() const { return sampler_; }
  int width() const { return width_; }
  int height() const { return height_; }
  /** True when last presented frame is full-range (PC) YUV. */
  bool full_range() const { return full_range_.load(std::memory_order_acquire); }
  bool has_valid_frame() const { return valid_.load(std::memory_order_acquire); }
  uint64_t copy_ok() const { return copy_ok_.load(); }
  uint64_t copy_fail() const { return copy_fail_.load(); }
  uint64_t present_flips() const { return present_flips_.load(); }
  std::string last_error() const;

  /** Destroy NV12 slots. Pass the FFmpeg/NVDEC CUcontext used for import (before decoder.close). */
  void reset_slots(void* cu_context = nullptr);
  /** Wait until in-flight CUDA copies finish (call before reset_slots / reopen). */
  void drain_copies();
  void prepare_shutdown();

 private:
  struct Plane {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    void* cuda_ext_mem = nullptr;
    unsigned long long cuda_ptr = 0;
    VkDeviceSize alloc_size = 0;
    VkDeviceSize row_pitch = 0;
    VkDeviceSize offset = 0;
    int width = 0;
    int height = 0;
  };

  struct Slot {
    Plane y;
    Plane uv;
  };

  VkPhysicalDevice phys_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t queue_family_ = 0;
  std::mutex* queue_mu_ = nullptr;
  PFN_vkGetMemoryFdKHR get_memory_fd_ = nullptr;

  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
  VkSampler sampler_ = VK_NULL_HANDLE;

  Slot slots_[2]{};
  std::atomic<int> display_slot_{0};
  int write_ = 1;
  int width_ = 0;
  int height_ = 0;

  bool enabled_ = false;
  std::atomic<bool> valid_{false};
  std::atomic<bool> full_range_{false};
  bool create_failed_ = false;
  bool layouts_ready_ = false;
  std::atomic<bool> dying_{false};
  std::atomic<bool> copy_active_{false};
  std::atomic<uint64_t> copy_ok_{0};
  std::atomic<uint64_t> copy_fail_{0};
  std::atomic<uint64_t> present_flips_{0};
  std::atomic<uint64_t> present_gen_{0};
  uint64_t bound_gen_ = 0;

  mutable std::mutex err_mu_;
  std::string last_error_;
  /** Serialize CUDA copy vs slot destroy/recreate. */
  std::mutex slots_mu_;

  uint8_t vk_uuid_[16]{};
  int cuda_dev_ = -1;

  void set_error(const std::string& e);
  uint32_t find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags props) const;
  bool match_cuda_uuid();
  struct CtxScope {
    bool pushed = false;
    bool primary_retained = false;
    int cuda_dev = -1;
  };
  /** Push CUDA context; caller must pop_cuda_ctx(scope) when done. */
  bool push_cuda_ctx(CtxScope& scope, void* preferred = nullptr);
  void pop_cuda_ctx(CtxScope& scope);
  void destroy_plane(Plane& p);
  void destroy_slots();
  bool create_plane(Plane& p, int width, int height, VkFormat format);
  bool create_slot(Slot& slot, int width, int height);
  bool import_cuda_plane(Plane& p);
  bool ensure_cuda_imports();
  bool ensure_size(int width, int height);
  bool transition_all_to_shader_read();
  bool fence_ready() const;
  bool wait_fence_timeout(uint64_t ns);
};
}  // namespace vrp
