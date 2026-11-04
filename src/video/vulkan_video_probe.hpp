#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>

namespace vrp {

/**
 * Phase 0 probe for optional FFmpeg Vulkan Video zero-copy (AV_PIX_FMT_VULKAN).
 * Does not change the active CUDA/VAAPI decode path — capability logging only.
 */
struct VulkanVideoProbe {
  bool phys_ok = false;
  uint32_t api_version = 0;  // VkPhysicalDeviceProperties::apiVersion
  bool api_1_3 = false;      // FFmpeg AVVulkanDeviceContext requires ≥ 1.3

  bool ext_video_queue = false;
  bool ext_video_decode_queue = false;
  bool ext_decode_h264 = false;
  bool ext_decode_h265 = false;
  bool ext_decode_av1 = false;
  bool ext_timeline_semaphore = false;
  bool ext_synchronization2 = false;

  /** Queue family with VK_QUEUE_VIDEO_DECODE_BIT_KHR, or UINT32_MAX. */
  uint32_t decode_queue_family = UINT32_MAX;
  uint32_t decode_queue_count = 0;

  /** libavutil lists AV_HWDEVICE_TYPE_VULKAN. */
  bool ffmpeg_vulkan_type = false;
  /** At least one common codec advertises VULKAN HW_DEVICE_CTX. */
  bool ffmpeg_codec_vulkan = false;

  /**
   * True when device-side Video Decode bits look present AND FFmpeg can see
   * the vulkan hwaccel type. Does not guarantee NVIDIA driver decode works.
   */
  bool candidate = false;

  /** Set after Phase 1: XR VkDevice actually enabled video decode exts + queue. */
  bool enabled_on_device = false;

  std::string summary() const;
};

/** Probe a physical device (+ optional FFmpeg). Safe before logical device create. */
VulkanVideoProbe probe_vulkan_video(VkPhysicalDevice phys);

}  // namespace vrp
