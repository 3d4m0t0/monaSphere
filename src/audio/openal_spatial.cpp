#include "audio/openal_spatial.hpp"

#include "common.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#if defined(VRP_HAS_OPENAL)
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#endif

namespace vrp {
namespace {

#if defined(VRP_HAS_OPENAL)
constexpr uint64_t kChFL = 0x00000001ull;
constexpr uint64_t kChFR = 0x00000002ull;
constexpr uint64_t kChFC = 0x00000004ull;
constexpr uint64_t kChLFE = 0x00000008ull;
constexpr uint64_t kChBL = 0x00000010ull;
constexpr uint64_t kChBR = 0x00000020ull;
constexpr uint64_t kChBC = 0x00000100ull;
constexpr uint64_t kChSL = 0x00000200ull;
constexpr uint64_t kChSR = 0x00000400ull;

constexpr int kBuffersPerSource = 4;
constexpr int kBufferFrames = 2048;
constexpr float kSpeakerDist = 2.5f;

struct AzEl {
  float az_deg = 0.f;
  float el_deg = 0.f;
  bool lfe = false;
};

AzEl az_for_mask_bit(uint64_t bit) {
  switch (bit) {
    case kChFL: return {-30.f, 0.f, false};
    case kChFR: return {30.f, 0.f, false};
    case kChFC: return {0.f, 0.f, false};
    case kChLFE: return {0.f, 0.f, true};
    case kChBL: return {-135.f, 0.f, false};
    case kChBR: return {135.f, 0.f, false};
    case kChBC: return {180.f, 0.f, false};
    case kChSL: return {-90.f, 0.f, false};
    case kChSR: return {90.f, 0.f, false};
    default: return {0.f, 0.f, false};
  }
}

void az_el_to_xyz(float az_deg, float el_deg, float dist, float& x, float& y, float& z) {
  const float az = az_deg * static_cast<float>(M_PI) / 180.f;
  const float el = el_deg * static_cast<float>(M_PI) / 180.f;
  const float c = std::cos(el);
  x = std::sin(az) * c * dist;
  y = std::sin(el) * dist;
  z = -std::cos(az) * c * dist;
}

bool enable_hrtf(ALCdevice* dev) {
  if (!alcIsExtensionPresent(dev, "ALC_SOFT_HRTF")) {
    VRP_LOG("OpenAL: ALC_SOFT_HRTF missing");
    return false;
  }
  auto reset = reinterpret_cast<LPALCRESETDEVICESOFT>(alcGetProcAddress(dev, "alcResetDeviceSOFT"));
  auto getstr = reinterpret_cast<LPALCGETSTRINGISOFT>(alcGetProcAddress(dev, "alcGetStringiSOFT"));
  if (!reset) return false;
  const ALCint attrs[] = {ALC_HRTF_SOFT, ALC_TRUE, 0};
  if (!reset(dev, attrs)) return false;
  ALCint on = ALC_FALSE;
  alcGetIntegerv(dev, ALC_HRTF_SOFT, 1, &on);
  if (on) {
    const ALCchar* name = getstr ? getstr(dev, ALC_HRTF_SPECIFIER_SOFT, 0) : nullptr;
    VRP_LOG("OpenAL Soft HRTF: %s", name ? name : "on");
  }
  return on == ALC_TRUE;
}

struct Speaker {
  ALuint source = 0;
  ALuint buffers[kBuffersPerSource]{};
  ALuint free_ids[kBuffersPerSource]{};
  int free_count = 0;
  int channel_index = 0;
  bool lfe = false;
  std::vector<int16_t> mono;
};
#endif

}  // namespace

struct OpenAlSpatial::Impl {
#if defined(VRP_HAS_OPENAL)
  ALCdevice* device = nullptr;
  ALCcontext* context = nullptr;
  std::vector<Speaker> speakers;
#endif
  bool ready = false;
  int sample_rate = 48000;
  int channels = 0;
  float gain = 1.f;
  std::string last_error;
};

OpenAlSpatial::OpenAlSpatial() : impl_(std::make_unique<Impl>()) {}
OpenAlSpatial::~OpenAlSpatial() { shutdown(); }

bool OpenAlSpatial::runtime_available() {
#if defined(VRP_HAS_OPENAL)
  ALCdevice* d = alcOpenDevice(nullptr);
  if (!d) return false;
  alcCloseDevice(d);
  return true;
#else
  return false;
#endif
}

bool OpenAlSpatial::ready() const { return impl_ && impl_->ready; }
const std::string& OpenAlSpatial::last_error() const { return impl_->last_error; }

bool OpenAlSpatial::init() {
#if !defined(VRP_HAS_OPENAL)
  impl_->last_error = "OpenAL Soft がビルドに含まれていません";
  return false;
#else
  shutdown();
  impl_->device = alcOpenDevice(nullptr);
  if (!impl_->device) {
    impl_->last_error = "OpenAL デバイスを開けません";
    VRP_ERR("openal: %s", impl_->last_error.c_str());
    return false;
  }
  impl_->context = alcCreateContext(impl_->device, nullptr);
  if (!impl_->context || !alcMakeContextCurrent(impl_->context)) {
    impl_->last_error = "OpenAL コンテキストを作成できません";
    VRP_ERR("openal: %s", impl_->last_error.c_str());
    shutdown();
    return false;
  }
  (void)enable_hrtf(impl_->device);
  alDistanceModel(AL_INVERSE_DISTANCE_CLAMPED);
  alListener3f(AL_POSITION, 0.f, 0.f, 0.f);
  alListener3f(AL_VELOCITY, 0.f, 0.f, 0.f);
  const ALfloat orient[6] = {0.f, 0.f, -1.f, 0.f, 1.f, 0.f};
  alListenerfv(AL_ORIENTATION, orient);
  impl_->ready = true;
  return true;
#endif
}

void OpenAlSpatial::shutdown() {
#if defined(VRP_HAS_OPENAL)
  if (!impl_) return;
  if (impl_->context) {
    alcMakeContextCurrent(impl_->context);
    for (Speaker& sp : impl_->speakers) {
      if (sp.source) {
        alSourceStop(sp.source);
        alSourcei(sp.source, AL_BUFFER, AL_NONE);
        alDeleteSources(1, &sp.source);
      }
      alDeleteBuffers(kBuffersPerSource, sp.buffers);
    }
    impl_->speakers.clear();
    alcMakeContextCurrent(nullptr);
    alcDestroyContext(impl_->context);
    impl_->context = nullptr;
  }
  if (impl_->device) {
    alcCloseDevice(impl_->device);
    impl_->device = nullptr;
  }
#endif
  if (impl_) {
    impl_->ready = false;
    impl_->channels = 0;
  }
}

bool OpenAlSpatial::configure(uint64_t channel_mask, int channels, int sample_rate) {
#if !defined(VRP_HAS_OPENAL)
  (void)channel_mask;
  (void)channels;
  (void)sample_rate;
  return false;
#else
  if (!impl_->ready && !init()) return false;
  alcMakeContextCurrent(impl_->context);
  for (Speaker& sp : impl_->speakers) {
    if (sp.source) {
      alSourceStop(sp.source);
      alSourcei(sp.source, AL_BUFFER, AL_NONE);
      alDeleteSources(1, &sp.source);
    }
    alDeleteBuffers(kBuffersPerSource, sp.buffers);
  }
  impl_->speakers.clear();
  impl_->sample_rate = sample_rate > 0 ? sample_rate : 48000;

  auto add = [&](float az, float el, bool lfe, int ch_index) {
    Speaker sp;
    sp.channel_index = ch_index;
    sp.lfe = lfe;
    alGenSources(1, &sp.source);
    alGenBuffers(kBuffersPerSource, sp.buffers);
    for (int i = 0; i < kBuffersPerSource; ++i) sp.free_ids[i] = sp.buffers[i];
    sp.free_count = kBuffersPerSource;
    sp.mono.resize(static_cast<size_t>(kBufferFrames));
    alSourcef(sp.source, AL_ROLLOFF_FACTOR, lfe ? 0.f : 1.f);
    alSourcef(sp.source, AL_REFERENCE_DISTANCE, 1.f);
    alSourcef(sp.source, AL_MAX_DISTANCE, 20.f);
    alSourcef(sp.source, AL_GAIN, lfe ? 0.65f : 1.f);
    if (lfe) {
      alSourcei(sp.source, AL_SOURCE_RELATIVE, AL_TRUE);
      alSource3f(sp.source, AL_POSITION, 0.f, 0.f, 0.f);
    } else {
      float x, y, z;
      az_el_to_xyz(az, el, kSpeakerDist, x, y, z);
      alSourcei(sp.source, AL_SOURCE_RELATIVE, AL_FALSE);
      alSource3f(sp.source, AL_POSITION, x, y, z);
    }
    impl_->speakers.push_back(std::move(sp));
  };

  int index = 0;
  if (channel_mask != 0) {
    for (int bit = 0; bit < 64 && index < channels; ++bit) {
      const uint64_t b = 1ull << bit;
      if ((channel_mask & b) == 0) continue;
      const AzEl ae = az_for_mask_bit(b);
      add(ae.az_deg, ae.el_deg, ae.lfe, index++);
    }
  }
  // FFmpeg native order follows mask bit order; require a speaker per PCM channel.
  if (static_cast<int>(impl_->speakers.size()) != channels) {
    for (Speaker& sp : impl_->speakers) {
      if (sp.source) {
        alSourceStop(sp.source);
        alSourcei(sp.source, AL_BUFFER, AL_NONE);
        alDeleteSources(1, &sp.source);
      }
      alDeleteBuffers(kBuffersPerSource, sp.buffers);
    }
    impl_->speakers.clear();
    for (int i = 0; i < channels; ++i) {
      const float az =
          (channels <= 1) ? 0.f : (-30.f + 60.f * static_cast<float>(i) / static_cast<float>(channels - 1));
      add(az, 0.f, false, i);
    }
  }
  impl_->channels = channels;
  if (impl_->speakers.empty() || impl_->channels <= 0) {
    impl_->last_error = "対応するチャンネル配置がありません";
    return false;
  }
  VRP_LOG("OpenAL spatial: %d ch, %d Hz, mask=0x%llx", impl_->channels, impl_->sample_rate,
          static_cast<unsigned long long>(channel_mask));
  return true;
#endif
}

void OpenAlSpatial::set_listener_orientation(float yaw_rad, float pitch_rad) {
#if defined(VRP_HAS_OPENAL)
  if (!impl_->ready || !impl_->context) return;
  alcMakeContextCurrent(impl_->context);
  const float cp = std::cos(pitch_rad);
  const float sp = std::sin(pitch_rad);
  const float cy = std::cos(yaw_rad);
  const float sy = std::sin(yaw_rad);
  const ALfloat orient[6] = {sy * cp, sp, -cy * cp, -sy * sp, cp, cy * sp};
  alListenerfv(AL_ORIENTATION, orient);
#else
  (void)yaw_rad;
  (void)pitch_rad;
#endif
}

void OpenAlSpatial::set_gain(float linear_0_1) {
  impl_->gain = std::clamp(linear_0_1, 0.f, 1.f);
#if defined(VRP_HAS_OPENAL)
  if (!impl_->ready || !impl_->context) return;
  alcMakeContextCurrent(impl_->context);
  alListenerf(AL_GAIN, impl_->gain);
#endif
}

void OpenAlSpatial::play() {
#if defined(VRP_HAS_OPENAL)
  if (!impl_->ready || !impl_->context) return;
  alcMakeContextCurrent(impl_->context);
  for (Speaker& sp : impl_->speakers) {
    ALint state = AL_INITIAL;
    alGetSourcei(sp.source, AL_SOURCE_STATE, &state);
    if (state != AL_PLAYING) alSourcePlay(sp.source);
  }
#endif
}

void OpenAlSpatial::pause() {
#if defined(VRP_HAS_OPENAL)
  if (!impl_->ready || !impl_->context) return;
  alcMakeContextCurrent(impl_->context);
  for (Speaker& sp : impl_->speakers) alSourcePause(sp.source);
#endif
}

void OpenAlSpatial::flush() {
#if defined(VRP_HAS_OPENAL)
  if (!impl_->ready || !impl_->context) return;
  alcMakeContextCurrent(impl_->context);
  for (Speaker& sp : impl_->speakers) {
    alSourceStop(sp.source);
    ALint queued = 0;
    alGetSourcei(sp.source, AL_BUFFERS_QUEUED, &queued);
    while (queued-- > 0) {
      ALuint buf = 0;
      alSourceUnqueueBuffers(sp.source, 1, &buf);
    }
    for (int i = 0; i < kBuffersPerSource; ++i) sp.free_ids[i] = sp.buffers[i];
    sp.free_count = kBuffersPerSource;
  }
#endif
}

int OpenAlSpatial::min_queued_frames() const {
#if defined(VRP_HAS_OPENAL)
  if (!impl_->ready || impl_->speakers.empty()) return 0;
  int min_free = kBuffersPerSource;
  for (const Speaker& sp : impl_->speakers) min_free = std::min(min_free, sp.free_count);
  return (kBuffersPerSource - min_free) * kBufferFrames;
#else
  return 0;
#endif
}

int OpenAlSpatial::queue_interleaved_s16(const int16_t* frames, int frame_count) {
#if !defined(VRP_HAS_OPENAL)
  (void)frames;
  (void)frame_count;
  return 0;
#else
  if (!impl_->ready || !impl_->context || !frames || frame_count <= 0 || impl_->speakers.empty())
    return 0;
  alcMakeContextCurrent(impl_->context);

  auto unqueue = [&] {
    for (Speaker& sp : impl_->speakers) {
      ALint processed = 0;
      alGetSourcei(sp.source, AL_BUFFERS_PROCESSED, &processed);
      while (processed-- > 0 && sp.free_count < kBuffersPerSource) {
        ALuint buf = 0;
        alSourceUnqueueBuffers(sp.source, 1, &buf);
        sp.free_ids[sp.free_count++] = buf;
      }
    }
  };

  int offset = 0;
  while (offset < frame_count) {
    unqueue();
    bool all_have = true;
    for (const Speaker& sp : impl_->speakers) {
      if (sp.free_count <= 0) {
        all_have = false;
        break;
      }
    }
    if (!all_have) break;

    const int n = std::min(kBufferFrames, frame_count - offset);
    for (Speaker& sp : impl_->speakers) {
      sp.mono.resize(static_cast<size_t>(n));
      for (int i = 0; i < n; ++i) {
        sp.mono[static_cast<size_t>(i)] =
            frames[static_cast<size_t>((offset + i) * impl_->channels + sp.channel_index)];
      }
      const ALuint buf = sp.free_ids[--sp.free_count];
      alBufferData(buf, AL_FORMAT_MONO16, sp.mono.data(),
                   static_cast<ALsizei>(n * static_cast<int>(sizeof(int16_t))), impl_->sample_rate);
      alSourceQueueBuffers(sp.source, 1, &buf);
    }
    offset += n;
  }
  if (offset > 0) play();
  return offset;
#endif
}

}  // namespace vrp
