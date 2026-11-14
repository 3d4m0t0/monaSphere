#pragma once

#include "common.hpp"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct AVBufferRef;

namespace vrp {

class CudaNv12Texture;
class VulkanVideoTexture;

/** Playback NVDEC and thumbnail NVDEC must not enter CUDA at the same time. */
std::mutex& cuda_decode_mu();
/** Browser file preview and seek-thumb strip must not decode offline in parallel. */
std::mutex& ffmpeg_offline_mu();
class VaapiNv12Texture;

/** Content fingerprint for Live CPU path (sparse grid + row checksums). */
inline uint64_t rgba_content_fp(const uint8_t* rgba, int w, int h) {
  if (!rgba || w < 2 || h < 2) return 0;
  uint64_t x = 14695981039346656037ull;
  auto mix = [&](uint32_t v) {
    x ^= v;
    x *= 1099511628211ull;
  };
  auto at = [&](int px, int py) {
    const size_t o =
        (static_cast<size_t>(py) * static_cast<size_t>(w) + static_cast<size_t>(px)) * 4u;
    return static_cast<uint32_t>(rgba[o]) | (static_cast<uint32_t>(rgba[o + 1]) << 8) |
           (static_cast<uint32_t>(rgba[o + 2]) << 16) | (static_cast<uint32_t>(rgba[o + 3]) << 24);
  };
  for (int gy = 0; gy < 4; ++gy) {
    for (int gx = 0; gx < 4; ++gx) {
      const int px = std::min(w - 1, (gx * (w - 1)) / 3);
      const int py = std::min(h - 1, (gy * (h - 1)) / 3);
      mix(at(px, py));
    }
  }
  // Row checksums catch Meta double-buffer ping-pong the 4×4 grid can miss.
  const int y_step = std::max(1, h / 32);
  const int x_step = std::max(1, w / 64);
  for (int y = 0; y < h; y += y_step) {
    uint32_t row = 2166136261u;
    const uint8_t* line = rgba + static_cast<size_t>(y) * static_cast<size_t>(w) * 4u;
    for (int px = 0; px < w; px += x_step) {
      const size_t o = static_cast<size_t>(px) * 4u;
      row ^= static_cast<uint32_t>(line[o]) | (static_cast<uint32_t>(line[o + 1]) << 8) |
             (static_cast<uint32_t>(line[o + 2]) << 16);
      row *= 16777619u;
    }
    mix(row ^ static_cast<uint32_t>(y));
  }
  mix(static_cast<uint32_t>(w));
  mix(static_cast<uint32_t>(h));
  return x;
}

struct VideoFrame {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgba;  // tightly packed RGBA8 (CPU path)
  double pts_sec = 0.0;
  bool gpu = false;  // true when CUDA→Vulkan path updated the shared texture
  bool full_range = false;  // AVCOL_RANGE_JPEG / PC levels
  /** PipeWire / Live capture generation (monotonic). 0 = unset (file decode). */
  uint64_t capture_seq = 0;
  /** Sample fingerprint of rgba at publish time (Live CPU). */
  uint64_t content_fp = 0;
};

/** Precomputed seek-preview strip. Each still is a JPEG, expanded to RGBA when shown. */
struct ThumbnailStrip {
  int thumb_w = 160;
  int thumb_h = 90;
  int count = 0;
  double interval_sec = 0.0;
  double duration_sec = 0.0;
  std::vector<std::vector<uint8_t>> jpeg;
  std::vector<double> times_sec;
  bool ready = false;
  std::string status;  // UI text

