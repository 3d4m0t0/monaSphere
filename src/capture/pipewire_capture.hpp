#pragma once

#include "video/video_decoder.hpp"

#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace vrp {

/** One ScreenCast plane as a dma-buf FD (caller owns `fd` after take_latest_dma). */
struct PipewireDmaFrame {
  int fd = -1;
  int width = 0;
  int height = 0;
  uint32_t drm_fourcc = 0;
  uint64_t modifier = 0;
  /** True when SPA reported SPA_VIDEO_FLAG_MODIFIER with a non-INVALID value. */
  bool has_modifier = false;
  uint32_t stride = 0;
  uint32_t offset = 0;
  uint32_t size = 0;
  uint64_t seq = 0;

  PipewireDmaFrame() = default;
  ~PipewireDmaFrame() { release(); }

  PipewireDmaFrame(const PipewireDmaFrame&) = delete;
  PipewireDmaFrame& operator=(const PipewireDmaFrame&) = delete;

  PipewireDmaFrame(PipewireDmaFrame&& o) noexcept { *this = std::move(o); }
  PipewireDmaFrame& operator=(PipewireDmaFrame&& o) noexcept {
    if (this == &o) return *this;
    release();
    fd = o.fd;
    width = o.width;
    height = o.height;
    drm_fourcc = o.drm_fourcc;
    modifier = o.modifier;
    has_modifier = o.has_modifier;
    stride = o.stride;
    offset = o.offset;
    size = o.size;
    seq = o.seq;
    o.fd = -1;
    o.has_modifier = false;
    return *this;
  }

  void release() {
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
  }

  explicit operator bool() const { return fd >= 0 && width > 0 && height > 0; }
};

/**
 * Consume a portal PipeWire remote FD + node id.
 * Prefers SPA_DATA_DmaBuf (zero-copy); falls back to MemFd/MemPtr → CPU RGBA.
 */
class PipewireCapture {
 public:
  PipewireCapture() = default;
  ~PipewireCapture();

  PipewireCapture(const PipewireCapture&) = delete;
  PipewireCapture& operator=(const PipewireCapture&) = delete;

  /**
   * Takes ownership of pw_fd.
   * @param prefer_w/h Preferred SPA_FORMAT_VIDEO_size (KWin resizes virtual
   *        MONITOR streams to this). Pass live_virtual_size — a hardcoded
   *        1920×1080 preferred size used to shrink 4K Virtual-* down to 1080p.
   * @param consumer_modifiers DRM modifiers this process can sample (from Vulkan).
   *        Empty → LINEAR only. Do not include MOD_INVALID (forces SHM demote on NVIDIA).
   */
  bool start(int pw_fd, uint32_t node_id, uint64_t pipewire_serial = 0,
             std::vector<uint64_t> consumer_modifiers = {}, int prefer_w = 0, int prefer_h = 0);
  void stop();

  bool running() const { return running_.load(std::memory_order_acquire); }
  std::string last_error() const;

  /** Copy latest CPU RGBA frame if newer than `seen_seq`. */
  bool take_latest(VideoFrame& out, uint64_t& seen_seq);

  /** Move latest dma-buf frame if newer than `seen_seq` (transfers FD ownership). */
  bool take_latest_dma(PipewireDmaFrame& out, uint64_t& seen_seq);

  int width() const { return width_.load(std::memory_order_relaxed); }
  int height() const { return height_.load(std::memory_order_relaxed); }

  /** True after at least one DMA frame was published (may later fall back). */
  bool dma_seen() const { return dma_seen_.load(std::memory_order_relaxed); }

  /** Called from PipeWire process callback. */
  void publish_frame(VideoFrame frame);
  void publish_dma(PipewireDmaFrame frame);
  void set_error_public(const std::string& e);

  /** Negotiated SPA video format / DRM fourcc / modifier (for logging). */
  void set_negotiated(uint32_t spa_format, uint32_t drm_fourcc, uint64_t modifier, bool using_dmabuf);
  uint32_t negotiated_drm_fourcc() const { return drm_fourcc_.load(std::memory_order_relaxed); }
  uint64_t negotiated_modifier() const { return modifier_.load(std::memory_order_relaxed); }
  bool using_dmabuf() const { return using_dmabuf_.load(std::memory_order_relaxed); }

  /** Path / format notes for the host UI log (drained once). */
  void push_status(std::string line);
  std::vector<std::string> drain_status();

 private:
  void thread_main(int pw_fd, uint32_t node_id, uint64_t serial);
  void set_error(const std::string& e);

  std::thread thread_;
  std::vector<uint64_t> consumer_modifiers_;
  int prefer_w_ = 0;
  int prefer_h_ = 0;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_{false};
  std::atomic<int> width_{0};
  std::atomic<int> height_{0};
  std::atomic<uint64_t> seq_{0};
  std::atomic<uint64_t> dma_seq_{0};
  std::atomic<bool> dma_seen_{false};
  std::atomic<uint32_t> drm_fourcc_{0};
  std::atomic<uint64_t> modifier_{0};
  std::atomic<bool> using_dmabuf_{false};

  mutable std::mutex frame_mu_;
  VideoFrame latest_;
  PipewireDmaFrame latest_dma_;
  mutable std::mutex err_mu_;
  std::string last_error_;
  mutable std::mutex status_mu_;
  std::deque<std::string> status_lines_;
};

}  // namespace vrp
