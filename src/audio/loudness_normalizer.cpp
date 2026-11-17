#include "audio/loudness_normalizer.hpp"

#include <algorithm>
#include <cmath>

namespace vrp {

namespace {

constexpr float kTarget = 0.158489f;   // -16 dBFS RMS
constexpr float kGate = 0.003981f;     // -48 dBFS: hold gain
constexpr float kMinGain = 0.251189f;  // -12 dB
constexpr float kMaxGain = 2.511886f;  // +8 dB (was +12 — less boost into bass hits)
constexpr float kCeiling = 16384.f / 32768.f;  // -6.02 dBFS
constexpr float kBoostTau = 0.50f;
constexpr float kCutTau = 0.22f;
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
  int peak_frame = 0;
  for (int f = 0; f < frames; ++f) {
    for (int c = 0; c < channels; ++c) {
      const float s = static_cast<float>(interleaved[f * channels + c]) * kS16;
      sum_sq += static_cast<double>(s) * static_cast<double>(s);
      const float a = std::fabs(s);
      if (a > peak) {
        peak = a;
        peak_frame = f;
      }
    }
  }
  const float rms = std::sqrt(static_cast<float>(sum_sq / static_cast<double>(n)));

  float desired = gain_;
  if (rms >= kGate) desired = std::clamp(kTarget / rms, kMinGain, kMaxGain);

  const float dt = static_cast<float>(frames) / static_cast<float>(sample_rate);
  const float tau = desired < gain_ ? kCutTau : kBoostTau;
  const float alpha = 1.f - std::exp(-dt / std::max(tau, 1e-3f));
  float g_end = std::clamp(gain_ + (desired - gain_) * alpha, kMinGain, kMaxGain);

  const float g_safe = (peak > 1e-6f) ? std::max(kMinGain, kCeiling / peak) : kMaxGain;
  g_end = std::min(g_end, g_safe);

  // Lookahead in this chunk: be at g_safe by the peak frame, then ease to g_end.
  // No per-chunk g0 snap (clicked) and no tanh soft-clip (gritty bass noise).
  float g0 = gain_;
  float g_peak = g0;
  if (peak > 1e-6f) {
    g_peak = std::min(g0, g_safe);
    if (peak_frame <= 0) g0 = g_peak;  // peak on first frame — start already safe
  }

  auto gain_at = [&](int f) -> float {
    if (frames <= 1) return g_end;
    if (peak > 1e-6f && f <= peak_frame) {
      if (peak_frame <= 0) return g_peak;
      const float t = static_cast<float>(f) / static_cast<float>(peak_frame);
      return g0 + (g_peak - g0) * t;
    }
    if (peak > 1e-6f && f > peak_frame) {
      const int span = frames - 1 - peak_frame;
      if (span <= 0) return g_end;
      const float t = static_cast<float>(f - peak_frame) / static_cast<float>(span);
      return g_peak + (g_end - g_peak) * t;
    }
    const float t = static_cast<float>(f) / static_cast<float>(frames - 1);
    return g0 + (g_end - g0) * t;
  };

  for (int f = 0; f < frames; ++f) {
    const float g = gain_at(f);
    for (int c = 0; c < channels; ++c) {
      float y = static_cast<float>(interleaved[f * channels + c]) * kS16 * g;
      y = std::clamp(y, -kCeiling, kCeiling);
      const int s = static_cast<int>(std::lround(y * 32768.f));
      interleaved[f * channels + c] = static_cast<int16_t>(std::clamp(s, -16384, 16384));
    }
  }
  gain_ = g_end;
}

}  // namespace vrp
