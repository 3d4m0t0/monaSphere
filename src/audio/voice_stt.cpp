#include "audio/voice_stt.hpp"

#include "audio/vosk_bundle.hpp"
#include "common.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <vector>

#if defined(VRP_HAS_PULSE)
#include <pulse/error.h>
#include <pulse/simple.h>
#endif

namespace vrp {
namespace {

namespace fs = std::filesystem;

std::string json_text_field(const char* json) {
  if (!json || !*json) return {};
  const char* key = std::strstr(json, "\"text\"");
  if (!key) key = std::strstr(json, "\"partial\"");
  if (!key) return {};
  const char* colon = std::strchr(key, ':');
  if (!colon) return {};
  const char* q1 = std::strchr(colon, '"');
  if (!q1) return {};
  ++q1;
  const char* q2 = q1;
  while (*q2 && *q2 != '"') {
    if (*q2 == '\\' && q2[1]) q2 += 2;
    else ++q2;
  }
  if (!*q2) return {};
  return std::string(q1, static_cast<size_t>(q2 - q1));
}

}  // namespace

VoiceStt::~VoiceStt() { stop(); }

std::string VoiceStt::last_error() const {
  std::lock_guard<std::mutex> lock(mu_);
  return last_error_;
}

std::string VoiceStt::text() const {
  std::lock_guard<std::mutex> lock(mu_);
  return text_;
}

void VoiceStt::clear_text() {
  std::lock_guard<std::mutex> lock(mu_);
  text_.clear();
}

void VoiceStt::reset_utterance() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    text_.clear();
  }
  if (running_.load(std::memory_order_acquire)) {
    reset_req_.store(true, std::memory_order_release);
  }
}

bool VoiceStt::load_vosk() {
  if (dl_) return true;
  VoskLibStatus st;
  const std::string primary = resolve_vosk_lib_for_dlopen(&st);
  if (st.kind == VoskLibStatus::Kind::OfficialMatch) {
    VRP_LOG("voice-stt: official libvosk.so SHA-256 OK (%s)", st.actual_sha256.c_str());
  } else if (st.kind == VoskLibStatus::Kind::OfficialMismatch) {
    VRP_LOG("voice-stt: bundled libvosk.so SHA-256 mismatch (expected %s got %s)",
            st.expected_sha256.c_str(), st.actual_sha256.c_str());
  }
  const char* candidates[] = {
      primary.empty() ? nullptr : primary.c_str(),
      "libvosk.so",
      "libvosk.so.0",
      "/usr/lib64/libvosk.so",
      "/usr/lib/libvosk.so",
  };
  for (const char* path : candidates) {
    if (!path) continue;
    dl_ = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (dl_) {
      VRP_LOG("voice-stt: dlopen %s", path);
      break;
    }
  }
  if (!dl_) {
    last_error_ = "libvosk.so not found (run tools/fetch_vosk.sh or set MONASPHERE_VOSK_LIB)";
    return false;
  }
  model_new_ = reinterpret_cast<ModelNewFn>(dlsym(dl_, "vosk_model_new"));
  model_free_ = reinterpret_cast<ModelFreeFn>(dlsym(dl_, "vosk_model_free"));
  rec_new_ = reinterpret_cast<RecNewFn>(dlsym(dl_, "vosk_recognizer_new"));
  rec_free_ = reinterpret_cast<RecFreeFn>(dlsym(dl_, "vosk_recognizer_free"));
  rec_accept_ = reinterpret_cast<RecAcceptFn>(dlsym(dl_, "vosk_recognizer_accept_waveform"));
  rec_partial_ = reinterpret_cast<RecPartialFn>(dlsym(dl_, "vosk_recognizer_partial_result"));
  rec_result_ = reinterpret_cast<RecResultFn>(dlsym(dl_, "vosk_recognizer_result"));
  rec_final_ = reinterpret_cast<RecFinalFn>(dlsym(dl_, "vosk_recognizer_final_result"));
  if (!model_new_ || !model_free_ || !rec_new_ || !rec_free_ || !rec_accept_ || !rec_partial_ ||
      !rec_result_ || !rec_final_) {
    last_error_ = "libvosk.so missing symbols";
    unload_vosk();
    return false;
  }
  return true;
}

void VoiceStt::unload_vosk() {
  if (model_ && model_free_) {
    model_free_(model_);
    model_ = nullptr;
  }
  if (dl_) {
    dlclose(dl_);
    dl_ = nullptr;
  }
  model_new_ = nullptr;
  model_free_ = nullptr;
  rec_new_ = nullptr;
  rec_free_ = nullptr;
  rec_accept_ = nullptr;
  rec_partial_ = nullptr;
  rec_result_ = nullptr;
  rec_final_ = nullptr;
}

