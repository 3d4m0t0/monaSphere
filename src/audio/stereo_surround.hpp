#pragma once

#include <cstdint>
#include <vector>

namespace vrp {

/**
 * Stereo → 4.0 pseudo-surround: keep FL/FR dry, synthesize BL/BR from
 * side difference (L−R) with delay + high-pass (Haas-friendly ambient).
 */
class StereoSurround {
 public:
  void reset(int sample_rate);
  void clear();

  /** Rear (BL/BR) linear gain. Default 0.55. Clamped to 0..2. */
  void set_rear_gain(float g);
  float rear_gain() const { return rear_gain_; }

  /**
   * Interleaved S16 stereo in → interleaved S16 quad out (FL, FR, BL, BR).
   * @return number of output frames (== frame_count).
   */
  int process(const int16_t* stereo_in, int frame_count, std::vector<int16_t>& quad_out);

  static constexpr int kOutChannels = 4;
  static constexpr float kDefaultRearGain = 0.55f;

 private:
  int sample_rate_ = 48000;
  int delay_samples_ = 0;
  std::vector<float> delay_;
  int delay_pos_ = 0;
  float hpf_x1_ = 0.f;
  float hpf_y1_ = 0.f;
  float hpf_a_ = 0.f;
  float rear_gain_ = kDefaultRearGain;
};

}  // namespace vrp
