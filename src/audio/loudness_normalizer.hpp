#pragma once

#include <atomic>
#include <cstdint>

namespace vrp {

/**
 * Realtime loudness match for file and Live PCM.
 * One gain for every channel: block RMS eases toward -16 dBFS (boost 400 ms,
 * cut 50 ms, ±12 dB), then a -6 dBFS peak ceiling (±16384). Headphones and
 * HMD DACs distort at 0 dBFS. Silence below -48 dBFS holds the current gain.
 * In-place S16. Run before upmix and OpenAL HRTF so head turns do not move
 * the limiter. Reset on seek, loop, and live start.
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
