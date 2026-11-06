#include "audio/loudness_normalizer.hpp"

#include <algorithm>
#include <cmath>

namespace vrp {

namespace {

constexpr float kTarget = 0.158489f;   // -16 dBFS RMS
constexpr float kGate = 0.003981f;     // -48 dBFS: hold gain
constexpr float kMinGain = 0.251189f;  // -12 dB
constexpr float kMaxGain = 3.981072f;  // +12 dB
constexpr float kCeiling = 16384.f / 32768.f;  // -6.02 dBFS (HMD / headphone headroom)
constexpr float kBoostTau = 0.40f;
constexpr float kCutTau = 0.05f;
constexpr float kS16 = 1.f / 32768.f;

}  // namespace

void LoudnessNormalizer::set_enabled(bool on) {
  enabled_.store(on, std::memory_order_relaxed);
}

void LoudnessNormalizer::reset() { gain_ = 1.f; }

void LoudnessNormalizer::process(int16_t* interleaved, int frames, int channels, int sample_rate) {
  if (!enabled_.load(std::memory_order_relaxed)) return;
  if (!interleaved || frames <= 0 || channels <= 0) return;
  if (sample_rate < 8000) sample_rate = 48000;

  const int n = frames * channels;
  double sum_sq = 0.0;
  float peak = 0.f;
  for (int i = 0; i < n; ++i) {
    const float s = static_cast<float>(interleaved[i]) * kS16;
    sum_sq += static_cast<double>(s) * static_cast<double>(s);
    peak = std::max(peak, std::fabs(s));
  }
  const float rms = std::sqrt(static_cast<float>(sum_sq / static_cast<double>(n)));

  float desired = gain_;
  if (rms >= kGate) desired = std::clamp(kTarget / rms, kMinGain, kMaxGain);
  if (peak > 1e-6f) desired = std::min(desired, kCeiling / peak);

  const float dt = static_cast<float>(frames) / static_cast<float>(sample_rate);
  const float tau = desired < gain_ ? kCutTau : kBoostTau;
  const float alpha = 1.f - std::exp(-dt / tau);
  float g1 = gain_ + (desired - gain_) * alpha;
  float g0 = gain_;
  if (peak > 1e-6f) {
    const float cap = kCeiling / peak;
    g0 = std::min(g0, cap);
    g1 = std::min(g1, cap);
  }
  g1 = std::min(g1, kMaxGain);

  const float denom = static_cast<float>(std::max(frames - 1, 1));
  const float step = (g1 - g0) / denom;
  for (int f = 0; f < frames; ++f) {
    const float g = g0 + step * static_cast<float>(f);
    for (int c = 0; c < channels; ++c) {
      float y = static_cast<float>(interleaved[f * channels + c]) * kS16 * g;
      y = std::clamp(y, -kCeiling, kCeiling);
      const int s = static_cast<int>(std::lround(y * 32768.f));
      interleaved[f * channels + c] = static_cast<int16_t>(std::clamp(s, -16384, 16384));
    }
  }
  gain_ = g1;
}

}  // namespace vrp
