#include "audio/audio_player.hpp"

#include "common.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#if defined(VRP_HAS_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}
#endif

#if defined(VRP_HAS_PULSE)
#include <pulse/error.h>
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>
#include <pulse/volume.h>
#endif

#if defined(VRP_HAS_MINIAUDIO)
#include "miniaudio.h"
#endif

namespace vrp {
namespace {

#if defined(VRP_HAS_PULSE)
constexpr const char* kProfilePrefix = "vrp:profile:";

struct PulseSession {
  pa_mainloop* ml = nullptr;
  pa_context* ctx = nullptr;
  bool ready = false;
  bool failed = false;

  bool connect(const char* app) {
    ml = pa_mainloop_new();
    if (!ml) return false;
    pa_mainloop_api* api = pa_mainloop_get_api(ml);
    ctx = pa_context_new(api, app);
    if (!ctx) return false;
    pa_context_set_state_callback(
        ctx,
        [](pa_context* c, void* u) {
          auto* s = static_cast<PulseSession*>(u);
          const auto st = pa_context_get_state(c);
          if (st == PA_CONTEXT_READY) s->ready = true;
          if (st == PA_CONTEXT_FAILED || st == PA_CONTEXT_TERMINATED) s->failed = true;
        },
        this);
    if (pa_context_connect(ctx, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!ready && !failed && std::chrono::steady_clock::now() < deadline) {
      if (pa_mainloop_iterate(ml, 1, nullptr) < 0) break;
    }
    return ready;
  }

  void disconnect() {
    if (ctx) {
      pa_context_disconnect(ctx);
      pa_context_unref(ctx);
      ctx = nullptr;
    }
    if (ml) {
      pa_mainloop_free(ml);
      ml = nullptr;
    }
  }

  ~PulseSession() { disconnect(); }

  bool wait_op(pa_operation* op, int timeout_ms = 3000) {
    if (!op) return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (pa_operation_get_state(op) == PA_OPERATION_RUNNING &&
           std::chrono::steady_clock::now() < deadline) {
      if (pa_mainloop_iterate(ml, 1, nullptr) < 0) break;
    }
    const bool ok = pa_operation_get_state(op) == PA_OPERATION_DONE;
    pa_operation_unref(op);
    return ok;
  }
};

bool looks_like_psvr2(const std::string& a, const std::string& b = {}) {
  auto has = [](const std::string& s) {
    std::string l = s;
    for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return l.find("ps vr2") != std::string::npos || l.find("psvr2") != std::string::npos ||
           l.find("playstation") != std::string::npos;
  };
  return has(a) || has(b);
}

bool looks_like_tv(const std::string& a, const std::string& b = {}) {
  auto has = [](const std::string& s) {
    std::string l = s;
    for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return l.find("lg ") != std::string::npos || l.find("tv") != std::string::npos ||
           l.find("sscr") != std::string::npos;
  };
  return has(a) || has(b);
}

const char* profile_for_port(const char* port_name) {
  if (!port_name) return nullptr;
  const std::string p = port_name;
  if (p == "hdmi-output-0") return "output:hdmi-stereo";
  if (p == "hdmi-output-1") return "output:hdmi-stereo-extra1";
  if (p == "hdmi-output-2") return "output:hdmi-stereo-extra2";
  if (p == "hdmi-output-3") return "output:hdmi-stereo-extra3";
  return nullptr;
}

/** Infer NVIDIA HDMI card profile needed for a sink name. */
std::string profile_for_sink_name(const std::string& sink) {
  if (sink.find("hdmi-stereo-extra1") != std::string::npos ||
      sink.find("hdmi-surround-extra1") != std::string::npos)
    return "output:hdmi-stereo-extra1";
  if (sink.find("hdmi-stereo-extra2") != std::string::npos) return "output:hdmi-stereo-extra2";
  if (sink.find("hdmi-stereo-extra3") != std::string::npos) return "output:hdmi-stereo-extra3";
  if (sink.find("hdmi-surround") != std::string::npos && sink.find("extra") == std::string::npos)
    return "output:hdmi-surround";
  if (sink.find("hdmi-stereo") != std::string::npos && sink.find("extra") == std::string::npos)
    return "output:hdmi-stereo";
  return {};
}

bool set_card_profile(PulseSession& ps, const std::string& card, const std::string& profile) {
  if (card.empty() || profile.empty()) return false;
  bool ok = false;
  ps.wait_op(pa_context_set_card_profile_by_name(
      ps.ctx, card.c_str(), profile.c_str(),
      [](pa_context*, int success, void* u) { *static_cast<bool*>(u) = success != 0; }, &ok));
  if (ok) VRP_LOG("Pulse: card %s → profile %s", card.c_str(), profile.c_str());
  else VRP_ERR("Pulse: failed profile %s → %s", card.c_str(), profile.c_str());
  return ok;
}

std::string find_nvidia_card(PulseSession& ps) {
  struct CardFind {
    std::string name;
    bool done = false;
  } cf;
  ps.wait_op(pa_context_get_card_info_list(
      ps.ctx,
      [](pa_context*, const pa_card_info* c, int eol, void* u) {
        auto* s = static_cast<CardFind*>(u);
        if (eol > 0) {
          s->done = true;
          return;
        }
        if (!c || !c->name || !s->name.empty()) return;
        std::string hay = c->name;
        if (c->proplist) {
          if (const char* v = pa_proplist_gets(c->proplist, "alsa.card_name")) hay += v;
          if (const char* v = pa_proplist_gets(c->proplist, "device.product.name")) hay += v;
          if (const char* v = pa_proplist_gets(c->proplist, "alsa.long_card_name")) hay += v;
        }
        std::string low = hay;
        for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (low.find("nvidia") != std::string::npos || low.find("hda nvidia") != std::string::npos) {
          s->name = c->name;
        }
      },
      &cf));
  return cf.name;
}

void activate_sink(PulseSession& ps, const std::string& sink) {
  if (sink.empty()) return;
  ps.wait_op(pa_context_set_default_sink(
      ps.ctx, sink.c_str(), [](pa_context*, int, void*) {}, nullptr));
  // Unmute + 100%
  pa_cvolume vol;
  pa_cvolume_set(&vol, 2, PA_VOLUME_NORM);
  ps.wait_op(pa_context_set_sink_mute_by_name(
      ps.ctx, sink.c_str(), 0, [](pa_context*, int, void*) {}, nullptr));
  ps.wait_op(pa_context_set_sink_volume_by_name(
      ps.ctx, sink.c_str(), &vol, [](pa_context*, int, void*) {}, nullptr));
  VRP_LOG("Pulse: default sink + unmute → %s", sink.c_str());
}

std::string wait_for_sink(PulseSession& ps, const std::string& profile, bool want_psvr) {
  std::string best;
  for (int attempt = 0; attempt < 25; ++attempt) {
    struct SinkPick {
      std::string best;
      std::string any_hdmi;
      bool done = false;
    } pick;
    ps.wait_op(pa_context_get_sink_info_list(
        ps.ctx,
        [](pa_context*, const pa_sink_info* i, int eol, void* u) {
          auto* s = static_cast<SinkPick*>(u);
          if (eol > 0) {
            s->done = true;
            return;
          }
          if (!i || !i->name) return;
          const std::string name = i->name;
          const std::string desc = i->description ? i->description : "";
          if (name.find("hdmi") != std::string::npos && s->any_hdmi.empty()) s->any_hdmi = name;
        },
        &pick));
    // Second pass with profile/want in mind — redo with captures via static args
    struct SinkPick2 {
      std::string profile;
      bool want_psvr = false;
      std::string best;
      std::string any_hdmi;
      bool done = false;
    } pick2;
    pick2.profile = profile;
    pick2.want_psvr = want_psvr;
    ps.wait_op(pa_context_get_sink_info_list(
        ps.ctx,
        [](pa_context*, const pa_sink_info* i, int eol, void* u) {
          auto* s = static_cast<SinkPick2*>(u);
          if (eol > 0) {
            s->done = true;
            return;
          }
          if (!i || !i->name) return;
          const std::string name = i->name;
          const std::string desc = i->description ? i->description : "";
          if (name.find("hdmi") != std::string::npos && s->any_hdmi.empty()) s->any_hdmi = name;

          const bool is_psvr = looks_like_psvr2(desc, name) || name.find("extra1") != std::string::npos;
          const bool is_tv = looks_like_tv(desc, name) ||
                             (name.find("hdmi-stereo") != std::string::npos && name.find("extra") == std::string::npos) ||
                             (name.find("hdmi-surround") != std::string::npos && name.find("extra") == std::string::npos);

          if (!s->profile.empty() && name.find(s->profile.substr(s->profile.find(':') + 1)) != std::string::npos) {
            // profile "output:hdmi-stereo-extra1" → match "hdmi-stereo-extra1"
            const auto colon = s->profile.rfind(':');
            const std::string key = colon == std::string::npos ? s->profile : s->profile.substr(colon + 1);
            if (name.find(key) != std::string::npos) s->best = name;
          }
          if (s->want_psvr && is_psvr) s->best = name;
          if (!s->want_psvr && is_tv && s->best.empty()) s->best = name;
        },
        &pick2));
    if (!pick2.best.empty()) return pick2.best;
    if (!pick2.any_hdmi.empty()) best = pick2.any_hdmi;
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
  }
  return best;
}

std::string resolve_pulse_sink(const std::string& device_name) {
  if (device_name.empty() || device_name == "default") return {};

  PulseSession ps;
  if (!ps.connect("vrp-audio-route")) return device_name.rfind(kProfilePrefix, 0) == 0 ? std::string{} : device_name;

  std::string card;
  std::string profile;
  bool want_psvr = false;

  if (device_name.rfind(kProfilePrefix, 0) == 0) {
    const std::string body = device_name.substr(std::strlen(kProfilePrefix));
    const auto bar = body.find('|');
    if (bar == std::string::npos) return {};
    card = body.substr(0, bar);
    profile = body.substr(bar + 1);
    want_psvr = profile.find("extra1") != std::string::npos;
  } else {
    profile = profile_for_sink_name(device_name);
    want_psvr = looks_like_psvr2(device_name) || device_name.find("extra1") != std::string::npos;
    if (!profile.empty()) card = find_nvidia_card(ps);
  }

  if (!card.empty() && !profile.empty()) {
    set_card_profile(ps, card, profile);
    std::string sink = wait_for_sink(ps, profile, want_psvr);
    if (sink.empty() && device_name.rfind(kProfilePrefix, 0) != 0) sink = device_name;
    if (!sink.empty()) {
      activate_sink(ps, sink);
      return sink;
    }
    return {};
  }

  // Non-HDMI sink (e.g. jamesdsp): use as-is, set default.
  activate_sink(ps, device_name);
  return device_name;
}
#endif
}  // namespace

struct AudioPlayer::State {
#if defined(VRP_HAS_FFMPEG)
  AVFormatContext* fmt = nullptr;
  AVCodecContext* codec = nullptr;
  SwrContext* swr = nullptr;
  AVFrame* frame = nullptr;
  AVPacket* packet = nullptr;
  int stream_index = -1;
  AVRational time_base{};
  int out_rate = 48000;
  int out_channels = 2;
#endif
#if defined(VRP_HAS_MINIAUDIO)
  ma_device device{};
  bool device_ok = false;
  ma_pcm_rb rb{};
  bool rb_ok = false;
  /** Owned backing store so open/flush can zero previous PCM (reset alone keeps samples). */
  std::vector<std::uint8_t> rb_mem;

