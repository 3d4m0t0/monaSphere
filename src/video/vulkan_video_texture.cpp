#include "video/vulkan_video_texture.hpp"

#include "common.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <sstream>
#include <thread>
#include <utility>

#if defined(VRP_HAS_FFMPEG)
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/pixdesc.h>
}
#endif

namespace vrp {

bool VulkanVideoTexture::init(VkPhysicalDevice phys, VkDevice device, VkQueue queue,
                              uint32_t queue_family, std::mutex* queue_mu) {
  shutdown();
  phys_ = phys;
  device_ = device;
  queue_ = queue;
  queue_family_ = queue_family;
  queue_mu_ = queue_mu;
  if (!phys_ || !device_ || !queue_) {
    last_error_ = "null phys/device/queue";
    return false;
  }

  VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sci.magFilter = VK_FILTER_LINEAR;
  sci.minFilter = VK_FILTER_LINEAR;
  sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (vkCreateSampler(device_, &sci, nullptr, &sampler_) != VK_SUCCESS) {
    last_error_ = "sampler";
    return false;
  }

  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = queue_family_;
  if (vkCreateCommandPool(device_, &pci, nullptr, &cmd_pool_) != VK_SUCCESS) {
    last_error_ = "cmd pool";
    shutdown();
    return false;
  }
  VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = cmd_pool_;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = 1;
  if (vkAllocateCommandBuffers(device_, &cai, &cmd_) != VK_SUCCESS) {
    last_error_ = "cmd buf";
    shutdown();
    return false;
  }
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (vkCreateFence(device_, &fci, nullptr, &fence_) != VK_SUCCESS) {
    last_error_ = "fence";
    shutdown();
    return false;
  }

  enabled_ = true;
  VRP_LOG("VulkanVideoTexture: armed (AVVkFrame mailbox: front+back, decode-ready only)");
  return true;
}

bool VulkanVideoTexture::layout_fence_idle() {
  if (!device_ || !fence_ || !fence_pending_) return true;
  const VkResult st = vkGetFenceStatus(device_, fence_);
  if (st == VK_SUCCESS) {
    fence_pending_ = false;
    pending_wait_count_ = 0;
    return true;
  }
  return false;
}

void VulkanVideoTexture::drain_layout_fence(uint64_t timeout_ns) {
  if (!device_ || !fence_ || !fence_pending_) return;
  // Acquire posts the sole consumer timeline +1. Never force-clear this fence on a
  // short miss during play — CPU-signalling ahead of GPU recreates need>cur stalls.
  const VkResult wr = vkWaitForFences(device_, 1, &fence_, VK_TRUE, timeout_ns);
  if (wr != VK_SUCCESS) {
    (void)recover_from_stall_unlocked();
    const uint64_t retry_ns =
        timeout_ns > 0 ? std::min(timeout_ns, uint64_t{200'000'000ull}) : uint64_t{0};
    const VkResult wr2 = vkWaitForFences(device_, 1, &fence_, VK_TRUE, retry_ns);
    if (wr2 != VK_SUCCESS) {
      VRP_ERR("VulkanVideoTexture: layout fence FAILED (%d) — leave pending (no host_signal leap)",
              static_cast<int>(wr2));
      return;
    }
  }
  fence_pending_ = false;
  pending_wait_count_ = 0;
}

void VulkanVideoTexture::shutdown() {
  // Teardown must not burn the XR join budget (was 2s+8s). kick_gpu leaves this
  // fence pending; a wedged graphics queue never signals it.
  drain_layout_fence(100'000'000ull);
  {
    std::lock_guard<std::mutex> lock(mu_);
    // Abandon mailbox without long timeline waits — device is going away.
    if (front_.held) abandon_slot_locked(front_);
    if (back_.held) abandon_slot_locked(back_);
    deferred_drop_front_ = false;
    width_ = height_ = 0;
    valid_.store(false);
    present_gen_.store(0);
    bound_gen_ = 0;
    {
      std::lock_guard<std::mutex> tlock(timeline_mu_);
      waited_decode_.clear();
      signaled_upto_.clear();
    }
    fence_pending_ = false;
    pending_wait_count_ = 0;
  }
  if (device_) {
    if (fence_) vkDestroyFence(device_, fence_, nullptr);
    if (cmd_pool_) vkDestroyCommandPool(device_, cmd_pool_, nullptr);
    if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
  }
  fence_ = VK_NULL_HANDLE;
  cmd_ = VK_NULL_HANDLE;
  cmd_pool_ = VK_NULL_HANDLE;
  sampler_ = VK_NULL_HANDLE;
  device_ = VK_NULL_HANDLE;
  queue_ = VK_NULL_HANDLE;
  queue_mu_ = nullptr;
  phys_ = VK_NULL_HANDLE;
  enabled_ = false;
}

void VulkanVideoTexture::destroy_views(Slot& slot) {
  if (!device_) return;
  if (slot.y) vkDestroyImageView(device_, slot.y, nullptr);
  if (slot.uv) vkDestroyImageView(device_, slot.uv, nullptr);
  slot.y = slot.uv = VK_NULL_HANDLE;
}

void VulkanVideoTexture::clear_slot_meta(Slot& slot) {
  slot.sem_count = 0;
  slot.signaled = true;
  slot.acquired = false;
  slot.prepared = false;
  slot.eye_done = false;
  slot.decode_done = false;
  slot.w = slot.h = 0;
}

void VulkanVideoTexture::sync_vkf_sem(Slot& slot) {
#if defined(VRP_HAS_FFMPEG)
  if (!slot.held || !slot.held->data[0]) return;
  auto* vkf = reinterpret_cast<AVVkFrame*>(slot.held->data[0]);
  if (!vkf) return;
  for (uint32_t i = 0; i < slot.sem_count && i < AV_NUM_DATA_POINTERS; ++i)
    vkf->sem_value[i] = slot.sem_value[i];
#else
  (void)slot;
#endif
}

bool VulkanVideoTexture::wait_decode_ready(const Slot& slot, uint64_t timeout_ns) {
#if defined(VRP_HAS_FFMPEG)
  if (slot.sem_count == 0) return true;
  VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
  wi.semaphoreCount = slot.sem_count;
  wi.pSemaphores = slot.sem;
  wi.pValues = slot.sem_value;
  return vkWaitSemaphores(device_, &wi, timeout_ns) == VK_SUCCESS;
#else
  (void)slot;
  (void)timeout_ns;
  return true;
#endif
}

bool VulkanVideoTexture::decode_ready_now(const Slot& slot) const {
#if defined(VRP_HAS_FFMPEG)
  if (!device_ || slot.sem_count == 0) return true;
  for (uint32_t i = 0; i < slot.sem_count; ++i) {
    if (!slot.sem[i]) continue;
    uint64_t cur = 0;
    if (vkGetSemaphoreCounterValue(device_, slot.sem[i], &cur) != VK_SUCCESS) return false;
    if (cur < slot.sem_value[i]) return false;
  }
  return true;
#else
  (void)slot;
  return true;
#endif
}

bool VulkanVideoTexture::wait_avframe_decode(const AVFrame* frame, uint64_t timeout_ns,
                                            const char** why) {
#if defined(VRP_HAS_FFMPEG)
  auto set_why = [&](const char* w) {
    if (why) *why = w;
  };
  if (!device_ || !frame || !frame->data[0]) {
    set_why("bad");
    return false;
  }
  auto* vkf = reinterpret_cast<AVVkFrame*>(frame->data[0]);
  if (!vkf) {
    set_why("bad");
    return false;
  }
  VkSemaphore sems[2]{};
  uint64_t vals[2]{};
  uint32_t n = 0;
  const bool multiplane = (vkf->img[1] == VK_NULL_HANDLE);
  for (int i = 0; i < AV_NUM_DATA_POINTERS && n < 2; ++i) {
    if (!vkf->img[i] && i > 0) break;
    if (!vkf->sem[i]) {
      if (i == 0) break;
      continue;
    }
    sems[n] = vkf->sem[i];
    vals[n] = vkf->sem_value[i];
    ++n;
    if (multiplane) break;
  }
  if (n == 0) {
    set_why("nosem");
    return true;
  }
  // Poll so halt/open_media can abort. Do NOT abort on seek_req and do not use a
  // short timeout: returning before the decode timeline is signalled desyncs the
  // DPB; the next avcodec_flush_buffers / send_packet then hangs in
  // WaitForFences(UINT64_MAX) (seekdbg: phase=seek_av_seek forever).
  const bool infinite = (timeout_ns == 0);
  const auto t0 = std::chrono::steady_clock::now();
  const auto deadline = t0 + std::chrono::nanoseconds(infinite ? 0 : timeout_ns);
  auto next_stall_log = t0 + std::chrono::milliseconds(1000);
  VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
  wi.semaphoreCount = n;
  wi.pSemaphores = sems;
  wi.pValues = vals;
  for (;;) {
    if (present_abort_.load(std::memory_order_acquire)) {
      set_why("abort");
      return false;
    }
    const auto now = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(now - t0).count();
    // Newer seek_req while we wait (scrub stacked, or stop-seek during pre_flush):
    // leave without host-signalling need. Stale apply must not burn the full timeout.
    if (seek_interrupt_ && seek_interrupt_->load(std::memory_order_acquire) && ms >= 50.0) {
      uint64_t cur0 = 0;
      (void)vkGetSemaphoreCounterValue(device_, sems[0], &cur0);
      VRP_LOG("seekdbg: wait_avframe seek_intr %.0fms need=%llu cur=%llu | %s", ms,
              static_cast<unsigned long long>(vals[0]),
              static_cast<unsigned long long>(cur0), debug_state().c_str());
      set_why("seek_intr");
      return false;
    }
    // PowerMizer AUTO: with play/demux paused, clocks stay low and decode CB stalls.
    // Nudge the graphics queue periodically so clocks can ramp.
    if (ms >= 200.0 && now >= next_stall_log) {
      uint64_t cur0 = 0;
      (void)vkGetSemaphoreCounterValue(device_, sems[0], &cur0);
      VRP_LOG("seekdbg: wait_avframe stall %.0fms need=%llu cur=%llu | %s", ms,
              static_cast<unsigned long long>(vals[0]),
              static_cast<unsigned long long>(cur0), debug_state().c_str());
      kick_gpu_activity();
      next_stall_log = now + std::chrono::milliseconds(500);
    }
    if (!infinite) {
      if (now >= deadline) {
        set_why("timeout");
        return false;
      }
      const auto left = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count();
      const uint64_t slice = static_cast<uint64_t>(std::min<int64_t>(left, 5'000'000ll));
      if (vkWaitSemaphores(device_, &wi, slice) == VK_SUCCESS) {
        note_waited_decode(vkf, vals[0]);
        set_why("ok");
        return true;
      }
    } else {
      if (vkWaitSemaphores(device_, &wi, 5'000'000ull) == VK_SUCCESS) {
        note_waited_decode(vkf, vals[0]);
        set_why("ok");
        return true;
      }
    }
  }
#else
  (void)frame;
  (void)timeout_ns;
  if (why) *why = "ok";
  return true;
#endif
}

void VulkanVideoTexture::note_waited_decode(const void* vkf_key, uint64_t decode_val) {
  if (!vkf_key) return;
  std::lock_guard<std::mutex> lock(timeline_mu_);
  waited_decode_[vkf_key] = decode_val;
}

bool VulkanVideoTexture::consumer_signal_vkf(void* vkf_key, VkSemaphore sem, uint64_t decode_val,
                                             uint64_t* vkf_sem_value_out) {
#if defined(VRP_HAS_FFMPEG)
  if (!device_ || !sem || !vkf_key) return false;
  const uint64_t target = decode_val + 1;
  {
    std::lock_guard<std::mutex> lock(timeline_mu_);
    const auto it = signaled_upto_.find(vkf_key);
    if (it != signaled_upto_.end() && it->second >= target) {
      if (vkf_sem_value_out) *vkf_sem_value_out = std::max(*vkf_sem_value_out, it->second);
      return true;
    }
  }
  uint64_t cur = 0;
  if (vkGetSemaphoreCounterValue(device_, sem, &cur) != VK_SUCCESS) return false;
  if (cur < target) {
    VkSemaphoreSignalInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
    si.semaphore = sem;
    si.value = target;
    const VkResult sr = vkSignalSemaphore(device_, &si);
    if (sr != VK_SUCCESS) {
      VRP_ERR("VulkanVideoTexture: consumer +1 failed need=%llu cur=%llu result=%d",
              static_cast<unsigned long long>(target), static_cast<unsigned long long>(cur),
              static_cast<int>(sr));
      return false;
    }
    cur = target;
  }
  {
    std::lock_guard<std::mutex> lock(timeline_mu_);
    auto& up = signaled_upto_[vkf_key];
    if (up < cur) up = cur;
    if (vkf_sem_value_out) *vkf_sem_value_out = std::max(*vkf_sem_value_out, up);
  }
  return true;
#else
  (void)vkf_key;
  (void)sem;
  (void)decode_val;
  (void)vkf_sem_value_out;
  return true;
#endif
}

bool VulkanVideoTexture::recycle_frame(AVFrame* frame, uint64_t wait_ns, const char** why) {
#if defined(VRP_HAS_FFMPEG)
  if (!frame || frame->format != AV_PIX_FMT_VULKAN || !frame->data[0]) {
    if (why) *why = "bad";
    return false;
  }
  const char* w = "ok";
  if (!wait_decode_done(frame, wait_ns, &w)) {
    if (why) *why = w;
    return false;
  }
  auto* vkf = reinterpret_cast<AVVkFrame*>(frame->data[0]);
  uint64_t decode_val = vkf->sem_value[0];
  {
    std::lock_guard<std::mutex> lock(timeline_mu_);
    const auto it = waited_decode_.find(vkf);
    if (it != waited_decode_.end()) decode_val = it->second;
  }
  if (!vkf->sem[0]) {
    if (why) *why = "nosem";
    return true;
  }
  if (!consumer_signal_vkf(vkf, vkf->sem[0], decode_val, &vkf->sem_value[0])) {
    if (why) *why = "signal";
    return false;
  }
  if (why) *why = "ok";
  return true;
#else
  (void)frame;
  (void)wait_ns;
  if (why) *why = "ok";
  return true;
#endif
}

bool VulkanVideoTexture::try_unwedge_acquire() {
#if defined(VRP_HAS_FFMPEG)
  std::lock_guard<std::mutex> lock(mu_);
  if (!device_ || !fence_pending_ || pending_wait_count_ == 0) return false;
  bool any = false;
  for (uint32_t i = 0; i < pending_wait_count_; ++i) {
    if (!pending_wait_sem_[i]) continue;
    uint64_t cur = 0;
    if (vkGetSemaphoreCounterValue(device_, pending_wait_sem_[i], &cur) != VK_SUCCESS) continue;
    if (cur >= pending_wait_val_[i]) continue;
    VkSemaphoreSignalInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
    si.semaphore = pending_wait_sem_[i];
    si.value = pending_wait_val_[i];
    const VkResult sr = vkSignalSemaphore(device_, &si);
    VRP_ERR("VulkanVideoTexture: unwedge acquire wait=%llu (gpu=%llu) result=%d",
            static_cast<unsigned long long>(pending_wait_val_[i]),
            static_cast<unsigned long long>(cur), static_cast<int>(sr));
    if (sr == VK_SUCCESS) any = true;
  }
  return any;
#else
  return false;
#endif
}

bool VulkanVideoTexture::force_decode_timeline(Slot& slot) {
#if defined(VRP_HAS_FFMPEG)
  if (!device_ || slot.sem_count == 0) return true;
  for (uint32_t i = 0; i < slot.sem_count; ++i) {
    if (!slot.sem[i]) continue;
    uint64_t cur = 0;
    if (vkGetSemaphoreCounterValue(device_, slot.sem[i], &cur) != VK_SUCCESS) return false;
    if (cur >= slot.sem_value[i]) continue;
    VkSemaphoreSignalInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
    si.semaphore = slot.sem[i];
    si.value = slot.sem_value[i];
    if (vkSignalSemaphore(device_, &si) != VK_SUCCESS) return false;
  }
  return true;
#else
  (void)slot;
  return true;
#endif
}

bool VulkanVideoTexture::abandon_slot_locked(Slot& slot) {
#if defined(VRP_HAS_FFMPEG)
  if (!slot.held) {
    clear_slot_meta(slot);
    return true;
  }
  if (!slot.signaled) {
    destroy_views(slot);
    // Always advance the consumer timeline before unref — freeing without +1
    // wedges the next decode / avcodec_flush_buffers forever.
    if (!host_signal_slot(slot)) {
      for (uint32_t i = 0; i < slot.sem_count; ++i) {
        if (!slot.sem[i] || !device_) continue;
        uint64_t cur = 0;
        if (vkGetSemaphoreCounterValue(device_, slot.sem[i], &cur) != VK_SUCCESS) continue;
        const uint64_t next = std::max(cur, slot.sem_value[i]) + 1;
        if (cur >= next) continue;
        VkSemaphoreSignalInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
        si.semaphore = slot.sem[i];
        si.value = next;
        (void)vkSignalSemaphore(device_, &si);
      }
      slot.signaled = true;
      VRP_ERR("VulkanVideoTexture: abandon forced consumer +1 after host_signal fail");
    }
  } else {
    destroy_views(slot);
  }
  av_frame_free(&slot.held);
  slot.held = nullptr;
  clear_slot_meta(slot);
  return true;
#else
  clear_slot_meta(slot);
  return true;
#endif
}

bool VulkanVideoTexture::recover_from_stall_unlocked() {
#if defined(VRP_HAS_FFMPEG)
  (void)layout_fence_idle();
  if (!fence_pending_ || pending_wait_count_ == 0) return false;
  bool any = false;
  for (uint32_t i = 0; i < pending_wait_count_; ++i) {
    if (!pending_wait_sem_[i]) continue;
    uint64_t cur = 0;
    if (vkGetSemaphoreCounterValue(device_, pending_wait_sem_[i], &cur) != VK_SUCCESS) continue;
    if (cur >= pending_wait_val_[i]) continue;
    VkSemaphoreSignalInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
    si.semaphore = pending_wait_sem_[i];
    si.value = pending_wait_val_[i];
    if (vkSignalSemaphore(device_, &si) == VK_SUCCESS) any = true;
  }
  return any;
#else
  return false;
#endif
}

bool VulkanVideoTexture::recover_from_stall() {
  std::lock_guard<std::mutex> lock(mu_);
  return recover_from_stall_unlocked();
}

bool VulkanVideoTexture::host_signal_slot(Slot& slot) {
#if defined(VRP_HAS_FFMPEG)
  if (slot.sem_count == 0) {
    slot.signaled = true;
    return true;
  }
  if (!slot.held || !slot.held->data[0]) {
    slot.signaled = true;
    return true;
  }
  auto* vkf = reinterpret_cast<AVVkFrame*>(slot.held->data[0]);
  // Snapshot decode value from park time (slot.sem_value). Prefer ledger waited
  // value so we never +1 from an already-advanced live vkf->sem_value (→ D+2).
  for (uint32_t i = 0; i < slot.sem_count; ++i) {
    if (!slot.sem[i]) continue;
    uint64_t decode_val = slot.sem_value[i];
    {
      std::lock_guard<std::mutex> lock(timeline_mu_);
      const auto it = waited_decode_.find(vkf);
      if (it != waited_decode_.end()) decode_val = it->second;
    }
    if (!consumer_signal_vkf(vkf, slot.sem[i], decode_val, &vkf->sem_value[i])) return false;
    slot.sem_value[i] = vkf->sem_value[i];
  }
  slot.signaled = true;
  return true;
#else
  slot.signaled = true;
  return true;
#endif
}

bool VulkanVideoTexture::return_to_ffmpeg(Slot& slot, uint64_t wait_ns) {
#if defined(VRP_HAS_FFMPEG)
  if (!slot.held) {
    clear_slot_meta(slot);
    return true;
  }
  if (!slot.signaled) {
    // Mailbox frames are decode_done; front is acquired. Wait only as safety net.
    if (!slot.acquired && !slot.decode_done) {
      if (!wait_decode_ready(slot, wait_ns)) return false;
    }
    destroy_views(slot);
    if (!host_signal_slot(slot)) return false;
  } else {
    destroy_views(slot);
  }
  av_frame_free(&slot.held);
  slot.held = nullptr;
  clear_slot_meta(slot);
  return true;
#else
  clear_slot_meta(slot);
  return true;
#endif
}

void VulkanVideoTexture::sync_gpu_for_seek_reset() {
#if defined(VRP_HAS_FFMPEG)
  // Do NOT vkDeviceWaitIdle here. When the video decode CB is wedged, WaitIdle never
  // returns; holding queue_mu_ across a timed detach permanently deadlocks XR submits
  // and FFmpeg lock_queue (same mutex) — reopen "succeeds" then everything freezes.
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (fence_pending_) {
      (void)recover_from_stall_unlocked();
      (void)layout_fence_idle();
    }
  }
  {
    std::lock_guard<std::mutex> tlock(timeline_mu_);
    waited_decode_.clear();
    signaled_upto_.clear();
  }
#else
  (void)0;
#endif
}