  /** Decode the nearest thumb for seek position into out (RGBA). */
  bool sample_at(double sec, std::vector<uint8_t>& out_rgba, int& w, int& h) const;
};

struct VideoInfo {
  std::string path;
  std::string container;
  std::string codec;
  std::string codec_long;
  std::string pixel_format;
  std::string profile;
  std::string color_space;  // e.g. "bt709" / "bt709 tv"
  std::string hwaccel;  // e.g. "cuda (NVDEC)", "vaapi", "software"
  bool hwaccel_active = false;
  int width = 0;
  int height = 0;
  int display_width = 0;   // RGBA upload size (may be downscaled)
  int display_height = 0;
  std::string display_note;  // UI hint when downscaled
  double fps = 0.0;
  double duration_sec = 0.0;
  int64_t bitrate = 0;
  std::string audio_codec;
  int audio_sample_rate = 0;
  int audio_channels = 0;
  int thumb_count = 0;
  std::string thumb_status;
  bool ok = false;
  std::string error;

  std::string summary_line() const;
};

/** Default seek-strip length. `thumb_count` in monasphere.conf overrides this. */
inline constexpr int kSeekThumbCount = 128;
inline constexpr int kSeekThumbCountMin = 4;
inline constexpr int kSeekThumbCountMax = 512;
/** Long side that sets the seek-thumb pixel budget. `thumb_edge` in monasphere.conf overrides this. */
inline constexpr int kSeekThumbMaxEdge = 4096;
inline constexpr int kSeekThumbEdgeMin = 64;
inline constexpr int kSeekThumbEdgeMax = 4096;
/** MJPEG qscale for stored thumbs. Lower is cleaner; 3 stays sharp on the cinema quad. */
inline constexpr int kSeekThumbJpegQscale = 3;

/**
 * Called after each stored thumb, in order from the start of the file.
 * Return false to stop. `jpeg` is only valid for the call.
 */
using SeekThumbStep = std::function<bool(int index, double time_sec, int planned, double interval_sec, int thumb_w,
                                         int thumb_h, const uint8_t* jpeg, size_t jpeg_bytes)>;

/**
 * NVIDIA path. Called with one tightly packed RGBA frame instead of a JPEG.
 * Return false to stop. `rgba` is only valid for the call.
 */
using SeekThumbRawStep = std::function<bool(int index, double time_sec, int planned, double interval_sec, int thumb_w,
                                            int thumb_h, const uint8_t* rgba, size_t rgba_bytes)>;

/** Build a strip offline (own demuxer). Safe to call from a worker thread. */
ThumbnailStrip build_thumbnail_strip(const std::string& path, int max_thumbs = kSeekThumbCount, int thumb_w = 160,
                                     int thumb_h = 90, SeekThumbStep on_thumb = {}, SeekThumbRawStep on_raw = {},
                                     int max_edge = kSeekThumbMaxEdge, bool allow_hwaccel = true);

/**
 * Single mid-clip preview frame. Safe from a worker thread.
 * Scales uniformly so the long edge is at most max_edge. Does not enlarge a smaller frame.
 * Writes the stored size to out_w and out_h.
 */
bool extract_video_preview(const std::string& path, int max_edge, std::vector<uint8_t>& out_rgba, int& out_w,
                           int& out_h);

class VideoDecoder {
 public:
  VideoDecoder();
  ~VideoDecoder();

  bool available() const;
  /** `allow_hwaccel` false skips NVDEC, VA-API, and VDPAU. Software decode is used. */
  bool open(const std::string& path, bool allow_hwaccel = true);
  void close();
  /** Pause and join decode thread; keep demuxer/hwaccel alive (for CUDA interop teardown). */
  void halt_decode();
  /**
   * UI/close: trip stop flags without joining. Unwedges FFmpeg lock_queue(queue_mu)
   * so XR end_frame can finish; XR thread still calls halt_decode() later.
   */
  void abort_decode_async();
  /** NVDEC CUcontext while open, else nullptr. Valid until close(). */
  void* cuda_context() const;

  bool is_open() const { return open_.load(); }
  bool is_playing() const { return playing_.load(); }
  /** True while play()/seek/scrub-end is waiting for the prefetch threshold. */
  bool is_priming() const { return priming_.load(); }
  double duration() const { return duration_sec_; }
  double position() const { return position_sec_.load(); }
  int width() const { return width_; }
  int height() const { return height_; }
  int display_width() const { return display_w_; }
  int display_height() const { return display_h_; }
  const VideoInfo& info() const { return info_; }
  const std::string& last_error() const { return info_.error; }

