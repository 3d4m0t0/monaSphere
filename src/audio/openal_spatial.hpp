#pragma once

#include <cstdint>
#include <memory>
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

  bool init();
  void shutdown();
  bool ready() const;

  /**
   * Map FFmpeg native channel layout mask to virtual speakers.
   * @return false if OpenAL unavailable / layout empty.
   */
  bool configure(uint64_t channel_mask, int channels, int sample_rate);

  void set_listener_orientation(float yaw_rad, float pitch_rad);
  void set_gain(float linear_0_1);

  void play();
  void pause();
  void flush();

  /** Interleaved S16 frames (channel count from configure). Returns frames accepted. */
  int queue_interleaved_s16(const int16_t* frames, int frame_count);

  /** Approximate queued PCM depth (frames). */
  int min_queued_frames() const;

  const std::string& last_error() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace vrp
