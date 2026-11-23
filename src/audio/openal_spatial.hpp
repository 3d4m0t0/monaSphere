#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>  // recursive_mutex
#include <string>

namespace vrp {

/**
 * Cinema layout → OpenAL Soft HRTF binaural.
 * Speakers are world-locked; listener follows HMD / stick look.
 * Stereo is typically queued as 4.0 (FL/FR + side-diff BL/BR upmix).
 */
class OpenAlSpatial {
 public:
  OpenAlSpatial();
  ~OpenAlSpatial();

  OpenAlSpatial(const OpenAlSpatial&) = delete;
  OpenAlSpatial& operator=(const OpenAlSpatial&) = delete;

  static bool runtime_available();

  /** @param pulse_sink  Pulse/PipeWire sink name (e.g. jamesdsp_sink); empty = default. */
  bool init(const std::string& pulse_sink = {});
  void shutdown();
  bool ready() const;

  /**
   * Map FFmpeg native channel layout mask to virtual speakers.
   * @return false if OpenAL unavailable / layout empty.
   */
  bool configure(uint64_t channel_mask, int channels, int sample_rate,
                 const std::string& pulse_sink = {});

  /**
   * Publish HMD listener orientation. Never blocks the XR thread: stores atomics and
   * try_locks Soft. Live PCM feed holds Soft's mutex during alBufferData — a blocking
   * set_listener here used to stall xrWaitFrame (visual + HRTF logs freeze together).
   */
  void set_listener_orientation(float yaw_rad, float pitch_rad);
  void set_gain(float linear_0_1);
  /** While held, play() does not start sources (queued PCM stays silent). */
  void set_hold_silence(bool hold);

  void play();
  void pause();
  void flush();

  /** Interleaved S16 frames (channel count from configure). Returns frames accepted. */
  int queue_interleaved_s16(const int16_t* frames, int frame_count);

  /**
   * Queued PCM depth in real frames (sum of per-buffer sizes, not buffer_count×2048).
   * Reclaims played Soft buffers so depth stays accurate while nothing is queueing.
   */
  int min_queued_frames();

  const std::string& last_error() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  /** recursive: queue_interleaved_s16 calls play() while holding the lock. */
  mutable std::recursive_mutex mu_;
  std::atomic<float> pending_yaw_{0.f};
  std::atomic<float> pending_pitch_{0.f};
  std::atomic<bool> pending_orient_{false};
  /** Caller must hold mu_. */
  void shutdown_unlocked();
  /** Caller must hold mu_ and have a current Soft context. */
  void apply_pending_orient_unlocked();
};

}  // namespace vrp
