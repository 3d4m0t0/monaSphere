#pragma once

#include "common.hpp"

#include <vulkan/vulkan.h>

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

namespace vrp {

class CudaNv12Texture;
class VaapiNv12Texture;

struct VideoFrame {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgba;  // tightly packed RGBA8 (CPU path)
  double pts_sec = 0.0;
  bool gpu = false;  // true when CUDA→Vulkan path updated the shared texture
  bool full_range = false;  // AVCOL_RANGE_JPEG / PC levels
};

/** Precomputed seek-preview strip (atlas of small RGBA thumbs). */
struct ThumbnailStrip {
  int thumb_w = 160;
  int thumb_h = 90;
  int count = 0;
  double interval_sec = 0.0;
  double duration_sec = 0.0;
  std::vector<uint8_t> rgba;  // count * thumb_w * thumb_h * 4
  std::vector<double> times_sec;
  bool ready = false;
  std::string status;  // UI text

  /** Copy the nearest thumb for seek position into out (RGBA). */
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

/** Build a strip offline (own demuxer). Safe to call from a worker thread. */
ThumbnailStrip build_thumbnail_strip(const std::string& path, int max_thumbs = 48, int thumb_w = 160,
                                     int thumb_h = 90);

/** Single mid-clip preview frame. Safe from a worker thread. */
bool extract_video_preview(const std::string& path, int thumb_w, int thumb_h,
                           std::vector<uint8_t>& out_rgba);

class VideoDecoder {
 public:
  VideoDecoder();
  ~VideoDecoder();

  bool available() const;
  bool open(const std::string& path);
  void close();
  /** Pause and join decode thread; keep demuxer/hwaccel alive (for CUDA interop teardown). */
  void halt_decode();
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
  void start_thumbnail_build(std::function<void(ThumbnailStrip)> on_done = {});
  bool thumbnails_ready() const { return thumbs_ready_.load(); }
  ThumbnailStrip thumbnails_copy() const;

  /** Attach CUDA↔Vulkan target before open(). nullptr = CPU RGBA path. */
  void set_cuda_texture(CudaNv12Texture* tex) { cuda_tex_ = tex; }
  void set_vaapi_texture(VaapiNv12Texture* tex) { vaapi_tex_ = tex; }
  /** NVDEC → Vulkan NV12 is the active display path. */
  bool gpu_path_active() const;
  /** VA-API dma-buf import has produced a frame the XR thread can sample. */
  bool vaapi_path_active() const;

  /** Cancel in-flight seek-thumbnail worker (non-blocking). */
  void stop_thumbnail_build();

  void play();
  void pause();
  void toggle();
  void seek_relative(double delta_sec);
  void seek_to(double sec);
  /** True while LB/RB key-repeat scrubbing — demux keeps only a tiny queue. */
  void set_seek_scrubbing(bool active);
  /** True once after the first decoded frame following a seek (for A/V resync). */
  bool consume_av_resync();
  /** True once when priming finished filling and playback started. */
  bool consume_playback_started();
  /** True once after an end-of-stream auto-loop (seek to 0). Clears on read. */
  bool consume_looped();

  /** Playback rate 0.5 .. 20 (1 = normal). Affects decode pacing. */
  void set_rate(float rate);
  float rate() const { return rate_.load(); }

  /** Pull latest decoded frame (CPU RGBA and/or GPU flag). */
  bool update(double delta_sec, VideoFrame& out_frame);

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

  // Packet read-ahead: max ~2s; seek / scrub-end resume at ~1s.
  std::thread demux_thread_;
  std::atomic<bool> demux_stop_{false};
  std::atomic<bool> demux_pause_{false};
  std::atomic<bool> demux_eof_{false};
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
  std::thread thumbs_thread_;

  bool decode_next(VideoFrame& out, bool convert_rgba);
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
};

/** Dual RGBA textures: upload to the idle slot without blocking the XR loop. */
class VideoTexture {
 public:
  void init(VkPhysicalDevice phys, VkDevice device, VkQueue queue, uint32_t queue_family,
            std::mutex* queue_mu = nullptr, bool nearest_filter = false);
  void shutdown();

  /** Non-blocking. Returns false if a previous upload is still in flight. */
  bool try_upload(const VideoFrame& frame);
  /** True when the display slot changed — caller should set_texture(view(), sampler()). */
  bool poll_present();

  VkImageView view() const { return slots_[display_].view; }
  VkSampler sampler() const { return sampler_; }
  int width() const { return width_; }
  int height() const { return height_; }

 private:
  struct Slot {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
  };

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
  Slot slots_[2]{};
  int display_ = 0;
  int write_ = 1;
  bool in_flight_ = false;
  bool valid_ = false;
  bool rebind_ = false;

  VkSampler sampler_ = VK_NULL_HANDLE;
  VkBuffer staging_ = VK_NULL_HANDLE;
  VkDeviceMemory staging_mem_ = VK_NULL_HANDLE;
  VkDeviceSize staging_size_ = 0;
  void* staging_mapped_ = nullptr;

  uint32_t find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags props) const;
  void destroy_slots();
  bool create_slot(Slot& slot, int width, int height);
  void ensure_size(int width, int height);
  void wait_fence();
  bool fence_ready() const;
};

}  // namespace vrp
