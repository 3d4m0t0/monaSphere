#include "audio/stereo_surround.hpp"

#include <algorithm>
#include <cmath>

namespace vrp {
namespace {

constexpr float kDelayMs = 14.f;
constexpr float kHpfHz = 180.f;

inline int16_t clamp_s16(float v) {
  if (v > 32767.f) return 32767;
  if (v < -32768.f) return -32768;
  return static_cast<int16_t>(std::lrintf(v));
}

}  // namespace

void StereoSurround::set_rear_gain(float g) {
  rear_gain_ = std::clamp(g, 0.f, 2.f);
}

void StereoSurround::reset(int sample_rate) {
  sample_rate_ = sample_rate > 0 ? sample_rate : 48000;
  delay_samples_ = std::max(1, static_cast<int>(std::lround(kDelayMs * 0.001f * sample_rate_)));
  delay_.assign(static_cast<size_t>(delay_samples_), 0.f);
  delay_pos_ = 0;
  hpf_x1_ = 0.f;
  hpf_y1_ = 0.f;
  // One-pole HPF: y = a*(y + x - x1), a = rc/(rc+dt), rc = 1/(2πf)
  const float dt = 1.f / static_cast<float>(sample_rate_);
  const float rc = 1.f / (2.f * static_cast<float>(M_PI) * kHpfHz);
  hpf_a_ = rc / (rc + dt);
}

void StereoSurround::clear() {
  std::fill(delay_.begin(), delay_.end(), 0.f);
  delay_pos_ = 0;
  hpf_x1_ = 0.f;
  hpf_y1_ = 0.f;
}

int StereoSurround::process(const int16_t* stereo_in, int frame_count, std::vector<int16_t>& quad_out) {
  if (!stereo_in || frame_count <= 0) return 0;
  if (delay_.empty()) reset(sample_rate_);

  quad_out.resize(static_cast<size_t>(frame_count) * kOutChannels);
  for (int i = 0; i < frame_count; ++i) {
    const float L = static_cast<float>(stereo_in[i * 2]);
    const float R = static_cast<float>(stereo_in[i * 2 + 1]);
    const float diff = 0.5f * (L - R);

    const float delayed = delay_[static_cast<size_t>(delay_pos_)];
    delay_[static_cast<size_t>(delay_pos_)] = diff;
    delay_pos_ = (delay_pos_ + 1) % delay_samples_;

    const float hp = hpf_a_ * (hpf_y1_ + delayed - hpf_x1_);
    hpf_x1_ = delayed;
    hpf_y1_ = hp;

    const float rear = rear_gain_ * hp;
    const size_t o = static_cast<size_t>(i) * kOutChannels;
    quad_out[o + 0] = clamp_s16(L);
    quad_out[o + 1] = clamp_s16(R);
    quad_out[o + 2] = clamp_s16(rear);   // BL
    quad_out[o + 3] = clamp_s16(-rear);  // BR
  }
  return frame_count;
}

}  // namespace vrp
