#include "video/vulkan_video_probe.hpp"

#include "common.hpp"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#if defined(VRP_HAS_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}
#endif

namespace vrp {
namespace {

bool has_device_ext(VkPhysicalDevice phys, const char* name) {
  uint32_t n = 0;
  vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, nullptr);
  std::vector<VkExtensionProperties> exts(n);
  if (n) vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, exts.data());
  for (const auto& e : exts) {
    if (std::strcmp(e.extensionName, name) == 0) return true;
  }
  return false;
}

#if defined(VRP_HAS_FFMPEG)
bool ffmpeg_lists_vulkan_type() {
  AVHWDeviceType t = AV_HWDEVICE_TYPE_NONE;
  while ((t = av_hwdevice_iterate_types(t)) != AV_HWDEVICE_TYPE_NONE) {
    if (t == AV_HWDEVICE_TYPE_VULKAN) return true;
  }
  return false;
}

bool ffmpeg_codec_has_vulkan(AVCodecID id) {
  const AVCodec* dec = avcodec_find_decoder(id);
  if (!dec) return false;
  for (int i = 0;; ++i) {
    const AVCodecHWConfig* cfg = avcodec_get_hw_config(dec, i);
    if (!cfg) break;
    if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
        cfg->device_type == AV_HWDEVICE_TYPE_VULKAN) {
      return true;
    }
  }
  return false;
}
#endif

}  // namespace

std::string VulkanVideoProbe::summary() const {
  std::ostringstream o;
  o << "Vulkan Video probe: candidate=" << (candidate ? "yes" : "no");
  o << " api=" << VK_API_VERSION_MAJOR(api_version) << '.' << VK_API_VERSION_MINOR(api_version)
    << (api_1_3 ? " (>=1.3 ok)" : " (<1.3 — FFmpeg needs 1.3+)");
  o << " ext[vq=" << ext_video_queue << " decq=" << ext_video_decode_queue
    << " h264=" << ext_decode_h264 << " h265=" << ext_decode_h265 << " av1=" << ext_decode_av1
    << " vp9=" << ext_decode_vp9
    << " timeline=" << ext_timeline_semaphore << " sync2=" << ext_synchronization2 << ']';
  if (decode_queue_family != UINT32_MAX) {
    o << " decode_qf=" << decode_queue_family << "x" << decode_queue_count;
  } else {
    o << " decode_qf=none";
  }
  o << " ffmpeg[type=" << ffmpeg_vulkan_type << " codec=" << ffmpeg_codec_vulkan << ']';
  o << " device=" << (enabled_on_device ? "video-ready" : "baseline");
  return o.str();
}

VulkanVideoProbe probe_vulkan_video(VkPhysicalDevice phys) {
  VulkanVideoProbe p;
  if (!phys) return p;
  p.phys_ok = true;

  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(phys, &props);
  p.api_version = props.apiVersion;
  p.api_1_3 = VK_API_VERSION_MAJOR(props.apiVersion) > 1 ||
              (VK_API_VERSION_MAJOR(props.apiVersion) == 1 &&
               VK_API_VERSION_MINOR(props.apiVersion) >= 3);

  p.ext_video_queue = has_device_ext(phys, VK_KHR_VIDEO_QUEUE_EXTENSION_NAME);
  p.ext_video_decode_queue = has_device_ext(phys, VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME);
#if defined(VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME)
  p.ext_decode_h264 = has_device_ext(phys, VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME);
#endif
#if defined(VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME)
  p.ext_decode_h265 = has_device_ext(phys, VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME);
#endif
#if defined(VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME)
  p.ext_decode_av1 = has_device_ext(phys, VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME);
#endif
#if defined(VK_KHR_VIDEO_DECODE_VP9_EXTENSION_NAME)
  if (has_device_ext(phys, VK_KHR_VIDEO_DECODE_VP9_EXTENSION_NAME)) {
    VkPhysicalDeviceVideoDecodeVP9FeaturesKHR vp9{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_DECODE_VP9_FEATURES_KHR};
    VkPhysicalDeviceFeatures2 feats{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    feats.pNext = &vp9;
    vkGetPhysicalDeviceFeatures2(phys, &feats);
    p.ext_decode_vp9 = vp9.videoDecodeVP9 == VK_TRUE;
  }
#endif
  p.ext_timeline_semaphore =
      has_device_ext(phys, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME) || p.api_1_3;
  p.ext_synchronization2 =
      has_device_ext(phys, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME) || p.api_1_3;

  uint32_t qcount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(phys, &qcount, nullptr);
  std::vector<VkQueueFamilyProperties> qprops(qcount);
  if (qcount) vkGetPhysicalDeviceQueueFamilyProperties(phys, &qcount, qprops.data());
  for (uint32_t i = 0; i < qcount; ++i) {
    if (qprops[i].queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) {
      p.decode_queue_family = i;
      p.decode_queue_count = qprops[i].queueCount;
      break;
    }
  }

#if defined(VRP_HAS_FFMPEG)
  p.ffmpeg_vulkan_type = ffmpeg_lists_vulkan_type();
  p.ffmpeg_codec_vulkan = ffmpeg_codec_has_vulkan(AV_CODEC_ID_H264) ||
                          ffmpeg_codec_has_vulkan(AV_CODEC_ID_HEVC) ||
                          ffmpeg_codec_has_vulkan(AV_CODEC_ID_AV1) ||
                          ffmpeg_codec_has_vulkan(AV_CODEC_ID_VP9);
#else
  p.ffmpeg_vulkan_type = false;
  p.ffmpeg_codec_vulkan = false;
#endif

  const bool device_side = p.ext_video_queue && p.ext_video_decode_queue &&
                           p.decode_queue_family != UINT32_MAX &&
                           (p.ext_decode_h264 || p.ext_decode_h265 || p.ext_decode_av1 ||
                            p.ext_decode_vp9);
  // Hardware + FFmpeg type present. api_1_3 is separately required to inject into FFmpeg.
  p.candidate = device_side && p.ffmpeg_vulkan_type && p.ffmpeg_codec_vulkan;

  VRP_LOG("%s (%s)", p.summary().c_str(), props.deviceName);
  if (p.candidate && !p.api_1_3) {
    VRP_LOG("Vulkan Video: device usable but API < 1.3 — cannot inject FFmpeg Vulkan hwctx");
  } else if (!p.candidate) {
    VRP_LOG("Vulkan Video: not a zero-copy candidate — keep CUDA/VAAPI path");
  } else {
    VRP_LOG("Vulkan Video: zero-copy candidate (enable with monasphere.conf vulkan_video=1)");
  }
  return p;
}

}  // namespace vrp
