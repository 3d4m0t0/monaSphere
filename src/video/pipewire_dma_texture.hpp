#pragma once

#include "capture/pipewire_capture.hpp"
#include "common.hpp"

#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace vrp {

/**
 * Single-plane PipeWire ScreenCast dma-buf (BGRA/XRGB/…) → sampled VkImage.
 *
 * Staging + fixed present (two private VkImages):
 *  - view() / display_slot_ always slots_[0] (present). Never ping-pong the
 *    bound image — latching 0↔1 every commit looked like ABAB flicker.
 *  - try_import copy-out → slots_[1] (staging). Promote → slots_[0] after
 *    eye_done + present hold so pool BO rotation cannot thrash every XR commit.
 *  - Host skips while Live FSR samples present.
 *  - pw_buffer held until staging blit completes (PipewireCapture hold Cap=2).
 *
 * Root cause of Live "old frame / reorder" (Mutter GNOME RecordVirtual):
 *  dma_seq and pool key (p#) stay monotonic, but Mutter may queue an unpainted
 *  recycled BO (empty VideoDamage, or chunk size=0 / CORRUPTED on CURSOR_ONLY).
 *  Copy-out of that BO pastes stale scene pixels into staging→present. Capture
 *  must skip those buffers (keep present[0], pw_stream_queue_buffer) before
 *  publish; SyncTimeline / EXPORT_SYNC_FILE alone do not fix unpainted content.
 *
 * NVIDIA: never sample producer BOs zero-copy. Same copy-out path on other GPUs.
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

  /**
   * DRM modifiers this GPU can sample as single-plane BGRA/RGBA (for PipeWire
   * EnumFormat). Always includes LINEAR. Empty phys → {LINEAR}.
   */
  static std::vector<uint64_t> query_sampleable_modifiers(VkPhysicalDevice phys);

  bool enabled() const { return enabled_; }
  bool has_valid_frame() const { return valid_.load(std::memory_order_acquire); }
  int width() const { return width_; }
  int height() const { return height_; }
  bool nvidia() const { return nvidia_; }
  const std::vector<uint64_t>& sampleable_modifiers() const { return sampleable_mods_; }
  /** True after a hard import failure — host must stop calling try_import. */
  bool import_abandoned() const { return abandoned_.load(std::memory_order_acquire); }

  VkImageView view() const;
  VkSampler sampler() const { return sampler_; }

  /** Import a dma-buf frame. Does not take ownership of `frame.fd` (dups internally).
   *  Soft skips (unstable / waiting for modifier) return false without abandoning.
   *  May update staging only (present held) and still return true. */
  bool try_import(const PipewireDmaFrame& frame);

  /**
   * If staging is ahead of present and eye/hold gates allow, promote staging→present.
   * Call once per XR tick so a hold-skipped stage still reaches the HMD.
   */
  bool try_promote_pending();

  bool poll_present();
  void reset();

  /**
   * Prior XR eye submit finished (host: wait_previous_submit). Advances the
   * per-slot epoch so a previously displayed slot may be overwritten.
   */
  void mark_eye_done();
  /** True if the present image is still busy for the next import. */
  bool pending_eye_retire() const;

  /** Invoked when a slot's imported dma-buf is destroyed (seq from PipewireDmaFrame). */
  void set_release_seq_callback(std::function<void(uint64_t)> cb) { release_seq_ = std::move(cb); }

  const std::string& last_error() const { return last_error_; }

  /** Display diag (same role as VideoTexture slot_content_* for SHM). */
  int display_slot() const { return display_slot_.load(std::memory_order_acquire); }
  /** Slot last try_import wrote (staging = 1). Present is always 0. */
  int last_write_slot() const { return last_write_slot_; }
  uint64_t display_seq() const;
  uint64_t slot_content_seq(int slot) const;
  uint64_t slot_content_key(int slot) const;
  /** SPA_META_Header carried with the last copy into this slot (0 if absent). */
  uint64_t slot_spa_seq(int slot) const;
  int64_t slot_spa_pts(int slot) const;
  bool slot_has_spa(int slot) const;
  /** fstat identity of a dma-buf FD (pool recycle key). */
  static bool dma_buf_key(int fd, uint64_t* out_key);

 private:
  struct Slot {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    /** fstat identity of imported dma-buf; 0 = unknown / not reusable. */
    uint64_t buf_key = 0;
    /** PipewireDmaFrame.seq — release PW buffer when this slot is destroyed. */
    uint64_t pw_seq = 0;
    /** Last PW seq / pool key copied into this private image. */
    uint64_t content_seq = 0;
    uint64_t content_key = 0;
    /** Producer SPA_META_Header (content generation; independent of dma-buf BO id). */
    uint64_t spa_seq = 0;
    int64_t spa_pts = 0;
    bool has_spa_meta = false;
    /** Private OPTIMAL copy; safe after producer rewrites dma-buf. */
    bool is_private = false;
    /**
     * Slot may be overwritten only when eye_done_epoch_ >= busy_until_epoch_.
     * Set to eye_done_epoch_+1 when content is installed for display.
     */
    uint64_t busy_until_epoch = 0;
    int priv_w = 0;
    int priv_h = 0;
    VkFormat priv_fmt = VK_FORMAT_UNDEFINED;
    /** ImageView A=ONE (XRGB/XBGR). Copied present must match staging. */
    bool force_opaque = false;
  };

  bool enabled_ = false;
  bool dying_ = false;
  bool nvidia_ = false;
  int width_ = 0;
  int height_ = 0;
  std::string last_error_;
  std::vector<uint64_t> sampleable_mods_;

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

  // slots_[0] = present (always sampled). slots_[1] = staging (dma copy-out).
  Slot slots_[2]{};
  std::atomic<int> display_slot_{0};
  std::atomic<bool> valid_{false};
  std::atomic<bool> abandoned_{false};
  std::atomic<uint64_t> present_gen_{0};
  uint64_t bound_gen_ = 0;
  bool import_logged_ = false;
  int last_write_slot_ = 0;
  /** Advanced in mark_eye_done; compared to Slot::busy_until_epoch. */
  uint64_t eye_done_epoch_ = 0;
  /**
   * Present may be promoted only when eye_done_epoch_ >= this.
   * Set to eye_done+kPresentHoldEpochs on each successful promote so pool-BO
   * thrash cannot flash a new image every XR commit.
   */
  uint64_t present_hold_until_epoch_ = 0;
  static constexpr uint64_t kPresentHoldEpochs = 2;
  /** Once EXPORT_SYNC_FILE has succeeded, later ioctl-fail soft-skips the blit. */
  bool dmabuf_sync_ever_ok_ = false;

  // Arming: identical geometry/modifier for N frames before first Vulkan import.
  int arm_w_ = 0;
  int arm_h_ = 0;
  uint64_t arm_mod_ = 0;
  uint32_t arm_fourcc_ = 0;
  int arm_hits_ = 0;
  std::function<void(uint64_t)> release_seq_;

  void set_error(const std::string& e);
  void abandon(const std::string& e);
  void destroy_slot(Slot& slot);
  bool wait_fence();
  bool frame_ready_to_import(const PipewireDmaFrame& frame, uint64_t* out_mod) const;
  bool modifier_ok(uint64_t modifier, VkFormat format,
                   VkExternalMemoryHandleTypeFlagBits handle_type) const;
  bool probe_handle_type(int fd, VkExternalMemoryHandleTypeFlagBits* out_type,
                         VkMemoryFdPropertiesKHR* out_props) const;
  bool import_image(Slot& slot, int fd, uint64_t modifier, int width, int height, VkFormat format,
                    bool force_opaque_alpha, VkDeviceSize offset, VkDeviceSize row_pitch,
                    VkDeviceSize size, bool require_transfer_src = false);
  /** First-time layout: UNDEFINED → SHADER_READ_ONLY. */
  bool transition_slot(const Slot& slot);
  bool ensure_private_slot(Slot& slot, int w, int h, VkFormat format, bool force_opaque_alpha);
  /** Import external → blit into private slot → drop import; releases frame.seq. */
  bool import_copy_out(Slot& dst, const PipewireDmaFrame& frame, uint64_t mod, VkFormat format,
                       bool force_opaque_alpha);
  /** GPU copy staging → present (both private OPTIMAL images). */
  bool promote_staging_to_present();
  /** Shared gate + promote + present_gen bump. */
  bool promote_staging_if_allowed();
  /** @param force_opaque_alpha XRGB/XBGR → ImageView A=ONE (Flat uses SRC_ALPHA blend). */
  static bool map_drm_to_vk(uint32_t drm_fourcc, VkFormat& out_format, bool& force_opaque_alpha);
};

}  // namespace vrp