void VulkanVideoTexture::kick_gpu_activity() {
#if defined(VRP_HAS_FFMPEG)
  std::lock_guard<std::mutex> lock(mu_);
  if (!device_ || !queue_ || !cmd_ || !fence_) return;
  if (fence_pending_) {
    (void)recover_from_stall_unlocked();
    (void)layout_fence_idle();
    if (fence_pending_) return;
  }
  vkResetCommandBuffer(cmd_, 0);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(cmd_, &bi) != VK_SUCCESS) return;
  if (vkEndCommandBuffer(cmd_) != VK_SUCCESS) return;
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd_;
  std::unique_lock<std::mutex> qlock;
  if (queue_mu_) qlock = std::unique_lock<std::mutex>(*queue_mu_);
  if (vkQueueSubmit(queue_, 1, &si, fence_) != VK_SUCCESS) return;
  fence_pending_ = true;
  pending_wait_count_ = 0;
  // Non-blocking drain on next kick / acquire; brief poll so we do not pile up.
  (void)layout_fence_idle();
#endif
}

void VulkanVideoTexture::reset() {
  std::lock_guard<std::mutex> lock(mu_);
  drain_layout_fence();
  deferred_drop_front_ = false;
  if (!return_to_ffmpeg(front_, 200'000'000ull)) abandon_slot_locked(front_);
  if (!return_to_ffmpeg(back_, 200'000'000ull)) abandon_slot_locked(back_);
  width_ = height_ = 0;
  valid_.store(false);
  present_gen_.store(0);
  bound_gen_ = 0;
  {
    std::lock_guard<std::mutex> tlock(timeline_mu_);
    waited_decode_.clear();
    signaled_upto_.clear();
  }
}

