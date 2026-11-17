#pragma once

#include <atomic>
#include <cstdint>

namespace vrp {

/**
 * Realtime loudness match for file and Live PCM.
 * Block RMS eases toward -16 dBFS (boost 500 ms, cut 220 ms, max +8 dB).
 * Within each chunk, gain ramps down to a peak-safe value by the loudest frame
 * (lookahead) so quiet→bass does not click or soft-clip into grit. Output is
 * hard-capped at -6 dBFS only as a safety net. Silence below -48 dBFS holds
 * gain. In-place S16; run before upmix/HRTF. Reset on seek, loop, live start.
 */
class LoudnessNormalizer {
 public:
  void set_enabled(bool on);
  bool enabled() const { return enabled_.load(std::memory_order_relaxed); }

  /** Unity gain. Call from the same thread that calls process. */
  void reset();

  void process(int16_t* interleaved, int frames, int channels, int sample_rate);

 private:
  std::atomic<bool> enabled_{true};
  float gain_ = 1.f;
};

}  // namespace vrp