  /** Non-blocking; spawns worker. on_done may run on that worker thread. */
  void start_thumbnail_build(std::function<void(ThumbnailStrip)> on_done = {},
                             int max_thumbs = kSeekThumbCount, int thumb_w = 0, int thumb_h = 0,
                             SeekThumbRawStep on_raw = {}, int max_edge = kSeekThumbMaxEdge,
                             bool allow_hwaccel = true);
  /** No prebuilt strip — scrub/seek preview uses decoder.seek_to frames. */
  void disable_thumbnail_strip();
  bool thumbnails_ready() const { return thumbs_ready_.load(); }
  /** The worker for the current build has finished, success or failure. */
  bool thumbnails_finished() const {
    const uint64_t epoch = thumbs_epoch_.load();
    return epoch != 0 && thumbs_done_epoch_.load() == epoch;
  }
  /** Seconds from 0 that already have a thumb. Equals the duration once the build finishes. */
  double thumb_cover_sec() const { return thumb_cover_sec_.load(); }
  ThumbnailStrip thumbnails_copy() const;
  /**
   * Nearest ready seek thumb. Returns false when the strip is not ready or `index`
   * is already that thumb. On success, `index` becomes the thumb that was copied.
   */
  bool copy_thumb(double sec, int& index, VideoFrame& out) const;

  /** Attach CUDA↔Vulkan target before open(). nullptr = CPU RGBA path. */
  void set_cuda_texture(CudaNv12Texture* tex) { cuda_tex_ = tex; }
  void set_vaapi_texture(VaapiNv12Texture* tex) { vaapi_tex_ = tex; }
  void set_vulkan_texture(VulkanVideoTexture* tex);
  /**
   * Borrowed AVHWDeviceContext (VULKAN) from FfmpegVulkanDevice. Not owned.
   * Used only when set_prefer_vulkan_video(true) (conf `vulkan_video=1`).
   */
  void set_vulkan_hw_device(AVBufferRef* device_ref) { vulkan_hw_device_ = device_ref; }
  /** Prefer FFmpeg Vulkan Video over CUDA/NVDEC. Default false (NVIDIA CUDA path). */
  void set_prefer_vulkan_video(bool prefer) { prefer_vulkan_video_ = prefer; }
  bool prefer_vulkan_video() const { return prefer_vulkan_video_; }
  /**
   * When true (conf `vulkan_h264_cb_cuda=1`, default) on NVIDIA, skip Vulkan Video
   * for H.264 Constrained Baseline with long edge ≥3840 (fall through to CUDA).
   * Ignored on non-NVIDIA (RADV / VA-API); that fingerprint is an NVIDIA driver class.
   */
  void set_vulkan_h264_cb_cuda(bool on) { vulkan_h264_cb_cuda_ = on; }
  bool vulkan_h264_cb_cuda() const { return vulkan_h264_cb_cuda_; }
  /** VkPhysicalDeviceProperties::vendorID (e.g. 0x10DE NVIDIA, 0x1002 AMD). */
  void set_gpu_vendor_id(uint32_t vendor_id) { gpu_vendor_id_ = vendor_id; }
  uint32_t gpu_vendor_id() const { return gpu_vendor_id_; }
  /** NVDEC → Vulkan NV12 is the active display path. */
  bool gpu_path_active() const;
  /** FFmpeg Vulkan Video → AVVkFrame sampling is active. */
  bool vulkan_path_active() const;
  /** VA-API dma-buf import has produced a frame the XR thread can sample. */
  bool vaapi_path_active() const;

  /** Cancel in-flight seek-thumbnail worker (non-blocking). */
  void stop_thumbnail_build();