  // Hot path is lock-free SPSC (ma_pcm_rb). Reset only while output_live==false and
  // writer_in_rb==false — never from the device callback.
  void clear_ring_exclusive() {
    if (!rb_ok) return;
    ma_pcm_rb_reset(&rb);
    if (!rb_mem.empty()) std::memset(rb_mem.data(), 0, rb_mem.size());
  }

  static void data_callback(ma_device* dev, void* output, const void* /*input*/, ma_uint32 frame_count) {
    auto* self = static_cast<State*>(dev->pUserData);
    auto* out = static_cast<int16_t*>(output);
    const ma_uint32 channels = dev->playback.channels;
    const size_t bytes = static_cast<size_t>(frame_count) * channels * sizeof(int16_t);
    // Silence when muted / prefilling — do not touch the ring (keeps SPSC reset safe).
    if (!self || !self->rb_ok || !self->output_live.load(std::memory_order_acquire)) {
      std::memset(out, 0, bytes);
      return;
    }
    self->callback_in_rb.fetch_add(1, std::memory_order_acq_rel);
    // Re-check after entering: quiesce may have flipped output_live.
    if (!self->output_live.load(std::memory_order_acquire)) {
      self->callback_in_rb.fetch_sub(1, std::memory_order_acq_rel);
      std::memset(out, 0, bytes);
      return;
    }
    const float gain = std::clamp(self->volume.load(std::memory_order_relaxed), 0.f, 1.f);
    int fade_left = self->fade_in_left.load(std::memory_order_relaxed);
    ma_uint32 frames_remaining = frame_count;
    while (frames_remaining > 0) {
      void* src = nullptr;
      ma_uint32 req = frames_remaining;
      if (ma_pcm_rb_acquire_read(&self->rb, &req, &src) != MA_SUCCESS || req == 0) {
        // Ring underrun — decode fell behind (or prefill raced). Emit silence for the gap.
        self->underrun_frames.fetch_add(frames_remaining, std::memory_order_relaxed);
        self->underrun_events.fetch_add(1, std::memory_order_relaxed);
        std::memset(out, 0, static_cast<size_t>(frames_remaining) * channels * sizeof(int16_t));
        break;
      }
      const auto* in_s = static_cast<const int16_t*>(src);
      const ma_uint32 n = req * channels;
      if (fade_left > 0 || gain < 0.999f) {
        for (ma_uint32 i = 0; i < n; ++i) {
          float g = gain;
          if (fade_left > 0) {
            const int frame_i = static_cast<int>(i / channels);
            const int fl = fade_left - frame_i;
            if (fl > 0) {
              const int total = std::max(1, self->fade_in_total.load(std::memory_order_relaxed));
              const int done = total - fl;
              g *= std::clamp(static_cast<float>(done) / static_cast<float>(total), 0.f, 1.f);
            }
          }
          if (g <= 0.001f) out[i] = 0;
          else if (g >= 0.999f) out[i] = in_s[i];
          else
            out[i] = static_cast<int16_t>(std::lround(static_cast<float>(in_s[i]) * g));
        }
        if (fade_left > 0) {
          fade_left = std::max(0, fade_left - static_cast<int>(req));
          self->fade_in_left.store(fade_left, std::memory_order_relaxed);
        }
      } else if (gain <= 0.001f) {
        std::memset(out, 0, static_cast<size_t>(n) * sizeof(int16_t));
      } else {
        std::memcpy(out, in_s, static_cast<size_t>(n) * sizeof(int16_t));
      }
      ma_pcm_rb_commit_read(&self->rb, req);
      out += n;
      frames_remaining -= req;
    }
    self->callback_in_rb.fetch_sub(1, std::memory_order_acq_rel);
  }
#endif
  std::mutex mu;
  std::thread thread;
  std::atomic<bool> stop{false};
  /** Decode thread may write PCM into the ring. */
  std::atomic<bool> playing{false};
  /** Device callback may read PCM (false while muted / prefilling / resetting). */
  std::atomic<bool> output_live{false};
  /** Decode thread is inside acquire_write…commit_write. */
  std::atomic<bool> writer_in_rb{false};
  /** Device callback is inside ring read (for SPSC-safe reset). */
  std::atomic<int> callback_in_rb{0};
  std::atomic<bool> seek_req{false};
  std::atomic<double> seek_sec{0.0};
  /** Drop decoded audio that ends before this timeline (backward seek preroll). -1 = off. */
  double discard_until_sec = -1.0;
  std::atomic<uint64_t> seek_epoch{0};
  std::atomic<uint64_t> seek_done_epoch{0};
  std::atomic<uint64_t> underrun_frames{0};
  std::atomic<uint64_t> underrun_events{0};
  /** Soft fade-in samples after play() to avoid stop→play pop. */
  std::atomic<int> fade_in_left{0};
  std::atomic<int> fade_in_total{0};
  std::atomic<float> volume{1.f};
  std::atomic<float> rate{1.f};
};

AudioPlayer::AudioPlayer() = default;

AudioPlayer::~AudioPlayer() { close(); }

void AudioPlayer::fail(const std::string& msg) {
  last_error_ = msg;
  VRP_ERR("audio: %s", msg.c_str());
}

std::vector<AudioDeviceInfo> AudioPlayer::list_devices() {
  std::vector<AudioDeviceInfo> out;
  out.push_back({-1, "default", "default（システム既定）"});

#if defined(VRP_HAS_PULSE)
  PulseSession ps;
  if (!ps.connect("vrp-list-sinks")) return out;

  struct ListState {
    std::vector<AudioDeviceInfo>* out = nullptr;
    int idx = 0;
  } ls;
  ls.out = &out;

  // Live sinks (jamesdsp etc.)
  ps.wait_op(pa_context_get_sink_info_list(
      ps.ctx,
      [](pa_context*, const pa_sink_info* i, int eol, void* u) {
        auto* s = static_cast<ListState*>(u);
        if (eol > 0) return;
        if (!i || !i->name) return;
        // Skip raw HDMI ALSA sinks — profile entries below are the reliable switch path.
        const std::string name = i->name;
        if (name.find("hdmi-") != std::string::npos) return;
        AudioDeviceInfo d;
        d.index = s->idx++;
        d.name = name;
        d.description = i->description ? i->description : name;
        s->out->push_back(std::move(d));
      },
      &ls));

  // Always list TV + PS VR2 as profile switches (PipeWire only exposes one HDMI profile at a time).
  ps.wait_op(pa_context_get_card_info_list(
      ps.ctx,
      [](pa_context*, const pa_card_info* c, int eol, void* u) {
        auto* s = static_cast<ListState*>(u);
        if (eol > 0) return;
        if (!c || !c->name) return;
        for (uint32_t pi = 0; pi < c->n_ports; ++pi) {
          const pa_card_port_info* port = c->ports[pi];
          if (!port || !port->name) continue;
          if (!(port->direction & PA_DIRECTION_OUTPUT)) continue;
          if (port->available == PA_PORT_AVAILABLE_NO) continue;
          const char* profile = profile_for_port(port->name);
          if (!profile) continue;
          // Must exist on this card.
          bool have = false;
          for (uint32_t pri = 0; pri < c->n_profiles; ++pri) {
            if (c->profiles[pri].name && std::string(c->profiles[pri].name) == profile) {
              have = true;
              break;
            }
          }
          if (!have) continue;

          std::string product;
          if (port->proplist) {
            if (const char* pn = pa_proplist_gets(port->proplist, "device.product.name")) product = pn;
          }
          const std::string port_desc = port->description ? port->description : port->name;

          AudioDeviceInfo d;
          d.index = s->idx++;
          d.name = std::string(kProfilePrefix) + c->name + "|" + profile;
          const std::string label_base = product.empty() ? port_desc : product;
          // Tentative labels; classify_device() refines kind/score after list.
          if (std::string(port->name) == "hdmi-output-0") {
            d.description = label_base + "（HDMI・TV）";
          } else {
            d.description = label_base + "（HDMI）";
          }
          s->out->push_back(std::move(d));
        }
      },
      &ls));
#endif
  for (auto& d : out) classify_device(d);
  for (auto& d : out) {
    if (d.kind != AudioRouteKind::HmdHdmi) continue;
    // Normalize UI label for HMD headphone routes.
    std::string base = d.description;
    const auto paren = base.find("（");
    if (paren != std::string::npos) base = base.substr(0, paren);
    while (!base.empty() && base.back() == ' ') base.pop_back();
    d.description = base + "（HDMI・HMD ヘッドホン）";
  }
  return out;
}

void AudioPlayer::classify_device(AudioDeviceInfo& d, const std::string& xr_system_hint) {
  auto lower = [](std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };
  const std::string blob = lower(d.name + " " + d.description);
  const std::string hint = lower(xr_system_hint);

  d.kind = AudioRouteKind::Generic;
  d.hmd_score = 0;

  if (d.name == "default" || blob.find("jamesdsp") != std::string::npos) {
    d.kind = AudioRouteKind::Generic;
    return;
  }

  // TV / monitor on GPU HDMI — never auto-pick for HMD.
  static const char* kTv[] = {"lg tv", "sscr", "samsung", "bravia", "tcl ", "vizio", "monitor",
                              "hdmi・tv", "hdmi・tv", "（hdmi・tv）", "living room"};
  for (const char* t : kTv) {
    if (blob.find(t) != std::string::npos) {
      d.kind = AudioRouteKind::TvHdmi;
      d.hmd_score = 0;
      return;
    }
  }
  // hdmi-output-0 / hdmi-stereo without extra → typically first display (TV)
  if (blob.find("hdmi-stereo") != std::string::npos && blob.find("extra") == std::string::npos &&
      blob.find("hmd") == std::string::npos && blob.find("vr") == std::string::npos) {
    if (d.name.find("vrp:profile:") != std::string::npos &&
        d.name.find("hdmi-stereo") != std::string::npos && d.name.find("extra") == std::string::npos) {
      d.kind = AudioRouteKind::TvHdmi;
      return;
    }
  }

  struct Hit {
    const char* key;
    int score;
  };
  // Known HMDs that expose GPU DP/HDMI audio (Windows-style path).
  static const Hit kHmd[] = {
      {"ps vr2", 100},
      {"psvr2", 100},
      {"playstation vr2", 100},
      {"playstation®vr2", 100},
      {"playstation vr", 90},
      {"ps vr", 85},
      {"index", 70},  // Valve Index
      {"vive", 70},
      {"beyond", 65},  // Bigscreen Beyond etc.
      {"pimax", 65},
      {"varjo", 65},
      {"reverb", 60},
      {"mixed reality", 55},
      {"windows mr", 55},
      {"quest", 40},  // rare on GPU HDMI; low score
      {"pico", 40},
      {"hmd", 50},
      {"vr headset", 55},
      {"headphones", 20},
      {"hdmi・hmd", 80},
      {"hmd ヘッドホン", 90},
  };

  int best = 0;
  for (const Hit& h : kHmd) {
    if (blob.find(h.key) != std::string::npos) best = std::max(best, h.score);
  }

  // OpenXR system name overlap (e.g. system "PlayStation VR2" vs port "PS VR2").
  if (!hint.empty()) {
    // Tokenize hint on spaces / punctuation
    std::string token;
    auto flush = [&] {
      if (token.size() >= 3 && blob.find(token) != std::string::npos) {
        best = std::max(best, 75);
      }
      token.clear();
    };
    for (char c : hint) {
      if (std::isalnum(static_cast<unsigned char>(c)))
        token.push_back(c);
      else
        flush();
    }
    flush();
    if (blob.find(hint) != std::string::npos) best = std::max(best, 95);
  }

  // HDMI extra1+ often is the HMD when product string is unknown but port is secondary DP.
  if (best == 0 && d.name.find("extra1") != std::string::npos) {
    best = 45;  // modest: prefer over TV when XR session is up
  }

  if (best > 0) {
    d.kind = AudioRouteKind::HmdHdmi;
    d.hmd_score = best;
  }
}

std::string AudioPlayer::prefer_hmd_device(const std::vector<AudioDeviceInfo>& devices,
                                           const std::string& xr_system_hint) {
  int best_score = 0;
  std::string best_name;
  for (auto d : devices) {
    classify_device(d, xr_system_hint);
    if (d.kind == AudioRouteKind::HmdHdmi && d.hmd_score > best_score) {
      best_score = d.hmd_score;
      best_name = d.name;
    }
  }
  return best_name;
}

bool AudioPlayer::is_wivrn_device(const AudioDeviceInfo& device) {
  if (device.name == "default") return false;
  auto lower = [](std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };
  const std::string blob = lower(device.name + " " + device.description);
  return blob.find("wivrn") != std::string::npos;
}

std::string AudioPlayer::find_wivrn_device(const std::vector<AudioDeviceInfo>& devices) {
  for (const auto& d : devices) {
    if (is_wivrn_device(d)) return d.name;
  }
  return {};
}

bool AudioPlayer::open(const std::string& path, const std::string& device_name) {
  if (!ensure_output(device_name)) return false;
  return switch_file(path);
}

bool AudioPlayer::is_output_ready() const {
  return st_ && st_->device_ok && st_->rb_ok;
}

std::string AudioPlayer::resolve_sink_label(const std::string& device_name) const {
  std::string sink_label = "default";
#if defined(VRP_HAS_PULSE)
  const std::string resolved = resolve_pulse_sink(device_name);
  if (!resolved.empty()) sink_label = resolved;
  else if (!device_name.empty() && device_name != "default") sink_label = device_name;
#else
  (void)device_name;
#endif
  return sink_label;
}

void AudioPlayer::shutdown_device() {
#if defined(VRP_HAS_MINIAUDIO)
  if (!st_) return;
  st_->output_live.store(false, std::memory_order_release);
  st_->playing.store(false, std::memory_order_release);
  if (st_->device_ok) {
    (void)ma_device_stop(&st_->device);
  }
  if (st_->rb_ok) {
    st_->clear_ring_exclusive();
    ma_pcm_rb_uninit(&st_->rb);
    st_->rb = {};
    st_->rb_ok = false;
  }
  st_->rb_mem.clear();
  st_->rb_mem.shrink_to_fit();
  if (st_->device_ok) {
    ma_device_uninit(&st_->device);
    st_->device = {};
    st_->device_ok = false;
  }
#endif
}

bool AudioPlayer::init_device(const std::string& sink_label) {
#if !defined(VRP_HAS_MINIAUDIO)
  (void)sink_label;
  fail("miniaudio が必要です");
  return false;
#else
  if (!st_) {
    fail("内部状態がありません");
    return false;
  }
  st_->out_rate = 48000;
  st_->out_channels = 2;

  constexpr ma_uint32 kRingFrames = 48000 * 2;
  const size_t rb_bytes =
      static_cast<size_t>(kRingFrames) * static_cast<size_t>(st_->out_channels) * sizeof(int16_t);
  st_->rb_mem.assign(rb_bytes, 0);
  if (ma_pcm_rb_init(ma_format_s16, static_cast<ma_uint32>(st_->out_channels), kRingFrames,
                     st_->rb_mem.data(), nullptr, &st_->rb) != MA_SUCCESS) {
    st_->rb_mem.clear();
    fail("miniaudio リングバッファを初期化できません");
    return false;
  }
  st_->rb_ok = true;
  st_->clear_ring_exclusive();

  ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
  cfg.playback.format = ma_format_s16;
  cfg.playback.channels = static_cast<ma_uint32>(st_->out_channels);
  cfg.sampleRate = static_cast<ma_uint32>(st_->out_rate);
  cfg.dataCallback = State::data_callback;
  cfg.pUserData = st_.get();
  cfg.periodSizeInFrames = 960;

  if (ma_device_init(nullptr, &cfg, &st_->device) != MA_SUCCESS) {
    fail(std::string("miniaudio 出力を開けません (") + sink_label + ")");
    shutdown_device();
    return false;
  }
  st_->device_ok = true;
  if (ma_device_start(&st_->device) != MA_SUCCESS) {
    fail("miniaudio 再生開始に失敗しました");
    shutdown_device();
    return false;
  }
  device_name_ = sink_label;
  return true;
#endif
}

bool AudioPlayer::ensure_output(const std::string& device_name) {
#if !defined(VRP_HAS_MINIAUDIO)
  (void)device_name;
  fail("miniaudio が必要です");
  return false;
#else
  const std::string sink = resolve_sink_label(device_name);
  if (is_output_ready() && device_name_ == sink) {
    mute_output();
    return true;
  }

  // Sink change or first open: (re)create device. Keep pumping silence once up.
  const bool had_file = has_audio_;
  std::string keep_path;
#if defined(VRP_HAS_FFMPEG)
  // Path is not stored separately — caller re-switch_file after sink change.
  (void)had_file;
  (void)keep_path;
#endif

  stop_thread();
  free_demux();
  has_audio_ = false;
  playing_ = false;

  if (!st_) st_ = std::make_unique<State>();
  else shutdown_device();

  st_->playing.store(false, std::memory_order_release);
  st_->output_live.store(false, std::memory_order_release);
  st_->stop.store(false);
  st_->seek_req.store(false);
  st_->volume.store(volume_);
  st_->rate.store(rate_);

  if (!init_device(sink)) {
    open_ = false;
    return false;
  }
  open_ = true;
  start_thread();  // idle until a file is attached
  VRP_LOG("Audio output ready (silence): miniaudio '%s' (%d Hz, %d ch)", device_name_.c_str(),
          st_->out_rate, st_->out_channels);
  return true;
#endif
}

void AudioPlayer::clear_file() {
  mute_output();
  stop_thread();
  free_demux();
  has_audio_ = false;
  playing_ = false;
  if (st_) {
    st_->playing.store(false, std::memory_order_release);
    st_->stop.store(false);
    flush_output();
    if (is_output_ready()) start_thread();  // keep idle silence thread
  }
}

void AudioPlayer::free_demux() {
#if defined(VRP_HAS_FFMPEG)
  if (!st_) return;
  std::lock_guard<std::mutex> lock(st_->mu);
  if (st_->swr) {
    swr_free(&st_->swr);
    st_->swr = nullptr;
  }
  if (st_->frame) av_frame_free(&st_->frame);
  if (st_->packet) av_packet_free(&st_->packet);
  if (st_->codec) avcodec_free_context(&st_->codec);
  if (st_->fmt) avformat_close_input(&st_->fmt);
  st_->stream_index = -1;
#endif
}

bool AudioPlayer::setup_demux(const std::string& path) {
#if !defined(VRP_HAS_FFMPEG)
  (void)path;
  fail("音声再生には FFmpeg が必要です");
  return false;
#else
  if (!st_) {
    fail("内部状態がありません");
    return false;
  }
  free_demux();
  if (avformat_open_input(&st_->fmt, path.c_str(), nullptr, nullptr) < 0) {
    fail("音声ファイルを開けません: " + path);
    return false;
  }
  if (avformat_find_stream_info(st_->fmt, nullptr) < 0) {
    fail("ストリーム情報を取得できません");
    free_demux();
    return false;
  }
  st_->stream_index = av_find_best_stream(st_->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  if (st_->stream_index < 0) {
    fail("音声ストリームがありません（映像のみ）");
    free_demux();
    return false;
  }

  AVStream* ast = st_->fmt->streams[st_->stream_index];
  st_->time_base = ast->time_base;
  const AVCodec* dec = avcodec_find_decoder(ast->codecpar->codec_id);
  if (!dec) {
    fail("音声デコーダが見つかりません");
    free_demux();
    return false;
  }
  st_->codec = avcodec_alloc_context3(dec);
  if (!st_->codec || avcodec_parameters_to_context(st_->codec, ast->codecpar) < 0 ||
      avcodec_open2(st_->codec, dec, nullptr) < 0) {
    fail("音声デコーダを開けません");
    free_demux();
    return false;
  }

  st_->out_rate = 48000;
  st_->out_channels = 2;

  AVChannelLayout out_ch{};
  av_channel_layout_default(&out_ch, st_->out_channels);
  if (swr_alloc_set_opts2(&st_->swr, &out_ch, AV_SAMPLE_FMT_S16, st_->out_rate, &st_->codec->ch_layout,
                          st_->codec->sample_fmt, st_->codec->sample_rate, 0, nullptr) < 0 ||
      swr_init(st_->swr) < 0) {
    av_channel_layout_uninit(&out_ch);
    fail("swresample を初期化できません");
    free_demux();
    return false;
  }
  av_channel_layout_uninit(&out_ch);

  st_->frame = av_frame_alloc();
  st_->packet = av_packet_alloc();
  if (!st_->frame || !st_->packet) {
    fail("音声フレームを確保できません");
    free_demux();
    return false;
  }
  return true;
#endif
}

bool AudioPlayer::switch_file(const std::string& path) {
#if !defined(VRP_HAS_FFMPEG) || !defined(VRP_HAS_MINIAUDIO)
  (void)path;
  fail("FFmpeg + miniaudio が必要です");
  return false;
#else
  if (!is_output_ready() && !ensure_output(device_name_)) return false;
  // Keep device running silence while we swap the demuxer — drains previous file from Pulse/HMD.
  mute_output();
  stop_thread();
  if (!setup_demux(path)) {
    has_audio_ = false;
    if (st_) {
      st_->stop.store(false);
      start_thread();  // idle silence
    }
    return false;
  }
  flush_output();
  playing_ = false;
  st_->playing.store(false, std::memory_order_release);
  st_->stop.store(false);
  st_->seek_req.store(false);
  st_->volume.store(volume_);
  st_->rate.store(rate_);
  has_audio_ = true;
  open_ = true;
  start_thread();
  VRP_LOG("Audio switched file (device kept): %s", path.c_str());
  return true;
#endif
}

void AudioPlayer::start_thread() {
#if defined(VRP_HAS_FFMPEG) && defined(VRP_HAS_MINIAUDIO)
  stop_thread();
  if (!st_) return;
  st_->stop.store(false, std::memory_order_release);
  st_->thread = std::thread([this] {
    std::vector<int16_t> buf;

    auto flush_swr = [&] {
      if (!st_->swr) return;
      for (;;) {
        buf.resize(static_cast<size_t>(2048 * st_->out_channels));
        uint8_t* outs[1] = {reinterpret_cast<uint8_t*>(buf.data())};
        const int n = swr_convert(st_->swr, outs, 2048, nullptr, 0);
        if (n <= 0) break;
      }
    };

    auto restart_at_start = [&]() -> bool {
      // Caller must hold st_->mu. Ring reset is producer-side while output is briefly muted.
      int sr = av_seek_frame(st_->fmt, st_->stream_index, 0, AVSEEK_FLAG_BACKWARD);
      if (sr < 0) {
        sr = avformat_seek_file(st_->fmt, st_->stream_index, INT64_MIN, 0, 0, 0);
      }
      if (sr < 0) {
        VRP_ERR("audio: loop seek failed (%d)", sr);
        return false;
      }
      avcodec_flush_buffers(st_->codec);
      flush_swr();
      const bool was_out = st_->output_live.exchange(false, std::memory_order_acq_rel);
      // Let the device callback observe output_live==false before resetting the SPSC ring.
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
      if (st_->rb_ok) st_->clear_ring_exclusive();
      if (was_out) st_->output_live.store(true, std::memory_order_release);
      return true;
    };

    auto last_underrun_log = std::chrono::steady_clock::now();
    uint64_t last_underrun_events = 0;

    while (!st_->stop.load()) {
      // No demux yet (session silence-only) — keep thread alive, emit nothing into the ring.
      if (!st_->fmt || st_->stream_index < 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
      if (st_->seek_req.exchange(false)) {
        std::lock_guard<std::mutex> lock(st_->mu);
        double sec = st_->seek_sec.load();
        sec = std::max(0.0, sec);
        const double tb = av_q2d(st_->time_base);
        int64_t ts = (tb > 0.0) ? static_cast<int64_t>(sec / tb) : 0;
        int sr = av_seek_frame(st_->fmt, st_->stream_index, ts, AVSEEK_FLAG_BACKWARD);
        if (sr < 0) {
          sr = avformat_seek_file(st_->fmt, st_->stream_index, INT64_MIN, ts, ts, 0);
        }
        avcodec_flush_buffers(st_->codec);
        flush_swr();
        st_->discard_until_sec = sec;
        // Host already muted output before posting seek_req; reset is exclusive here.
        if (st_->rb_ok) st_->clear_ring_exclusive();
        st_->seek_done_epoch.store(st_->seek_epoch.load(std::memory_order_relaxed),
                                   std::memory_order_release);
      }
      if (!st_->playing.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }

      {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_underrun_log > std::chrono::seconds(2)) {
          const uint64_t ev = st_->underrun_events.load(std::memory_order_relaxed);
          if (ev > last_underrun_events) {
            const uint64_t fr = st_->underrun_frames.load(std::memory_order_relaxed);
            VRP_LOG("audio underrun: +%llu events, %llu frames total (ring empty while output live)",
                    static_cast<unsigned long long>(ev - last_underrun_events),
                    static_cast<unsigned long long>(fr));
            last_underrun_events = ev;
          }
          last_underrun_log = now;
        }
      }

      // Keep the ring reasonably full so USB/decode stalls are absorbed; leave ~100ms headroom.
      // Volume is applied in data_callback (instant); rate changes seek (host) instead of flush.
      if (st_->rb_ok) {
        const ma_uint32 avail = ma_pcm_rb_available_write(&st_->rb);
        if (avail < 4800) {  // <100ms free → almost full
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
          continue;
        }
      }

      std::unique_lock<std::mutex> lock(st_->mu);
      if (av_read_frame(st_->fmt, st_->packet) < 0) {
        // End of stream — loop. If seek fails, pause rather than spin silent.
        if (!restart_at_start()) {
          lock.unlock();
          playing_ = false;
          st_->output_live.store(false, std::memory_order_release);
          st_->playing.store(false, std::memory_order_release);
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        continue;
      }
      if (st_->packet->stream_index != st_->stream_index) {
        av_packet_unref(st_->packet);
        continue;
      }
      if (avcodec_send_packet(st_->codec, st_->packet) < 0) {
        av_packet_unref(st_->packet);
        continue;
      }
      av_packet_unref(st_->packet);

      while (avcodec_receive_frame(st_->codec, st_->frame) == 0) {
        int skip_in = 0;
        if (st_->discard_until_sec >= 0.0) {
          double pts = -1.0;
          const int64_t stamp = st_->frame->best_effort_timestamp != AV_NOPTS_VALUE
                                    ? st_->frame->best_effort_timestamp
                                    : st_->frame->pts;
          if (stamp != AV_NOPTS_VALUE) pts = static_cast<double>(stamp) * av_q2d(st_->time_base);
          const double dur = st_->frame->sample_rate > 0
                                 ? st_->frame->nb_samples / static_cast<double>(st_->frame->sample_rate)
                                 : 0.0;
          if (pts >= 0.0 && pts + dur < st_->discard_until_sec - 0.005) continue;
          // Keep the overlapping frame, but drop the samples that sit before the
          // video PTS. Playing them makes audio lead by one packet after a keyframe seek.
          if (pts >= 0.0 && pts < st_->discard_until_sec && st_->frame->sample_rate > 0) {
            skip_in = static_cast<int>(std::lround((st_->discard_until_sec - pts) * st_->frame->sample_rate));
            if (skip_in >= st_->frame->nb_samples) skip_in = st_->frame->nb_samples;
          }
          st_->discard_until_sec = -1.0;
        }
        const int out_samples = swr_get_out_samples(st_->swr, st_->frame->nb_samples) + 256;
        buf.resize(static_cast<size_t>(std::max(out_samples, 0) * st_->out_channels));
        uint8_t* outs[1] = {reinterpret_cast<uint8_t*>(buf.data())};
        const int converted =
            swr_convert(st_->swr, outs, out_samples,
                        const_cast<const uint8_t**>(st_->frame->extended_data), st_->frame->nb_samples);
        if (converted <= 0 || !st_->rb_ok) continue;

        int skip_out = 0;
        if (skip_in > 0 && st_->frame->sample_rate > 0 && st_->out_rate > 0) {
          skip_out = static_cast<int>(std::lround(
              skip_in * (static_cast<double>(st_->out_rate) / st_->frame->sample_rate)));
          if (skip_out > converted) skip_out = converted;
        }
        if (skip_out >= converted) continue;

        float rate = std::clamp(st_->rate.load(), 0.5f, 20.f);
        std::vector<int16_t> timed;
        const int16_t* play_src = buf.data() + static_cast<size_t>(skip_out * st_->out_channels);
        int play_frames = converted - skip_out;
        if (std::fabs(rate - 1.f) > 0.01f) {
          const int out_n = std::max(1, static_cast<int>(std::lround(play_frames / rate)));
          timed.resize(static_cast<size_t>(out_n * st_->out_channels));
          for (int o = 0; o < out_n; ++o) {
            int src_i = static_cast<int>(std::lround(o * rate));
            if (src_i >= play_frames) src_i = play_frames - 1;
            for (int c = 0; c < st_->out_channels; ++c) {
              timed[static_cast<size_t>(o * st_->out_channels + c)] =
                  buf[static_cast<size_t>((skip_out + src_i) * st_->out_channels + c)];
            }
          }
          play_src = timed.data();
          play_frames = out_n;
        }

        ma_uint32 frames_left = static_cast<ma_uint32>(play_frames);
        const int16_t* src = play_src;
        lock.unlock();
        while (frames_left > 0 && !st_->stop.load() && st_->playing.load()) {
          if (!st_->rb_ok || !st_->playing.load()) break;
          st_->writer_in_rb.store(true, std::memory_order_release);
          void* dst = nullptr;
          ma_uint32 req = frames_left;
          if (ma_pcm_rb_acquire_write(&st_->rb, &req, &dst) != MA_SUCCESS || req == 0) {
            st_->writer_in_rb.store(false, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
          }
          std::memcpy(dst, src, static_cast<size_t>(req) * static_cast<size_t>(st_->out_channels) *
                                    sizeof(int16_t));
          ma_pcm_rb_commit_write(&st_->rb, req);
          st_->writer_in_rb.store(false, std::memory_order_release);
          src += req * st_->out_channels;
          frames_left -= req;
        }
        lock.lock();
      }
    }
  });
#endif
}

void AudioPlayer::stop_thread() {
  if (!st_) return;
  st_->stop.store(true);
  st_->playing.store(false, std::memory_order_release);
  if (st_->thread.joinable()) st_->thread.join();
}

void AudioPlayer::close() {
  // Full teardown (HMD disconnect / app exit). Prefer clear_file() while the session is live.
  mute_output();
  stop_thread();
  free_demux();
  shutdown_device();
  st_.reset();
  open_ = false;
  has_audio_ = false;
  playing_ = false;
  device_name_.clear();
}

void AudioPlayer::quiesce_and_flush() {
#if defined(VRP_HAS_MINIAUDIO)
  if (!st_) return;
  // Already muted and idle — just ensure the ring is empty (no sleeps).
  if (!st_->output_live.load(std::memory_order_acquire) &&
      !st_->playing.load(std::memory_order_acquire) &&
      !st_->writer_in_rb.load(std::memory_order_acquire) &&
      st_->callback_in_rb.load(std::memory_order_acquire) == 0) {
    if (st_->rb_ok) st_->clear_ring_exclusive();
    return;
  }
  st_->output_live.store(false, std::memory_order_release);
  st_->playing.store(false, std::memory_order_release);
  st_->fade_in_left.store(0, std::memory_order_relaxed);
  for (int i = 0; i < 100; ++i) {
    if (!st_->writer_in_rb.load(std::memory_order_acquire) &&
        st_->callback_in_rb.load(std::memory_order_acquire) == 0)
      break;
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  if (st_->rb_ok) st_->clear_ring_exclusive();
#endif
}

void AudioPlayer::seek_demux_wait(double sec) {
  if (!st_ || !has_audio_) return;
  const uint64_t ep = st_->seek_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
  st_->seek_sec.store(sec);
  st_->seek_req.store(true, std::memory_order_release);
  for (int i = 0; i < 100; ++i) {
    if (st_->seek_done_epoch.load(std::memory_order_acquire) >= ep) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  if (st_->rb_ok) st_->clear_ring_exclusive();
}

bool AudioPlayer::prefill_and_enable_output(bool wait_for_prefill) {
#if !defined(VRP_HAS_MINIAUDIO)
  (void)wait_for_prefill;
  return false;
#else
  if (!st_ || !st_->rb_ok || !has_audio_) return false;
  st_->playing.store(true, std::memory_order_release);  // decode fills ring; callback still muted
  if (wait_for_prefill) {
    constexpr ma_uint32 kPrefillFrames = 48000 / 10;  // ~100ms
    for (int i = 0; i < 50; ++i) {                   // up to ~100ms
      if (ma_pcm_rb_available_read(&st_->rb) >= kPrefillFrames) break;
      if (st_->stop.load(std::memory_order_relaxed)) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }
  const int fade = std::max(1, st_->out_rate * 15 / 1000);
  st_->fade_in_total.store(fade, std::memory_order_relaxed);
  st_->fade_in_left.store(fade, std::memory_order_relaxed);
  st_->output_live.store(true, std::memory_order_release);
  playing_ = true;
  return ma_pcm_rb_available_read(&st_->rb) > 0;
#endif
}

void AudioPlayer::play() {
  if (!open_ || !st_ || !has_audio_) {
    playing_ = false;
    return;
  }
  if (playing_ && st_->output_live.load(std::memory_order_acquire)) {
    st_->playing.store(true, std::memory_order_release);
    return;
  }
  // Already muted: do not quiesce again (avoids stacked gaps on seek recovery).
  if (st_->output_live.load(std::memory_order_acquire)) quiesce_and_flush();
  (void)prefill_and_enable_output(true);
}

void AudioPlayer::mute_output() {
  playing_ = false;
  if (!st_) return;
  quiesce_and_flush();
}

void AudioPlayer::park_at(double sec) {
  if (!st_ || !has_audio_) return;
  playing_ = false;
  quiesce_and_flush();
  seek_demux_wait(sec);
}

void AudioPlayer::arm_at(double sec) {
  if (!st_ || !has_audio_) return;
  playing_ = false;
  quiesce_and_flush();
  seek_demux_wait(sec);
  (void)prefill_and_enable_output(true);
}

void AudioPlayer::pause() {
  mute_output();
}

void AudioPlayer::stop() {
  if (!st_ || !open_) {
    playing_ = false;
    return;
  }
  mute_output();
  if (has_audio_) seek_to(0.0);
  flush_output();
}

void AudioPlayer::toggle() {
  if (!open_ || !has_audio_) return;
  if (playing_) stop();
  else play();
}

void AudioPlayer::seek_to(double sec) {
  if (!st_ || !open_ || !has_audio_) return;
  const bool was_playing = playing_;
  playing_ = false;
  quiesce_and_flush();
  seek_demux_wait(sec);
  if (was_playing) (void)prefill_and_enable_output(true);
}

void AudioPlayer::flush_output() {
#if defined(VRP_HAS_MINIAUDIO)
  if (!st_ || !st_->rb_ok) return;
  if (st_->output_live.load(std::memory_order_acquire) ||
      st_->playing.load(std::memory_order_acquire)) {
    quiesce_and_flush();
    return;
  }
  for (int i = 0; i < 50 && (st_->writer_in_rb.load(std::memory_order_acquire) ||
                             st_->callback_in_rb.load(std::memory_order_acquire) != 0);
       ++i)
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  st_->clear_ring_exclusive();
#endif
}

void AudioPlayer::set_volume(float v) {
  volume_ = std::clamp(v, 0.f, 1.f);
  if (st_) st_->volume.store(volume_, std::memory_order_relaxed);
}

void AudioPlayer::set_rate(float rate) {
  rate_ = std::clamp(rate, 0.5f, 20.f);
  if (st_) st_->rate.store(rate_, std::memory_order_relaxed);
  // Do not flush here alone — that drops buffered PCM while the demuxer stays ahead
  // and desyncs from video. Host must seek_to(video_pos) after rate changes.
}

}  // namespace vrp
