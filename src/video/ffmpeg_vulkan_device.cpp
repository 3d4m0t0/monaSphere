#include "video/ffmpeg_vulkan_device.hpp"

#include "common.hpp"

#if !defined(VRP_HAS_FFMPEG)

namespace vrp {

bool FfmpegVulkanDevice::init(VkInstance, VkPhysicalDevice, VkDevice, uint32_t, uint32_t, uint32_t,
                              uint32_t, std::mutex*, const std::vector<const char*>&) {
  last_error_ = "FFmpeg not built";
  return false;
}
void FfmpegVulkanDevice::reset() {}
void FfmpegVulkanDevice::lock_queue(struct AVHWDeviceContext*, uint32_t, uint32_t) {}
void FfmpegVulkanDevice::unlock_queue(struct AVHWDeviceContext*, uint32_t, uint32_t) {}

}  // namespace vrp

#else

#include <algorithm>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
}

namespace vrp {

void FfmpegVulkanDevice::lock_queue(AVHWDeviceContext* ctx, uint32_t, uint32_t) {
  auto* self = static_cast<FfmpegVulkanDevice*>(ctx->user_opaque);
  if (self && self->queue_mu_) self->queue_mu_->lock();
}

void FfmpegVulkanDevice::unlock_queue(AVHWDeviceContext* ctx, uint32_t, uint32_t) {
  auto* self = static_cast<FfmpegVulkanDevice*>(ctx->user_opaque);
  if (self && self->queue_mu_) self->queue_mu_->unlock();
}

void FfmpegVulkanDevice::reset() {
  if (!ref_) return;
  auto* ctx = reinterpret_cast<AVHWDeviceContext*>(ref_->data);
  if (ctx && ctx->hwctx) {
    auto* vk = static_cast<AVVulkanDeviceContext*>(ctx->hwctx);
    // FFmpeg's default free DestroyDevice/DestroyInstance — we do not own them.
    vk->act_dev = VK_NULL_HANDLE;
    vk->inst = VK_NULL_HANDLE;
    vk->phys_dev = VK_NULL_HANDLE;
#if FF_API_VULKAN_SYNC_QUEUES
    vk->lock_queue = nullptr;
    vk->unlock_queue = nullptr;
#endif
  }
  ctx->user_opaque = nullptr;
  av_buffer_unref(&ref_);
  ref_ = nullptr;
  queue_mu_ = nullptr;
}

bool FfmpegVulkanDevice::init(VkInstance inst, VkPhysicalDevice phys, VkDevice act_dev,
                              uint32_t graphics_qf, uint32_t graphics_queue_count, uint32_t decode_qf,
                              uint32_t decode_queue_count, std::mutex* queue_mu,
                              const std::vector<const char*>& enabled_dev_exts) {
  reset();
  last_error_.clear();
  if (!inst || !phys || !act_dev) {
    last_error_ = "null Vulkan handles";
    return false;
  }

  AVBufferRef* ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
  if (!ref) {
    last_error_ = "av_hwdevice_ctx_alloc(VULKAN) failed";
    return false;
  }
  auto* ctx = reinterpret_cast<AVHWDeviceContext*>(ref->data);
  auto* vk = static_cast<AVVulkanDeviceContext*>(ctx->hwctx);

  vk->get_proc_addr = vkGetInstanceProcAddr;
  vk->inst = inst;
  vk->phys_dev = phys;
  vk->act_dev = act_dev;
  vk->alloc = nullptr;

  feats13_ = {};
  feats13_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
  feats13_.synchronization2 = VK_TRUE;
  feats12_ = {};
  feats12_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  feats12_.timelineSemaphore = VK_TRUE;
  feats12_.pNext = &feats13_;
  feats2_ = {};
  feats2_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  feats2_.pNext = &feats12_;
  vk->device_features = feats2_;

  // Extension name list must be av_malloc'd — FFmpeg frees it on context destroy.
  if (!enabled_dev_exts.empty()) {
    auto** names = static_cast<const char**>(av_malloc(sizeof(char*) * enabled_dev_exts.size()));
    if (!names) {
      av_buffer_unref(&ref);
      last_error_ = "oom ext list";
      return false;
    }
    for (size_t i = 0; i < enabled_dev_exts.size(); ++i) {
      names[i] = av_strdup(enabled_dev_exts[i]);
      if (!names[i]) {
        for (size_t j = 0; j < i; ++j) av_free(const_cast<char*>(names[j]));
        av_free(names);
        av_buffer_unref(&ref);
        last_error_ = "oom ext strdup";
        return false;
      }
    }
    vk->enabled_dev_extensions = names;
    vk->nb_enabled_dev_extensions = static_cast<int>(enabled_dev_exts.size());
  }
  vk->enabled_inst_extensions = nullptr;
  vk->nb_enabled_inst_extensions = 0;

  vk->nb_qf = 0;
  auto push_qf = [&](uint32_t idx, uint32_t num, VkQueueFlagBits flags,
                     VkVideoCodecOperationFlagBitsKHR caps) {
    if (vk->nb_qf >= 64 || idx == UINT32_MAX || num == 0) return;
    AVVulkanDeviceQueueFamily& e = vk->qf[vk->nb_qf++];
    e.idx = static_cast<int>(idx);
    e.num = static_cast<int>(num);
    e.flags = flags;
    e.video_caps = caps;
  };

  VkQueueFlagBits gflags = static_cast<VkQueueFlagBits>(
      VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT | VK_QUEUE_COMPUTE_BIT);
  push_qf(graphics_qf, std::max(1u, graphics_queue_count), gflags,
          static_cast<VkVideoCodecOperationFlagBitsKHR>(0));

  if (decode_qf != UINT32_MAX && decode_queue_count > 0) {
    VkVideoCodecOperationFlagBitsKHR caps = static_cast<VkVideoCodecOperationFlagBitsKHR>(0);
#if defined(VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR)
    caps = static_cast<VkVideoCodecOperationFlagBitsKHR>(
        caps | VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR);
#endif
#if defined(VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR)
    caps = static_cast<VkVideoCodecOperationFlagBitsKHR>(
        caps | VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR);
#endif
#if defined(VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR)
    caps = static_cast<VkVideoCodecOperationFlagBitsKHR>(
        caps | VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR);
#endif
    push_qf(decode_qf, decode_queue_count, VK_QUEUE_VIDEO_DECODE_BIT_KHR, caps);
  }

#if FF_API_VULKAN_FIXED_QUEUES
  vk->queue_family_index = static_cast<int>(graphics_qf);
  vk->nb_graphics_queues = static_cast<int>(std::max(1u, graphics_queue_count));
  vk->queue_family_tx_index = static_cast<int>(graphics_qf);
  vk->nb_tx_queues = 1;
  vk->queue_family_comp_index = static_cast<int>(graphics_qf);
  vk->nb_comp_queues = static_cast<int>(std::max(1u, graphics_queue_count));
  vk->queue_family_encode_index = -1;
  vk->nb_encode_queues = 0;
  vk->queue_family_decode_index =
      decode_qf != UINT32_MAX ? static_cast<int>(decode_qf) : -1;
  vk->nb_decode_queues = decode_qf != UINT32_MAX ? static_cast<int>(decode_queue_count) : 0;
#endif

#if FF_API_VULKAN_SYNC_QUEUES
  queue_mu_ = queue_mu;
  ctx->user_opaque = this;
  vk->lock_queue = &FfmpegVulkanDevice::lock_queue;
  vk->unlock_queue = &FfmpegVulkanDevice::unlock_queue;
#else
  (void)queue_mu;
  queue_mu_ = queue_mu;
  ctx->user_opaque = this;
#endif

  const int err = av_hwdevice_ctx_init(ref);
  if (err < 0) {
    char buf[128];
    av_strerror(err, buf, sizeof(buf));
    last_error_ = std::string("av_hwdevice_ctx_init: ") + buf;
    // Null handles before unref so FFmpeg does not Destroy our XR device.
    vk->act_dev = VK_NULL_HANDLE;
    vk->inst = VK_NULL_HANDLE;
    vk->phys_dev = VK_NULL_HANDLE;
    av_buffer_unref(&ref);
    queue_mu_ = nullptr;
    return false;
  }

  ref_ = ref;
  VRP_LOG("FFmpeg Vulkan hwctx: injected XR device (graphics_qf=%u decode_qf=%u exts=%zu)",
          graphics_qf, decode_qf, enabled_dev_exts.size());
  return true;
}

}  // namespace vrp

#endif  // VRP_HAS_FFMPEG