namespace {

/** Map conf / UI codes to canonical live_voice_lang values. */
std::string normalize_voice_lang(std::string lang) {
  for (char& c : lang) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c == '-') c = '_';
  }
  if (lang == "zh_cn" || lang == "cn" || lang == "zh") return "zh_CN";
  if (lang == "zh_tw") return "zh_TW";
  if (lang == "ja" || lang == "en" || lang == "de" || lang == "fr" || lang == "es" || lang == "ko") {
    return lang;
  }
  return "ja";
}

const char* vosk_model_dirname(const std::string& lang) {
  if (lang == "en") return "vosk-model-small-en-us-0.15";
  if (lang == "de") return "vosk-model-small-de-0.15";
  if (lang == "fr") return "vosk-model-small-fr-0.22";
  if (lang == "es") return "vosk-model-small-es-0.42";
  if (lang == "ko") return "vosk-model-small-ko-0.22";
  if (lang == "zh_CN" || lang == "zh_TW") return "vosk-model-small-cn-0.22";
  return "vosk-model-small-ja-0.22";
}

}  // namespace

std::string VoiceStt::resolve_model_dir(const std::string& lang) const {
  const char* env = std::getenv("MONASPHERE_VOSK_MODEL");
  if (env && *env && fs::is_directory(env)) return env;
  const char* home = std::getenv("HOME");
  const std::string base =
      home ? (std::string(home) + "/.local/share/monasphere/vosk") : std::string("vosk");
  const char* name = vosk_model_dirname(lang);
  const fs::path p = fs::path(base) / name;
  if (fs::is_directory(p)) return p.string();
  const fs::path alt = fs::path(base) / lang;
  if (fs::is_directory(alt)) return alt.string();
  if (lang == "zh_CN" || lang == "zh_TW") {
    const fs::path cn = fs::path(base) / "zh";
    if (fs::is_directory(cn)) return cn.string();
  }
  return {};
}

bool VoiceStt::start(const std::string& lang) {
  stop();
  {
    std::lock_guard<std::mutex> lock(mu_);
    last_error_.clear();
    text_.clear();
  }
  level_.store(0.f, std::memory_order_release);
#if !defined(VRP_HAS_PULSE)
  {
    std::lock_guard<std::mutex> lock(mu_);
    last_error_ = "PulseAudio not built";
  }
  return false;
#else
  if (!load_vosk()) return false;
  const std::string use_lang = normalize_voice_lang(lang);
  const std::string model_dir = resolve_model_dir(use_lang);
  if (model_dir.empty()) {
    std::lock_guard<std::mutex> lock(mu_);
    last_error_ = std::string("Vosk model missing (~/.local/share/monasphere/vosk/") +
                  vosk_model_dirname(use_lang) + ") — see third_party/vosk/README.md";
    return false;
  }
  model_ = model_new_(model_dir.c_str());
  if (!model_) {
    std::lock_guard<std::mutex> lock(mu_);
    last_error_ = "vosk_model_new failed: " + model_dir;
    return false;
  }
  stop_req_.store(false, std::memory_order_release);
  reset_req_.store(false, std::memory_order_release);
  running_.store(true, std::memory_order_release);
  thr_ = std::thread([this] { worker_main({}); });
  return true;
#endif
}

void VoiceStt::stop() {
  stop_req_.store(true, std::memory_order_release);
  if (thr_.joinable()) thr_.join();
  running_.store(false, std::memory_order_release);
  level_.store(0.f, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(mu_);
    text_.clear();
    last_error_.clear();
  }
  if (model_ && model_free_) {
    model_free_(model_);
    model_ = nullptr;
  }
}

