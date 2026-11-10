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
 * Same ping-pong contract as VaapiNv12Texture: capture/import thread owns
 * try_import; XR thread only poll_present + view/sampler. On modifier or
 * format failure, try_import returns false and the host keeps CPU RGBA.
 *
 * Slot identity: PipeWire recycles a small dma-buf pool (same st_dev/st_ino).
 * The producer must not get a buffer back (queue_buffer) while a VkImage still
 * samples it — that rewrites displayed memory and looks like a short flicker /
 * dialog-close loop (stops when the cursor parks and damage ceases). Track
 * frame.seq per slot and invoke the release callback from destroy_slot.
 *
 * NVIDIA experimental notes (discard if unstable):
 *  - Probe OPAQUE_FD then DMA_BUF (or reverse) before vkAllocateMemory.
 *  - Always use VkImageDrmFormatModifierExplicitCreateInfoEXT with the
 *    producer modifier — never invent LINEAR when the SPA flag is missing.
 *  - Soft-skip unstable frames; hard-fail once → import_abandoned() so the
 *    host never retries (NVIDIA VkInstance import-poisoning).
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
   *  Soft skips (unstable / waiting for modifier) return false without abandoning. */
  bool try_import(const PipewireDmaFrame& frame);

  bool poll_present();
  void reset();

  /** Invoked when a slot's imported dma-buf is destroyed (seq from PipewireDmaFrame). */
  void set_release_seq_callback(std::function<void(uint64_t)> cb) { release_seq_ = std::move(cb); }

  const std::string& last_error() const { return last_error_; }

 private:
  struct Slot {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    /** fstat identity of imported dma-buf; 0 = unknown / not reusable. */
    uint64_t buf_key = 0;
    /** PipewireDmaFrame.seq — release PW buffer when this slot is destroyed. */
    uint64_t pw_seq = 0;
    /** Private OPTIMAL copy (not external); safe after producer rewrites dma-buf. */
    bool is_private = false;
    int priv_w = 0;
    int priv_h = 0;
    VkFormat priv_fmt = VK_FORMAT_UNDEFINED;
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

  // Ping-pong only (front display + back copy-out). Triple was +2 on top of a
  // PW pool that already preferred 4 — excess "latest+3" latency on Live.
  Slot slots_[2]{};
  std::atomic<int> display_slot_{0};
  std::atomic<bool> valid_{false};
  std::atomic<bool> abandoned_{false};
  std::atomic<uint64_t> present_gen_{0};
  uint64_t bound_gen_ = 0;
  bool import_logged_ = false;

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
  /** @param force_opaque_alpha XRGB/XBGR → ImageView A=ONE (Flat uses SRC_ALPHA blend). */
  static bool map_drm_to_vk(uint32_t drm_fourcc, VkFormat& out_format, bool& force_opaque_alpha);
  static bool dma_buf_key(int fd, uint64_t* out_key);
};

}  // namespace vrp
