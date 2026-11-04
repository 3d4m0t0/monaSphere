#pragma once

#include "common.hpp"

#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

struct AVFrame;

namespace vrp {

/**
 * Vulkan Video zero-copy — mailbox (not a 3-deep ring).
 *
 * Timeline state machine (strict, not timing-based):
 *   Decoded(D) --wait--> Waited(D) --consumer +1--> Released(D+1) --unref--> Freed
 *   Waited(D)  --mailbox ref--> Borrowed; acquire waits D only (no signal);
 *              --return--> Released(D+1)
 *   Exactly one consumer +1 per Waited epoch (ledger). Discard/skip must +1 too —
 *   wait-only discard left need=D+2 cur=D under scrub.
 *
 * Seek: host eye_done → decode release_all_for_flush + recycle ff_->frame → flush.
 * Never force_resume during seek_flush/seek_apply. Never CPU-FORCE decode complete.
 */
class VulkanVideoTexture {
 public:
  bool init(VkPhysicalDevice phys, VkDevice device, VkQueue queue, uint32_t queue_family,
            std::mutex* queue_mu);
  void shutdown();

  bool enabled() const { return enabled_; }
  bool full_range() const { return full_range_.load(std::memory_order_acquire); }
  bool has_valid_frame() const { return valid_.load(std::memory_order_acquire); }
  int width() const { return width_; }
  int height() const { return height_; }

  VkImageView y_view() const;
  VkImageView uv_view() const;
  VkSampler sampler() const { return sampler_; }
  VkImageLayout sample_layout() const { return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }

  /**
   * wait_ns: CPU wait for decode timeline before parking at back.
   * 0 = wait until signalled (or present_abort). Never use a short timeout before
   * avcodec_flush_buffers — that wedges FFmpeg Vulkan inside WaitForFences forever.
   * If wait_why != nullptr, set to "ok"|"abort"|"timeout"|"nosem"|"bad".
   */
  bool try_present(const AVFrame* vulkan_frame, bool full_range_hint,
                   uint64_t wait_ns = 0, const char** wait_why = nullptr);
  /** Decode-thread: wait AVVkFrame timeline (0 = infinite). Safe before flush/unref. */
  bool wait_decode_done(const AVFrame* frame, uint64_t wait_ns = 0, const char** why = nullptr);
  /**
   * Wait decode (if needed) then post the sole consumer +1 for this frame epoch.
   * Use for skip/discard/seek-bail and before av_frame_unref of ff_->frame.
   * No-ops if mailbox already released the same AVVkFrame epoch.
   */
  bool recycle_frame(AVFrame* frame, uint64_t wait_ns = 0, const char** why = nullptr);
  bool poll_present();
  bool pending_flip() const;
  uint64_t present_generation() const { return present_gen_.load(std::memory_order_acquire); }
  int display_index() const;
  int sampling_index() const;

  bool prepare_display();
  void mark_eye_done();

  std::string debug_state() const;

  bool try_unwedge_acquire();
  bool recover_from_stall();
  /** If front is eye-done, return it to FFmpeg (blank one frame) to free DPB. */
  bool kick_decode_stall();
  /** Abort in-flight decode-thread present waits (call before halt/join). */
  void set_present_abort(bool abort) { present_abort_.store(abort, std::memory_order_release); }
  bool present_abort() const { return present_abort_.load(std::memory_order_acquire); }
  /** Legacy hook (unused for wait abort — seek must not interrupt decode timeline). */
  void set_seek_interrupt(std::atomic<bool>* flag) { seek_interrupt_ = flag; }
  void release_held();
  /**
   * Return every mailbox AVVkFrame before avcodec_flush_buffers.
   * Never defers: holding a ref across flush wedges FFmpeg Vulkan
   * WaitForFences(UINT64_MAX) (phase=seek_flush forever).
   */
  void release_all_for_flush();
  /** Drain device queues before codec reset (timed; may detach if wedged). */
  void sync_gpu_for_seek_reset();
  /**
   * Submit a no-op on the graphics queue so PowerMizer AUTO can raise clocks while
   * seek has paused demux/play (otherwise decode CB can sit at need=cur+1 for seconds).
   */
  void kick_gpu_activity();
  /** Free mailbox back only — keeps front on screen (scrub DPB relief). */
  void release_back();
  void reset();
  const std::string& last_error() const { return last_error_; }

 private:
  struct Slot {
    VkImageView y = VK_NULL_HANDLE;
    VkImageView uv = VK_NULL_HANDLE;
    AVFrame* held = nullptr;
    VkSemaphore sem[2]{};
    uint64_t sem_value[2]{};
    uint32_t sem_count = 0;
    bool signaled = true;
    bool acquired = false;
    bool prepared = false;
    bool eye_done = false;
    bool decode_done = false;  // set only after CPU wait on decode thread
    int w = 0;
    int h = 0;
  };

  bool rebuild_views(Slot& slot, AVFrame* frame);
  void destroy_views(Slot& slot);
  void clear_slot_meta(Slot& slot);
  bool return_to_ffmpeg(Slot& slot, uint64_t wait_ns);
  bool wait_decode_ready(const Slot& slot, uint64_t timeout_ns);
  bool decode_ready_now(const Slot& slot) const;
  /** Decode-thread: wait AVVkFrame timeline before we take a hold. */
  bool wait_avframe_decode(const AVFrame* frame, uint64_t timeout_ns, const char** why);
  bool acquire_gpu(Slot& slot);
  /** Wait for layout/kick fence. timeout_ns=0 polls once. On timeout leave pending. */
  void drain_layout_fence(uint64_t timeout_ns = 2'000'000'000ull);
  bool layout_fence_idle();
  void sync_vkf_sem(Slot& slot);
  bool host_signal_slot(Slot& slot);
  /** Signal decode_val+1 exactly once per AVVkFrame epoch (thread-safe ledger). */
  bool consumer_signal_vkf(void* vkf_key, VkSemaphore sem, uint64_t decode_val,
                           uint64_t* vkf_sem_value_out);
  bool force_decode_timeline(Slot& slot);
  bool abandon_slot_locked(Slot& slot);
  bool recover_from_stall_unlocked();
  std::string debug_state_locked() const;
  void note_waited_decode(const void* vkf_key, uint64_t decode_val);

  bool enabled_ = false;
  std::atomic<bool> full_range_{false};
  std::atomic<bool> valid_{false};
  int width_ = 0;
  int height_ = 0;
  std::string last_error_;

  VkPhysicalDevice phys_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t queue_family_ = 0;
  std::mutex* queue_mu_ = nullptr;
  VkSampler sampler_ = VK_NULL_HANDLE;
  VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
  bool fence_pending_ = false;
  VkSemaphore pending_wait_sem_[2]{};
  uint64_t pending_wait_val_[2]{};
  uint32_t pending_wait_count_ = 0;

  Slot front_{};  // acquired + sampled by eye/FSR
  Slot back_{};   // decode-complete mailbox only
  /** Seek asked to drop front while the current eye submit still samples it. */
  bool deferred_drop_front_ = false;
  std::atomic<bool> present_abort_{false};
  std::atomic<bool>* seek_interrupt_ = nullptr;  // not owned (decoder seek_req_)
  std::atomic<uint64_t> present_gen_{0};
  uint64_t bound_gen_ = 0;
  mutable std::mutex mu_;
  /** AVVkFrame* → decode timeline value last waited / last consumer-signalled. */
  std::mutex timeline_mu_;
  std::unordered_map<const void*, uint64_t> waited_decode_;
  std::unordered_map<const void*, uint64_t> signaled_upto_;
};

}  // namespace vrp