  void play();
  /** Begin presenting frames. Called after audio is armed for this start. */
  void start_presentation();
  void pause();
  void toggle();
  void seek_relative(double delta_sec);
  void seek_to(double sec);
  /** True while LB/RB key-repeat scrubbing — demux keeps only a tiny queue. */
  void set_seek_scrubbing(bool active);
  /** True once after the first decoded frame following a seek (for A/V resync). */
  bool consume_av_resync();
  /** True until that post-seek frame is decoded. Playback must not start before then. */
  bool seek_in_flight() const { return seek_busy_.load(); }
  /** Host recovery when land never arrives — allow presentation to restart. */
  void clear_seek_in_flight();
  /**
   * TIMEOUT/recovery: skip priming and mark playback ready so the host can
   * arm audio + start_presentation even if demux is wedged behind ff_mu_.
   */
  void force_resume_after_seek();
  /** Decode-thread flag: host asked to abandon an in-flight post-seek wait. */
  bool consume_seek_abandon();
  /** True once when priming finished filling and playback started. */
  bool consume_playback_started();
  /** True once after an end-of-stream auto-loop (seek to 0). Clears on read. */
  bool consume_looped();
  /**
   * Seek hang diagnostics — where the decode thread is, issue/apply gens, queue.
   * Always safe to call from the XR/UI thread.
   */
  std::string seek_debug_state() const;
  const char* decode_phase() const { return decode_phase_.load(std::memory_order_acquire); }
  uint64_t seek_issue_gen() const { return seek_issue_gen_.load(std::memory_order_acquire); }
  uint64_t seek_apply_gen() const { return seek_apply_gen_.load(std::memory_order_acquire); }

  /** Playback rate 0.5 .. 20 (1 = normal). Affects decode pacing. */
  void set_rate(float rate);
  float rate() const { return rate_.load(); }

  /** Pull latest decoded frame (CPU RGBA and/or GPU flag). Playing only. */
  bool update(double delta_sec, VideoFrame& out_frame);
  /** Frames published since open. Advances when a frame is stored, including while paused. */
  uint64_t frame_seq();
  /** Like update(), but also while paused. Used to show the frame a seek landed on. */
  bool take_latest(VideoFrame& out_frame);
  /** Packet read-ahead depth (slow-storage / stall diagnostics). */
  void packet_queue_stats(size_t& packets, size_t& bytes) const;

 private:
  std::atomic<bool> open_{false};
  std::atomic<bool> playing_{false};
  std::atomic<float> rate_{1.f};
  std::atomic<bool> rate_reset_deadline_{false};
  bool has_ffmpeg_ = false;
  int width_ = 0;
  int height_ = 0;
  int display_w_ = 0;
  int display_h_ = 0;
  double duration_sec_ = 0.0;
  std::atomic<double> position_sec_{0.0};
  double frame_duration_ = 1.0 / 30.0;
  VideoInfo info_;
  CudaNv12Texture* cuda_tex_ = nullptr;
  VaapiNv12Texture* vaapi_tex_ = nullptr;
  VulkanVideoTexture* vulkan_tex_ = nullptr;
  AVBufferRef* vulkan_hw_device_ = nullptr;  // borrowed, not owned
  bool prefer_vulkan_video_ = false;
  bool vulkan_h264_cb_cuda_ = true;
  uint32_t gpu_vendor_id_ = 0;

  struct FFmpegState;
  std::unique_ptr<FFmpegState> ff_;
  std::mutex ff_mu_;

  std::thread decode_thread_;
  std::atomic<bool> decode_stop_{false};
  std::mutex frame_mu_;
  VideoFrame latest_;
  uint64_t latest_seq_ = 0;
  uint64_t consumed_seq_ = 0;
  std::atomic<bool> seek_req_{false};
  std::atomic<double> seek_target_{0.0};
  std::atomic<bool> looped_{false};
  std::atomic<bool> av_resync_{false};
  std::atomic<bool> priming_{false};
  std::atomic<bool> playback_started_{false};
  std::atomic<size_t> priming_need_bytes_{0};  // threshold for current priming
  bool awaiting_frame_after_seek_ = false;  // decode thread only
  /** Newest PTS shown during this scrub. Older pictures are not displayed. */
  double seek_shown_pts_ = -1.0;
  std::atomic<bool> seek_busy_{false};
  std::atomic<bool> seek_abandon_{false};
  /** Decode-thread progress label for seek hang diagnosis (string literal only). */
  std::atomic<const char*> decode_phase_{"idle"};
  std::atomic<uint64_t> seek_issue_gen_{0};
  std::atomic<uint64_t> seek_apply_gen_{0};
  std::atomic<int64_t> last_progress_ms_{0};
  void set_decode_phase(const char* phase);