void VulkanVideoTexture::release_held() {
  std::lock_guard<std::mutex> lock(mu_);
  const int had_f = front_.held ? 1 : 0;
  const int had_b = back_.held ? 1 : 0;
  drain_layout_fence();
  // Always free mailbox back (DPB pressure). Keep front while the current XR
  // submit still samples it — destroying views mid-pair makes L/R diverge.
  // Decode seek must use release_all_for_flush() instead (flush cannot wait on us).
  if (!return_to_ffmpeg(back_, 16'000'000ull)) abandon_slot_locked(back_);
  // If acquire fence is still pending, do not host_signal front (would leapfrog).
  if (fence_pending_ || (front_.held && !front_.eye_done)) {
    deferred_drop_front_ = front_.held != nullptr;
    VRP_LOG("seekdbg: release_held keep_front pending=%d eye_done=%d had_front=%d had_back=%d | %s",
            fence_pending_ ? 1 : 0, front_.eye_done ? 1 : 0, had_f, had_b,
            debug_state_locked().c_str());
    return;
  }
  deferred_drop_front_ = false;
  if (!return_to_ffmpeg(front_, 16'000'000ull)) abandon_slot_locked(front_);
  valid_.store(false, std::memory_order_release);
  present_gen_.fetch_add(1, std::memory_order_acq_rel);
  VRP_LOG("seekdbg: release_held had_front=%d had_back=%d | %s", had_f, had_b,
          debug_state_locked().c_str());
}

