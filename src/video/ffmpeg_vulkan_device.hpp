#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#if defined(VRP_HAS_FFMPEG)
extern "C" {
#include <libavutil/hwcontext.h>
}
#else
struct AVBufferRef;
struct AVHWDeviceContext;
#endif

namespace vrp {

/**
 * Phase 2: wrap the OpenXR VkInstance/VkDevice as FFmpeg AV_HWDEVICE_TYPE_VULKAN
 * without transferring ownership (CUDA playback remains default).
 */
class FfmpegVulkanDevice {
 public:
  FfmpegVulkanDevice() = default;
  ~FfmpegVulkanDevice() { reset(); }

  FfmpegVulkanDevice(const FfmpegVulkanDevice&) = delete;
  FfmpegVulkanDevice& operator=(const FfmpegVulkanDevice&) = delete;

  /**
   * Inject XR Vulkan objects into a new AVHWDeviceContext.
   * @param queue_mu  serialized with Monado / eye submits (lock_queue)
   * @param enabled_dev_exts  extensions actually enabled on act_dev (may be empty)
   */
  bool init(VkInstance inst, VkPhysicalDevice phys, VkDevice act_dev,
            uint32_t graphics_qf, uint32_t graphics_queue_count, uint32_t decode_qf,
            uint32_t decode_queue_count, std::mutex* queue_mu,
            const std::vector<const char*>& enabled_dev_exts);

  void reset();

  bool ok() const { return ref_ != nullptr; }
  /** Borrowed FFmpeg hwdevice ref (owned here). */
  AVBufferRef* ref() const { return ref_; }
  const std::string& last_error() const { return last_error_; }

 private:
  static void lock_queue(AVHWDeviceContext* ctx, uint32_t queue_family, uint32_t index);
  static void unlock_queue(AVHWDeviceContext* ctx, uint32_t queue_family, uint32_t index);

  AVBufferRef* ref_ = nullptr;
  std::mutex* queue_mu_ = nullptr;
  std::string last_error_;

  // Kept alive for AVVulkanDeviceContext.device_features pNext.
  VkPhysicalDeviceVulkan12Features feats12_{};
  VkPhysicalDeviceVulkan13Features feats13_{};
  VkPhysicalDeviceFeatures2 feats2_{};
};

}  // namespace vrp