  // Packet read-ahead: max ~2s; seek / scrub-end resume at ~1s.
  std::thread demux_thread_;
  std::atomic<bool> demux_stop_{false};
  std::atomic<bool> demux_pause_{false};
  std::atomic<bool> demux_eof_{false};
  /** Abort blocking FFmpeg IO (open / read) so XR halt cannot hang on USB. */
  std::atomic<bool> io_abort_{false};
  std::atomic<bool> seek_scrubbing_{false};
  std::atomic<uint64_t> seek_serial_{0};
  mutable std::mutex pkt_mu_;
  std::condition_variable pkt_cv_;
  std::deque<void*> pkt_q_;  // AVPacket* when FFmpeg enabled
  size_t pkt_bytes_ = 0;
  size_t prefetch_target_bytes_ = 4u << 20;   // ~2s
  size_t prefetch_resume_bytes_ = 2u << 20;   // ~1s

  mutable std::mutex thumbs_mu_;
  ThumbnailStrip thumbs_;
  std::atomic<bool> thumbs_ready_{false};
  std::atomic<bool> thumbs_cancel_{false};
  std::atomic<uint64_t> thumbs_gen_{0};
  std::atomic<uint64_t> thumbs_epoch_{0};
  std::atomic<uint64_t> thumbs_done_epoch_{0};
  std::atomic<double> thumb_cover_sec_{0};
  std::thread thumbs_thread_;

  /** present_after_sec >= 0 drops a frame that is still before that time (no pixels). */
  bool decode_next(VideoFrame& out, bool convert_rgba, double present_after_sec = -1.0);
  /** After a wedged Vulkan flush path: free+reopen codec keeping the same hwctx. */
  bool reopen_vulkan_codec_keep_hw();
  void fail_open(const std::string& path, const std::string& reason);
  void start_decode_thread();
  void stop_decode_thread();
  void decode_loop();
  void demux_loop();
  void clear_packet_queue();
  bool pop_video_packet();  // fills ff_->packet
  void choose_display_size();
  bool prefetch_enough(size_t need_bytes) const;
  void begin_priming(size_t need_bytes);
  static size_t prefetch_bytes_for_seconds(double sec, int64_t bitrate);
  void install_io_interrupt();
  static int ffmpeg_io_interrupt(void* opaque);
};

/** Dual RGBA textures: upload to the idle slot without blocking the XR loop. */
class VideoTexture {
 public:
  void init(VkPhysicalDevice phys, VkDevice device, VkQueue queue, uint32_t queue_family,
            std::mutex* queue_mu = nullptr, bool nearest_filter = false);
  void shutdown();

  /** Non-blocking. Returns false if a previous upload is still in flight. */
  bool try_upload(const VideoFrame& frame);
  /** False while a GPU upload fence is outstanding — caller should keep the frame. */
  bool upload_ready() const { return !in_flight_ && fence_ready(); }
  /** True when the display slot changed — caller should set_texture(view(), sampler()). */
  bool poll_present();
  /**
   * Semi-transparent flat panel used when no video is showing (startup / after Stop).
   * Blocks briefly if a previous upload is in flight. Returns true when presentable.
   */
  bool upload_idle_screen();
  /**
   * Dark slate + shared 8-frame / 120° 3-dot spinner (Live / DLNA motif).
   * Non-blocking via try_upload. @param phase radians; advance ~9.5 rad/s.
   */
  bool try_upload_spinner(int width, int height, float phase);