void VulkanVideoTexture::release_all_for_flush() {
  std::lock_guard<std::mutex> lock(mu_);
  const int had_f = front_.held ? 1 : 0;
  const int had_b = back_.held ? 1 : 0;
  deferred_drop_front_ = false;
  drain_layout_fence();
  if (fence_pending_) {
    VRP_ERR("seekdbg: release_all_for_flush — fence still pending, force stall recover");
    (void)recover_from_stall_unlocked();
    drain_layout_fence();
  }
  // Long waits: flush will block forever if any AVVkFrame ref remains.
  if (!return_to_ffmpeg(front_, 2'000'000'000ull)) abandon_slot_locked(front_);
  if (!return_to_ffmpeg(back_, 2'000'000'000ull)) abandon_slot_locked(back_);
  valid_.store(false, std::memory_order_release);
  present_gen_.fetch_add(1, std::memory_order_acq_rel);
  VRP_LOG("seekdbg: release_all_for_flush had_front=%d had_back=%d | %s", had_f, had_b,
          debug_state_locked().c_str());
}

void VulkanVideoTexture::release_back() {
  std::lock_guard<std::mutex> lock(mu_);
  if (!back_.held) {
    VRP_LOG("seekdbg: release_back (empty) | %s", debug_state_locked().c_str());
    return;
  }
  if (!return_to_ffmpeg(back_, 16'000'000ull)) abandon_slot_locked(back_);
  VRP_LOG("seekdbg: release_back | %s", debug_state_locked().c_str());
}

VkImageView VulkanVideoTexture::y_view() const {
  std::lock_guard<std::mutex> lock(mu_);
  return (front_.held && front_.acquired) ? front_.y : VK_NULL_HANDLE;
}
VkImageView VulkanVideoTexture::uv_view() const {
  std::lock_guard<std::mutex> lock(mu_);
  return (front_.held && front_.acquired) ? front_.uv : VK_NULL_HANDLE;
}

bool VulkanVideoTexture::pending_flip() const {
  std::lock_guard<std::mutex> lock(mu_);
  return back_.held != nullptr;
}

int VulkanVideoTexture::sampling_index() const {
  std::lock_guard<std::mutex> lock(mu_);
  return front_.held ? 0 : -1;
}

int VulkanVideoTexture::display_index() const {
  std::lock_guard<std::mutex> lock(mu_);
  if (back_.held) return 1;
  return front_.held ? 0 : -1;
}

bool VulkanVideoTexture::kick_decode_stall() {
  std::lock_guard<std::mutex> lock(mu_);
  // Front eye-done but no flip: return front so DPB can move (one blank frame).
  if (front_.held && front_.eye_done) {
    if (!return_to_ffmpeg(front_, 2'000'000ull)) abandon_slot_locked(front_);
    if (!back_.held) valid_.store(false, std::memory_order_release);
    VRP_LOG("vulkan mailbox: kick returned front | %s", debug_state_locked().c_str());
    return true;
  }
  return false;
}

#if defined(VRP_HAS_FFMPEG)
bool VulkanVideoTexture::rebuild_views(Slot& slot, AVFrame* frame) {
  destroy_views(slot);
  if (!frame || frame->format != AV_PIX_FMT_VULKAN || !frame->data[0]) return false;
  auto* vkf = reinterpret_cast<AVVkFrame*>(frame->data[0]);
  if (!vkf || !vkf->img[0]) return false;

  const int w = frame->width;
  const int h = frame->height;
  if (w < 2 || h < 2) return false;

  const bool multiplane = (vkf->img[1] == VK_NULL_HANDLE);
  VkFormat y_fmt = VK_FORMAT_R8_UNORM;
  VkFormat uv_fmt = VK_FORMAT_R8G8_UNORM;

  auto make_view = [&](VkImage image, VkFormat fmt, VkImageAspectFlags aspect,
                       VkImageView* out) -> bool {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange.aspectMask = aspect;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    return vkCreateImageView(device_, &vi, nullptr, out) == VK_SUCCESS;
  };

  if (multiplane) {
    if (!make_view(vkf->img[0], y_fmt, VK_IMAGE_ASPECT_PLANE_0_BIT, &slot.y)) return false;
    if (!make_view(vkf->img[0], uv_fmt, VK_IMAGE_ASPECT_PLANE_1_BIT, &slot.uv)) {
      destroy_views(slot);
      return false;
    }
  } else {
    if (!make_view(vkf->img[0], y_fmt, VK_IMAGE_ASPECT_COLOR_BIT, &slot.y)) return false;
    if (!make_view(vkf->img[1], uv_fmt, VK_IMAGE_ASPECT_COLOR_BIT, &slot.uv)) {
      destroy_views(slot);
      return false;
    }
  }

  slot.sem_count = 0;
  for (int i = 0; i < AV_NUM_DATA_POINTERS; ++i) {
    if (!vkf->img[i] && i > 0) break;
    if (!vkf->sem[i]) {
      if (i == 0) break;
      continue;
    }
    slot.sem[slot.sem_count] = vkf->sem[i];
    slot.sem_value[slot.sem_count] = vkf->sem_value[i];
    ++slot.sem_count;
    if (multiplane) break;
    if (slot.sem_count >= 2) break;
  }
  slot.w = w;
  slot.h = h;
  slot.prepared = false;
  slot.acquired = false;
  slot.eye_done = false;
  slot.signaled = false;
  slot.decode_done = false;
  return true;
}

bool VulkanVideoTexture::acquire_gpu(Slot& slot) {
  if (!slot.held || !slot.held->data[0] || !cmd_ || !fence_) return false;
  if (slot.acquired) {
    slot.prepared = true;
    return true;
  }

  auto* vkf = reinterpret_cast<AVVkFrame*>(slot.held->data[0]);
  if (!vkf || !vkf->img[0]) return false;

  // Mailbox invariant: decode already met on the decode thread.
  if (!slot.decode_done && !decode_ready_now(slot)) {
    if (!wait_decode_ready(slot, 2'000'000ull)) {
      VRP_LOG("vulkan acquire: unexpected unmet decode (mailbox breach)");
      return false;
    }
  }

  const bool multiplane = (vkf->img[1] == VK_NULL_HANDLE);
  const int nimg = multiplane ? 1 : (vkf->img[1] ? 2 : 1);

  bool need_barrier = false;
  for (int i = 0; i < nimg; ++i) {
    if (vkf->layout[i] != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) need_barrier = true;
    if (vkf->queue_family[i] != VK_QUEUE_FAMILY_IGNORED && vkf->queue_family[i] != queue_family_)
      need_barrier = true;
  }

  // Wait decode timeline only — do NOT signal +1 here. Publishing decode+1 at
  // submit time (before GPU completes) caused need=N cur=N-1 and flush hangs.
  // The sole consumer +1 is host_signal_slot on return_to_ffmpeg.
  uint64_t wait_vals[2]{};
  VkSemaphore wait_sems[2]{};
  uint32_t nsem = 0;
  for (uint32_t i = 0; i < slot.sem_count && nsem < 2; ++i) {
    if (!slot.sem[i]) continue;
    wait_sems[nsem] = slot.sem[i];
    wait_vals[nsem] = slot.sem_value[i];  // decode-done value; unchanged on vkf
    ++nsem;
  }

  if (!need_barrier && nsem == 0) {
    slot.acquired = true;
    slot.prepared = true;
    return true;
  }

  if (!layout_fence_idle()) return false;

  vkResetCommandBuffer(cmd_, 0);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(cmd_, &bi) != VK_SUCCESS) return false;

  if (need_barrier) {
    VkImageMemoryBarrier barriers[2]{};
    uint32_t nb = 0;
    for (int i = 0; i < nimg; ++i) {
      auto& b = barriers[nb++];
      b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
      b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      b.oldLayout = vkf->layout[i];
      b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      if (b.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED || b.oldLayout == VK_IMAGE_LAYOUT_PREINITIALIZED)
        b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
      const uint32_t src_qf = vkf->queue_family[i];
      if (src_qf != VK_QUEUE_FAMILY_IGNORED && src_qf != queue_family_) {
        b.srcQueueFamilyIndex = src_qf;
        b.dstQueueFamilyIndex = queue_family_;
      } else {
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      }
      b.image = vkf->img[i];
      b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      b.subresourceRange.levelCount = 1;
      b.subresourceRange.layerCount = 1;
    }
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, nb, barriers);
  } else {
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         0, 0, nullptr, 0, nullptr, 0, nullptr);
  }
  if (vkEndCommandBuffer(cmd_) != VK_SUCCESS) return false;

  VkTimelineSemaphoreSubmitInfo tsi{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd_;
  VkPipelineStageFlags wait_stages[2] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
  if (nsem > 0) {
    tsi.waitSemaphoreValueCount = nsem;
    tsi.pWaitSemaphoreValues = wait_vals;
    tsi.signalSemaphoreValueCount = 0;
    tsi.pSignalSemaphoreValues = nullptr;
    si.pNext = &tsi;
    si.waitSemaphoreCount = nsem;
    si.pWaitSemaphores = wait_sems;
    si.pWaitDstStageMask = wait_stages;
    si.signalSemaphoreCount = 0;
  }

  vkResetFences(device_, 1, &fence_);
  VkResult qr;
  {
    std::unique_lock<std::mutex> qlock;
    if (queue_mu_) qlock = std::unique_lock<std::mutex>(*queue_mu_);
    qr = vkQueueSubmit(queue_, 1, &si, fence_);
  }
  if (qr != VK_SUCCESS) {
    VRP_ERR("VulkanVideoTexture: acquire submit failed (%d)", static_cast<int>(qr));
    return false;
  }
  fence_pending_ = true;
  pending_wait_count_ = nsem;
  for (uint32_t i = 0; i < nsem; ++i) {
    pending_wait_sem_[i] = wait_sems[i];
    pending_wait_val_[i] = wait_vals[i];
  }
  // slot.sem_value / vkf->sem_value stay at decode-done; host_signal does +1.

  if (need_barrier) {
    for (int i = 0; i < nimg; ++i) {
      vkf->layout[i] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      vkf->access[i] = VK_ACCESS_SHADER_READ_BIT;
      if (vkf->queue_family[i] != VK_QUEUE_FAMILY_IGNORED) vkf->queue_family[i] = queue_family_;
    }
  }
  slot.acquired = true;
  slot.prepared = true;
  return true;
}

bool VulkanVideoTexture::wait_decode_done(const AVFrame* frame, uint64_t wait_ns,
                                          const char** why) {
  return wait_avframe_decode(frame, wait_ns, why);
}

bool VulkanVideoTexture::try_present(const AVFrame* vulkan_frame, bool full_range_hint,
                                     uint64_t wait_ns, const char** wait_why) {
  if (!enabled_ || !vulkan_frame) {
    if (wait_why) *wait_why = "bad";
    return false;
  }

  // Decode thread MUST wait for GPU decode before returning to receive_frame.
  // Dropping early races the GPU and can wedge the Vulkan DPB.
  const char* why = "ok";
  const auto t0 = std::chrono::steady_clock::now();
  if (!wait_avframe_decode(vulkan_frame, wait_ns, &why)) {
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    static std::atomic<int> late_n{0};
    const int n = late_n.fetch_add(1) + 1;
    if (n <= 12 || (n % 20) == 0 || std::strcmp(why, "seek_intr") == 0 ||
        std::strcmp(why, "abort") == 0) {
      VRP_LOG("seekdbg: wait_avframe fail why=%s elapsed=%.1fms budget=%.0fms #%d | %s", why, ms,
              static_cast<double>(wait_ns) / 1.0e6, n, debug_state().c_str());
    }
    if (wait_why) *wait_why = why;
    return false;
  }
  // Seek interrupted the wait — do not park; caller must not +1 until GPU hits need
  // (seek pre_flush recycle waits for the real timeline).
  if (why && std::strcmp(why, "seek_intr") == 0) {
    if (wait_why) *wait_why = why;
    return false;
  }
  if (wait_why) *wait_why = why;

  AVFrame* copy = av_frame_alloc();
  if (!copy) return false;
  if (av_frame_ref(copy, vulkan_frame) < 0) {
    av_frame_free(&copy);
    return false;
  }

  std::lock_guard<std::mutex> lock(mu_);
  // Replace previous back (already decode-done → return is host_signal only).
  if (back_.held) {
    if (!return_to_ffmpeg(back_, 2'000'000ull)) abandon_slot_locked(back_);
  }
  if (!rebuild_views(back_, copy)) {
    last_error_ = "plane views failed (format/tiling?)";
    av_frame_free(&copy);
    VRP_ERR("VulkanVideoTexture: %s", last_error_.c_str());
    return false;
  }
  back_.held = copy;
  back_.decode_done = true;  // waited above
  full_range_.store(full_range_hint, std::memory_order_release);
  width_ = back_.w;
  height_ = back_.h;
  valid_.store(true, std::memory_order_release);
  present_gen_.fetch_add(1, std::memory_order_acq_rel);
  return true;
}

bool VulkanVideoTexture::prepare_display() {
  if (!device_ || !enabled_) return false;
  std::unique_lock<std::mutex> lock(mu_);
  (void)layout_fence_idle();

  if (!back_.held) {
    // Retry acquire on front if prior fence was busy.
    if (front_.held && !front_.acquired) {
      Slot* retry = &front_;
      lock.unlock();
      return acquire_gpu(*retry) && retry->acquired;
    }
    return false;
  }

  // Keep showing front until the eye submit has sampled it.
  if (front_.held && !front_.eye_done) return false;

  // Take ownership of back before unlock so try_present cannot replace it mid-acquire.
  Slot promoting = back_;
  back_ = {};
  clear_slot_meta(back_);
  lock.unlock();

  if (!acquire_gpu(promoting)) {
    lock.lock();
    // Prefer newer mailbox frame if one arrived; else restore.
    if (back_.held) {
      abandon_slot_locked(promoting);
    } else {
      back_ = promoting;
    }
    return false;
  }

  lock.lock();
  Slot old = front_;
  front_ = promoting;
  front_.eye_done = false;

  if (old.held) {
    if (!return_to_ffmpeg(old, 2'000'000ull)) abandon_slot_locked(old);
  }
  return true;
}
#else
bool VulkanVideoTexture::rebuild_views(Slot&, AVFrame*) { return false; }
bool VulkanVideoTexture::acquire_gpu(Slot&) { return false; }
bool VulkanVideoTexture::wait_decode_done(const AVFrame*, uint64_t, const char** why) {
  if (why) *why = "ok";
  return true;
}
bool VulkanVideoTexture::recycle_frame(AVFrame*, uint64_t, const char** why) {
  if (why) *why = "ok";
  return true;
}
bool VulkanVideoTexture::try_present(const AVFrame*, bool, uint64_t, const char** why) {
  if (why) *why = "bad";
  return false;
}
bool VulkanVideoTexture::prepare_display() { return false; }
#endif

bool VulkanVideoTexture::poll_present() {
  const uint64_t g = present_gen_.load(std::memory_order_acquire);
  if (g == bound_gen_) return false;
  bound_gen_ = g;
  return true;
}

std::string VulkanVideoTexture::debug_state_locked() const {
  std::ostringstream os;
  os << "gen=" << present_gen_.load(std::memory_order_relaxed) << " fence=" << (fence_pending_ ? 1 : 0)
     << " front=";
  if (!front_.held) {
    os << "-";
  } else {
    os << (front_.decode_done ? 'D' : 'd') << (front_.acquired ? 'A' : 'a')
       << (front_.prepared ? 'P' : 'p') << (front_.eye_done ? 'E' : 'e')
       << (front_.signaled ? 'S' : 's');
  }
  os << " back=";
  if (!back_.held) {
    os << "-";
  } else {
    os << (back_.decode_done ? 'D' : 'd') << (back_.acquired ? 'A' : 'a')
       << (back_.prepared ? 'P' : 'p') << (back_.eye_done ? 'E' : 'e')
       << (back_.signaled ? 'S' : 's');
  }
  return os.str();
}

std::string VulkanVideoTexture::debug_state() const {
  std::lock_guard<std::mutex> lock(mu_);
  return debug_state_locked();
}

void VulkanVideoTexture::mark_eye_done() {
  std::lock_guard<std::mutex> lock(mu_);
  if (front_.held) front_.eye_done = true;
  if (!deferred_drop_front_ || !front_.held || !front_.eye_done) return;
  deferred_drop_front_ = false;
  if (fence_pending_) {
    // Acquire still in flight — retry after next eye fence.
    deferred_drop_front_ = true;
    return;
  }
  if (!return_to_ffmpeg(front_, 16'000'000ull)) abandon_slot_locked(front_);
  if (!back_.held) valid_.store(false, std::memory_order_release);
  present_gen_.fetch_add(1, std::memory_order_acq_rel);
  VRP_LOG("seekdbg: deferred front drop after eye_done | %s", debug_state_locked().c_str());
}

}  // namespace vrp