void VoiceStt::worker_main(std::string /*lang*/) {
#if !defined(VRP_HAS_PULSE)
  running_.store(false, std::memory_order_release);
#else
  constexpr int kRate = 16000;
  // Short capture for mic HUD level (~16ms). Same PCM goes to Vosk.
  constexpr int kChunkMs = 16;
  constexpr size_t kChunk = static_cast<size_t>(kRate * kChunkMs / 1000);
  pa_sample_spec ss{};
  ss.format = PA_SAMPLE_S16LE;
  ss.rate = static_cast<uint32_t>(kRate);
  ss.channels = 1;
  pa_buffer_attr attr{};
  attr.fragsize = static_cast<uint32_t>(kChunk * sizeof(int16_t));
  attr.maxlength = static_cast<uint32_t>(-1);
  attr.prebuf = static_cast<uint32_t>(-1);
  attr.tlength = static_cast<uint32_t>(-1);
  attr.minreq = static_cast<uint32_t>(-1);
  int err = 0;
  pa_simple* pa = pa_simple_new(nullptr, "monaSphere", PA_STREAM_RECORD, nullptr, "voice-stt", &ss,
                                nullptr, &attr, &err);
  if (!pa) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      last_error_ = std::string("mic open failed: ") + pa_strerror(err);
    }
    running_.store(false, std::memory_order_release);
    return;
  }

  void* rec = rec_new_(model_, static_cast<float>(kRate));
  if (!rec) {
    pa_simple_free(pa);
    {
      std::lock_guard<std::mutex> lock(mu_);
      last_error_ = "vosk_recognizer_new failed";
    }
    running_.store(false, std::memory_order_release);
    return;
  }

  std::string committed;
  std::vector<int16_t> buf(kChunk);
  // RMS gate: ignore room/mic noise so Vosk does not invent 「ん」 fillers.
  float rms_gate = 0.012f;
  if (const char* env = std::getenv("MONASPHERE_VOSK_RMS_GATE")) {
    char* end = nullptr;
    const float v = std::strtof(env, &end);
    if (end != env && v > 0.f && v < 1.f) rms_gate = v;
  }
  constexpr int kHangoverChunks = 10;  // ~160ms after last loud chunk
  int hangover = 0;
  while (!stop_req_.load(std::memory_order_acquire)) {
    if (reset_req_.exchange(false, std::memory_order_acq_rel)) {
      if (rec) {
        rec_free_(rec);
        rec = nullptr;
      }
      committed.clear();
      {
        std::lock_guard<std::mutex> lock(mu_);
        text_.clear();
      }
      rec = rec_new_(model_, static_cast<float>(kRate));
      if (!rec) {
        std::lock_guard<std::mutex> lock(mu_);
        last_error_ = "vosk_recognizer_new failed (reset)";
        break;
      }
    }
    if (pa_simple_read(pa, buf.data(), buf.size() * sizeof(int16_t), &err) < 0) {
      std::lock_guard<std::mutex> lock(mu_);
      last_error_ = std::string("mic read: ") + pa_strerror(err);
      break;
    }
    double acc = 0.0;
    float peak = 0.f;
    for (int16_t s : buf) {
      const float a = std::fabs(static_cast<float>(s) / 32768.f);
      if (a > peak) peak = a;
      acc += static_cast<double>(a) * static_cast<double>(a);
    }
    const float rms = static_cast<float>(std::sqrt(acc / static_cast<double>(buf.size())));
    level_.store(std::clamp(peak * 3.2f, 0.f, 1.f), std::memory_order_release);

    // Below RMS: replace with silence (keeps Vosk endpointing; skips noise→filler).
    const bool loud = rms >= rms_gate;
    if (loud) {
      hangover = kHangoverChunks;
    } else if (hangover > 0) {
      --hangover;
    }
    if (!(loud || hangover > 0)) {
      std::fill(buf.begin(), buf.end(), static_cast<int16_t>(0));
    }

    if (!rec) continue;
    // AcceptWaveform: 0 = still speaking → PartialResult; 1 = endpoint → Result, then continue.
    const int got = rec_accept_(rec, reinterpret_cast<const char*>(buf.data()),
                                static_cast<int>(buf.size() * sizeof(int16_t)));
    if (got) {
      const std::string piece = json_text_field(rec_result_(rec));
      if (!piece.empty()) {
        if (!committed.empty()) committed.push_back(' ');
        committed += piece;
      }
      std::lock_guard<std::mutex> lock(mu_);
      text_ = committed;
    } else {
      const std::string partial = json_text_field(rec_partial_(rec));
      std::lock_guard<std::mutex> lock(mu_);
      if (partial.empty()) {
        text_ = committed;
      } else if (committed.empty()) {
        text_ = partial;
      } else {
        text_ = committed + ' ' + partial;
      }
    }
  }

  const std::string fin = json_text_field(rec_final_(rec));
  if (!fin.empty()) {
    if (!committed.empty()) committed.push_back(' ');
    committed += fin;
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!committed.empty()) text_ = committed;
  }

  rec_free_(rec);
  pa_simple_free(pa);
  level_.store(0.f, std::memory_order_release);
  running_.store(false, std::memory_order_release);
#endif
}

}  // namespace vrp