  /**
   * Live Flat busy (partial update):
   *  - panel 1920×1080: bg + text (upload only when status changes)
   *  - spinner 256²×8: blit only the center 256×256 each animation step
   */
  static constexpr int kBusyPanelW = 1920;
  static constexpr int kBusyPanelH = 1080;
  static constexpr int kBusySpinnerW = 256;
  static constexpr int kBusySpinnerH = 256;
  static constexpr int kBusyFrameCount = 8;
  /** Upload 8 spinner frames once (256²). */
  bool ensure_busy_spinners(const VideoFrame* frames, int frame_count);
  /** Full panel (bg+text). Spinner hole at center is filled by present_busy. */
  bool upload_busy_panel(const VideoFrame& panel);
  void destroy_busy_atlas();
  bool busy_ready() const { return busy_panel_ready_ && busy_spinners_ready_; }
  /**
   * Blit current spinner frame into panel center if index changed; return panel view.
   * phase wraps on 2π/3 (3-dot motif).
   */
  VkImageView present_busy(float phase_rad);
  int busy_width() const { return busy_ready() ? kBusyPanelW : 0; }
  int busy_height() const { return busy_ready() ? kBusyPanelH : 0; }

  /** @deprecated aliases for call sites still using atlas naming */
  static constexpr int kBusyAtlasW = kBusyPanelW;
  static constexpr int kBusyAtlasH = kBusyPanelH;

  VkImageView view() const { return valid_ ? slots_[display_].view : VK_NULL_HANDLE; }
  VkSampler sampler() const { return sampler_; }
  int width() const { return width_; }
  int height() const { return height_; }
  /** Drop presentable content (Live↔Live switch) without destroying the VkDevice objects. */
  void invalidate_content();
  /** Content generation of the currently displayed slot (0 = never written). */
  uint64_t display_seq() const { return slot_seq_[display_]; }
  int display_slot() const { return display_; }
  int write_slot() const { return write_; }
  uint64_t slot_content_seq(int slot) const {
    return (slot >= 0 && slot < kSlots) ? slot_seq_[slot] : 0;
  }
  uint64_t slot_content_fp(int slot) const {
    return (slot >= 0 && slot < kSlots) ? slot_fp_[slot] : 0;
  }
  int prev_display_slot() const { return prev_display_; }
  bool upload_in_flight() const { return in_flight_; }

 private:
  struct Slot {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
  };

  /** Triple-buffer: display (sampled) + prev (XR may still read) + write. */
  static constexpr int kSlots = 3;

  VkPhysicalDevice phys_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t queue_family_ = 0;
  std::mutex* queue_mu_ = nullptr;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;

  int width_ = 0;
  int height_ = 0;
  Slot slots_[kSlots]{};
  uint64_t slot_seq_[kSlots]{};
  uint64_t slot_fp_[kSlots]{};
  uint64_t upload_seq_ = 0;
  int display_ = 0;
  int prev_display_ = 0;  // may still be sampled by the previous XR frame
  int write_ = 1;
  bool in_flight_ = false;
  bool valid_ = false;
  bool rebind_ = false;

  Slot busy_panel_{};
  Slot busy_spinners_[kBusyFrameCount]{};
  bool busy_panel_ready_ = false;
  bool busy_spinners_ready_ = false;
  int busy_spinner_idx_ = -1;

  VkSampler sampler_ = VK_NULL_HANDLE;
  VkBuffer staging_ = VK_NULL_HANDLE;
  VkDeviceMemory staging_mem_ = VK_NULL_HANDLE;
  VkDeviceSize staging_size_ = 0;
  void* staging_mapped_ = nullptr;

  uint32_t find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags props) const;
  void destroy_slots();
  bool create_slot(Slot& slot, int width, int height);
  void ensure_size(int width, int height);
  bool ensure_staging(VkDeviceSize bytes);
  bool upload_slot_rgba(Slot& slot, int w, int h, const uint8_t* rgba, size_t rgba_bytes);
  void wait_fence();
  bool fence_ready() const;
};

}  // namespace vrp
