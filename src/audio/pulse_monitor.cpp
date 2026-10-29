#include "audio/pulse_monitor.hpp"

#include "common.hpp"

#if defined(VRP_HAS_PULSE)
#include <pulse/error.h>
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

namespace vrp {
namespace {

std::atomic<uint32_t> g_yt_null_module{0};

#if defined(VRP_HAS_PULSE)
/**
 * Timed Pulse mainloop — never use pa_mainloop_iterate(block=1).
 * block=1 waits forever when idle; Live Soft start hung in drain() after route
 * Pass 1 (log stopped at "routing browsers", never reached Soft configure).
 */
struct PulseSession {
  pa_mainloop* ml = nullptr;
  pa_context* ctx = nullptr;
  bool ready = false;
  bool failed = false;

  /** One prepare/poll/dispatch with a hard timeout (ms). Never blocks forever. */
  bool pump(int timeout_ms) {
    if (!ml) return false;
    if (timeout_ms < 0) timeout_ms = 0;
    if (pa_mainloop_prepare(ml, timeout_ms * 1000) < 0) return false;
    if (pa_mainloop_poll(ml) < 0) return false;
    if (pa_mainloop_dispatch(ml) < 0) return false;
    return true;
  }

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
      const auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(
                           deadline - std::chrono::steady_clock::now())
                           .count();
      if (!pump(static_cast<int>(std::min<std::chrono::milliseconds::rep>(rem, 50)))) break;
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
    ready = false;
    failed = false;
  }

  ~PulseSession() { disconnect(); }

  bool wait_op(pa_operation* op, int timeout_ms = 3000) {
    if (!op) return false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (pa_operation_get_state(op) == PA_OPERATION_RUNNING &&
           std::chrono::steady_clock::now() < deadline) {
      const auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(
                           deadline - std::chrono::steady_clock::now())
                           .count();
      if (!pump(static_cast<int>(std::min<std::chrono::milliseconds::rep>(rem, 50)))) break;
    }
    const bool ok = pa_operation_get_state(op) == PA_OPERATION_DONE;
    pa_operation_unref(op);
    return ok;
  }

  /** Non-blocking dispatch of already-queued events (moves, etc.). */
  void drain(int n = 40) {
    for (int i = 0; i < n; ++i) {
      if (!pump(0)) break;
    }
  }
};

bool sink_exists(PulseSession& ps, const char* name) {
  bool found = false;
  struct St {
    const char* name = nullptr;
    bool* found = nullptr;
  } st{name, &found};
  ps.wait_op(pa_context_get_sink_info_by_name(
      ps.ctx, name,
      [](pa_context*, const pa_sink_info* i, int eol, void* u) {
        auto* s = static_cast<St*>(u);
        if (eol || !i) return;
        *s->found = true;
      },
      &st));
  return found;
}

uint32_t sink_index_by_name(PulseSession& ps, const char* name) {
  uint32_t idx = PA_INVALID_INDEX;
  struct St {
    const char* name = nullptr;
    uint32_t* idx = nullptr;
  } st{name, &idx};
  ps.wait_op(pa_context_get_sink_info_by_name(
      ps.ctx, name,
      [](pa_context*, const pa_sink_info* i, int eol, void* u) {
        auto* s = static_cast<St*>(u);
        if (eol || !i) return;
        *s->idx = i->index;
      },
      &st));
  return idx;
}

bool contains_ci(const char* hay, const char* needle) {
  if (!hay || !needle || !*needle) return false;
  const size_t nlen = std::strlen(needle);
  for (const char* p = hay; *p; ++p) {
    size_t i = 0;
    for (; i < nlen; ++i) {
      const char a = p[i];
      const char b = needle[i];
      if (!a) return false;
      const char al = (a >= 'A' && a <= 'Z') ? static_cast<char>(a - 'A' + 'a') : a;
      const char bl = (b >= 'A' && b <= 'Z') ? static_cast<char>(b - 'A' + 'a') : b;
      if (al != bl) break;
    }
    if (i == nlen) return true;
  }
  return false;
}

const char* input_prop(const pa_sink_input_info* i, const char* key) {
  return (i && i->proplist) ? pa_proplist_gets(i->proplist, key) : nullptr;
}

bool is_browser_input(const pa_sink_input_info* i) {
  if (!i) return false;
  // PipeWire-Pulse: node.name is often "Google Chrome" while binary is just "chrome".
  const char* keys[] = {i->name,
                        input_prop(i, "application.name"),
                        input_prop(i, "application.process.binary"),
                        input_prop(i, "application.icon_name"),
                        input_prop(i, "media.name"),
                        input_prop(i, "node.name")};
  for (const char* k : keys) {
    if (contains_ci(k, "chrome") || contains_ci(k, "chromium") || contains_ci(k, "monasphereyt") ||
        contains_ci(k, "brave") || contains_ci(k, "msedge") || contains_ci(k, "google-chrome") ||
        contains_ci(k, "firefox") || contains_ci(k, "librewolf"))
      return true;
  }
  return false;
}

/** Our OpenAL / silent device — must stay unmuted on the HMD sink. */
bool is_mona_openal_input(const pa_sink_input_info* i) {
  if (!i) return false;
  const char* pid_s = input_prop(i, "application.process.id");
  if (pid_s) {
    char self[32];
    std::snprintf(self, sizeof(self), "%d", static_cast<int>(::getpid()));
    if (std::strcmp(pid_s, self) == 0) return true;
  }
  const char* keys[] = {i->name, input_prop(i, "application.name"),
                        input_prop(i, "application.process.binary"),
                        input_prop(i, "application.id")};
  for (const char* k : keys) {
    if (contains_ci(k, "monasphere") || contains_ci(k, "openal") || contains_ci(k, "alsoft") ||
        contains_ci(k, "miniaudio"))
      return true;
  }
  return false;
}

/** Resolve monaSphereYt sink index via `pactl list short sinks` (0 = missing). */
uint32_t pactl_yt_sink_index() {
  FILE* f = popen("pactl list short sinks 2>/dev/null", "r");
  if (!f) return 0;
  char line[512];
  uint32_t idx = 0;
  while (std::fgets(line, sizeof(line), f)) {
    unsigned id = 0;
    char name[256] = {};
    if (std::sscanf(line, "%u %255s", &id, name) == 2 &&
        std::strcmp(name, PulseMonitor::kYtSinkName) == 0) {
      idx = id;
      break;
    }
  }
  (void)pclose(f);
  return idx;
}

/**
 * pactl fallback: move browser-like streams onto monaSphereYt; mute other HMD
 * competitors. Sink lines are numeric indices — never match the name string.
 */
int pactl_silence_hmd_competitors() {
  const uint32_t yt_idx = pactl_yt_sink_index();
  FILE* list = popen("pactl list sink-inputs 2>/dev/null", "r");
  if (!list) return 0;
  int muted = 0;
  char line[512];
  int cur = -1;
  bool browser = false;
  bool ours = false;
  uint32_t sink_idx = 0;
  bool have_sink = false;
  auto flush = [&] {
    if (cur < 0) return;
    char cmd[192];
    const bool on_yt = have_sink && yt_idx != 0 && sink_idx == yt_idx;
    if (browser && !ours && !on_yt && yt_idx != 0) {
      std::snprintf(cmd, sizeof(cmd), "pactl move-sink-input %d %s 2>/dev/null", cur,
                    PulseMonitor::kYtSinkName);
      (void)std::system(cmd);
      // Never mute browser streams — they feed monaSphereYt.monitor for Flat HRTF.
      std::snprintf(cmd, sizeof(cmd), "pactl set-sink-input-mute %d 0 2>/dev/null", cur);
      (void)std::system(cmd);
    } else if (!ours && !on_yt && !browser) {
      // Mute other apps still on a real output (JamesDSP / HDMI / HMD).
      std::snprintf(cmd, sizeof(cmd), "pactl set-sink-input-mute %d 1 2>/dev/null", cur);
      (void)std::system(cmd);
      ++muted;
    } else if (browser && !ours && on_yt) {
      std::snprintf(cmd, sizeof(cmd), "pactl set-sink-input-mute %d 0 2>/dev/null", cur);
      (void)std::system(cmd);
    }
    cur = -1;
    browser = false;
    ours = false;
    sink_idx = 0;
    have_sink = false;
  };
  while (std::fgets(line, sizeof(line), list)) {
    unsigned id = 0;
    if (std::sscanf(line, "Sink Input #%u", &id) == 1) {
      flush();
      cur = static_cast<int>(id);
      continue;
    }
    if (contains_ci(line, "chrome") || contains_ci(line, "chromium") ||
        contains_ci(line, "monasphereyt") || contains_ci(line, "brave") ||
        contains_ci(line, "google-chrome") || contains_ci(line, "firefox") ||
        contains_ci(line, "librewolf") || contains_ci(line, "msedge")) {
      browser = true;
    }
    if (contains_ci(line, "application.process.binary = \"monasphere\"") ||
        contains_ci(line, "application.name = \"monasphere\"") ||
        contains_ci(line, "openal") || contains_ci(line, "alsoft") ||
        contains_ci(line, "miniaudio")) {
      ours = true;
    }
    // "\tSink: 68" — numeric sink index (PipeWire-Pulse).
    unsigned sidx = 0;
    if (std::sscanf(line, "\tSink: %u", &sidx) == 1 ||
        std::sscanf(line, "    Sink: %u", &sidx) == 1) {
      sink_idx = sidx;
      have_sink = true;
    }
  }
  flush();
  (void)pclose(list);
  return muted;
}
#endif

}  // namespace

bool PulseMonitor::ok() const {
  std::lock_guard<std::mutex> lock(mu_);
  return ok_;
}

std::string PulseMonitor::source() const {
  std::lock_guard<std::mutex> lock(mu_);
  return source_;
}

std::string PulseMonitor::last_error() const {
  std::lock_guard<std::mutex> lock(mu_);
  return last_error_;
}

bool PulseMonitor::open(const std::string& source) {
#if !defined(VRP_HAS_PULSE)
  last_error_ = "PulseAudio not available";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  close_unlocked();
  if (source.empty()) {
    last_error_ = "empty monitor source";
    return false;
  }
  pa_sample_spec ss{};
  ss.format = PA_SAMPLE_S16LE;
  ss.rate = static_cast<uint32_t>(kRate);
  ss.channels = static_cast<uint8_t>(kChannels);
  pa_buffer_attr attr{};
  attr.fragsize = static_cast<uint32_t>(kRate / 50 * kChannels * sizeof(int16_t));  // ~20ms
  attr.maxlength = static_cast<uint32_t>(-1);
  attr.tlength = attr.prebuf = attr.minreq = static_cast<uint32_t>(-1);

  // Ensure monitor is not left suspended from a previous interrupt().
  {
    char cmd[256];
    std::snprintf(cmd, sizeof(cmd), "pactl suspend-source %s 0 2>/dev/null", source.c_str());
    (void)std::system(cmd);
  }

  int err = 0;
  pa_simple* s = pa_simple_new(nullptr, "monaSphere", PA_STREAM_RECORD, source.c_str(), "yt-monitor",
                               &ss, nullptr, &attr, &err);
  if (!s) {
    last_error_ = std::string("pa_simple_new: ") + pa_strerror(err);
    VRP_LOG("PulseMonitor: %s (source=%s)", last_error_.c_str(), source.c_str());
    return false;
  }
  stream_ = s;
  source_ = source;
  ok_ = true;
  VRP_LOG("PulseMonitor: recording %s (%d Hz stereo S16)", source.c_str(), kRate);
  return true;
#endif
}

void PulseMonitor::interrupt() {
  std::string src;
  {
    std::lock_guard<std::mutex> lock(mu_);
    src = source_;
    ok_ = false;  // readers should bail after unblock
  }
  if (src.empty()) return;
  // Wake a blocked pa_simple_read without freeing the stream (join, then close).
  char cmd[256];
  std::snprintf(cmd, sizeof(cmd), "pactl suspend-source %s 1 2>/dev/null", src.c_str());
  (void)std::system(cmd);
}

void PulseMonitor::close_unlocked() {
#if defined(VRP_HAS_PULSE)
  if (stream_) {
    pa_simple_free(static_cast<pa_simple*>(stream_));
    stream_ = nullptr;
  }
#endif
  ok_ = false;
  source_.clear();
}

void PulseMonitor::close() {
  std::lock_guard<std::mutex> lock(mu_);
  close_unlocked();
}

int PulseMonitor::read_s16_stereo(int16_t* dst, int max_frames) {
  if (!dst || max_frames <= 0) return -1;
#if !defined(VRP_HAS_PULSE)
  return -1;
#else
  pa_simple* s = nullptr;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!ok_ || !stream_) return -1;
    s = static_cast<pa_simple*>(stream_);
  }
  // Read without holding mu_ so interrupt()/close can run; close must only run
  // after the reader thread has joined (see AudioPlayer::stop_live_thread).
  const size_t bytes = static_cast<size_t>(max_frames) * static_cast<size_t>(kChannels) * sizeof(int16_t);
  int err = 0;
  if (pa_simple_read(s, dst, bytes, &err) < 0) {
    std::lock_guard<std::mutex> lock(mu_);
    last_error_ = pa_strerror(err);
    return -1;
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!ok_ || stream_ != s) return -1;
  }
  return max_frames;
#endif
}

uint32_t PulseMonitor::last_module_index() {
  return g_yt_null_module.load(std::memory_order_acquire);
}

std::string PulseMonitor::ensure_yt_null_sink() {
#if !defined(VRP_HAS_PULSE)
  return {};
#else
  PulseSession ps;
  if (!ps.connect("monaSphere-yt-sink")) {
    VRP_LOG("PulseMonitor: ensure_yt_null_sink — Pulse connect failed");
    return {};
  }

  const std::string sink = kYtSinkName;
  const std::string monitor = sink + ".monitor";
  if (sink_exists(ps, sink.c_str())) {
    // Recover module index if process restarted while the sink lingered.
    if (g_yt_null_module.load() == 0) {
      FILE* list = popen("pactl list modules 2>/dev/null", "r");
      if (list) {
        char line[1024];
        int cur = -1;
        while (std::fgets(line, sizeof(line), list)) {
          unsigned id = 0;
          if (std::sscanf(line, "Module #%u", &id) == 1) {
            cur = static_cast<int>(id);
            continue;
          }
          if (cur >= 0 && (contains_ci(line, "monaSphereYt") || contains_ci(line, kYtSinkName))) {
            g_yt_null_module.store(static_cast<uint32_t>(cur), std::memory_order_release);
            break;
          }
        }
        (void)pclose(list);
      }
    }
    VRP_LOG("PulseMonitor: reusing null sink %s (module=%u)", sink.c_str(),
            g_yt_null_module.load());
    return monitor;
  }

  // Internal capture sink only. Never use audio-input-microphone — Plasma then
  // lists monaSphereYt as an unknown mic/output and default routing breaks.
  // device.class=filter + media.role=filter: hide from most mixer pickers.
  FILE* p = popen("pactl load-module module-null-sink "
                  "sink_name=monaSphereYt "
                  "sink_properties=device.description=\"monaSphere live (internal)\","
                  "device.class=filter,"
                  "media.role=filter,"
                  "node.virtual=true,"
                  "device.icon_name=audio-card "
                  "rate=48000 channels=2 2>/dev/null",
                  "r");
  uint32_t mod = 0;
  if (p) {
    char buf[64]{};
    if (std::fgets(buf, sizeof(buf), p)) {
      unsigned id = 0;
      if (std::sscanf(buf, "%u", &id) == 1) mod = id;
    }
    (void)pclose(p);
  }
  g_yt_null_module.store(mod, std::memory_order_release);
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  if (!sink_exists(ps, sink.c_str())) {
    VRP_LOG("PulseMonitor: failed to create null sink %s", sink.c_str());
    g_yt_null_module.store(0, std::memory_order_release);
    return {};
  }
  VRP_LOG("PulseMonitor: created null sink %s (module=%u, filter-class)", sink.c_str(), mod);
  return monitor;
#endif
}

void PulseMonitor::destroy_yt_null_sink() {
#if !defined(VRP_HAS_PULSE)
  return;
#else
  int unloaded = 0;
  const uint32_t remembered = g_yt_null_module.exchange(0, std::memory_order_acq_rel);
  if (remembered != 0) {
    char cmd[96];
    std::snprintf(cmd, sizeof(cmd), "pactl unload-module %u 2>/dev/null", remembered);
    if (std::system(cmd) == 0) {
      ++unloaded;
      VRP_LOG("PulseMonitor: unloaded remembered module %u", remembered);
    }
  }

  // Full `pactl list modules` — short list often omits sink_name on PipeWire.
  FILE* list = popen("pactl list modules 2>/dev/null", "r");
  if (list) {
    char line[1024];
    int cur = -1;
    bool ours = false;
    auto flush = [&] {
      if (cur >= 0 && ours) {
        char cmd[96];
        std::snprintf(cmd, sizeof(cmd), "pactl unload-module %d 2>/dev/null", cur);
        if (std::system(cmd) == 0) {
          ++unloaded;
          VRP_LOG("PulseMonitor: unloaded module %d (monaSphereYt)", cur);
        }
      }
      cur = -1;
      ours = false;
    };
    while (std::fgets(line, sizeof(line), list)) {
      unsigned id = 0;
      if (std::sscanf(line, "Module #%u", &id) == 1) {
        flush();
        cur = static_cast<int>(id);
        continue;
      }
      if (contains_ci(line, "monaSphereYt") || contains_ci(line, kYtSinkName)) ours = true;
    }
    flush();
    (void)pclose(list);
  }

  // PipeWire node leftover (shows in some mixers after module unload).
  (void)std::system(
      "pw-dump 2>/dev/null | grep -F '\"node.name\": \"monaSphereYt\"' >/dev/null && "
      "pw-cli ls Node 2>/dev/null | awk '/id /{id=$2} /node.name.*=.*monaSphereYt/{print id}' | "
      "while read -r id; do pw-cli destroy \"$id\" 2>/dev/null; done");

  PulseSession ps;
  if (ps.connect("monaSphere-yt-destroy")) {
    if (sink_exists(ps, kYtSinkName)) {
      VRP_LOG("PulseMonitor: WARNING sink %s still present after unload", kYtSinkName);
    } else {
      VRP_LOG("PulseMonitor: null sink %s gone (unloaded=%d)", kYtSinkName, unloaded);
    }
  } else {
    VRP_LOG("PulseMonitor: destroy_yt_null_sink done (unloaded=%d)", unloaded);
  }
#endif
}

int PulseMonitor::route_browser_to_yt_sink() {
#if !defined(VRP_HAS_PULSE)
  return 0;
#else
  // pactl-only: no Pulse mainloop. Soft start must never block on this.
  // Shared Chrome creates sink-inputs only after media plays — callers retry.
  static std::mutex route_mu;
  std::unique_lock<std::mutex> route_lock(route_mu, std::try_to_lock);
  if (!route_lock.owns_lock()) return 0;

  const uint32_t yt_idx = pactl_yt_sink_index();
  if (yt_idx == 0) {
    VRP_LOG("PulseMonitor: route skip — %s missing", kYtSinkName);
    return 0;
  }

  FILE* list = popen("pactl list sink-inputs 2>/dev/null", "r");
  if (!list) return 0;

  int moved = 0;
  int browser_seen = 0;
  int dumped = 0;
  int cur = -1;
  bool browser = false;
  bool ours = false;
  uint32_t sink_idx = 0;
  bool have_sink = false;
  std::string app_name;
  std::string app_bin;

  auto flush = [&] {
    if (cur < 0) return;
    if (dumped < 12) {
      VRP_LOG("PulseMonitor: sink-input #%d app='%s' bin='%s' sink=%u", cur,
              app_name.empty() ? "?" : app_name.c_str(),
              app_bin.empty() ? "?" : app_bin.c_str(), have_sink ? sink_idx : 0u);
      ++dumped;
    }
    if (ours || !browser) {
      cur = -1;
      browser = ours = false;
      sink_idx = 0;
      have_sink = false;
      app_name.clear();
      app_bin.clear();
      return;
    }
    ++browser_seen;
    const bool on_yt = have_sink && sink_idx == yt_idx;
    if (!on_yt) {
      char cmd[192];
      std::snprintf(cmd, sizeof(cmd), "pactl move-sink-input %d %s 2>/dev/null", cur, kYtSinkName);
      if (std::system(cmd) == 0) {
        ++moved;
        VRP_LOG("PulseMonitor: move sink-input #%d (%s / %s) → %s", cur,
                app_name.empty() ? "?" : app_name.c_str(),
                app_bin.empty() ? "?" : app_bin.c_str(), kYtSinkName);
      }
    }
    // Keep browser unmuted on yt (never mute HMD competitors — Soft/Chrome race → silence).
    {
      char cmd[128];
      std::snprintf(cmd, sizeof(cmd), "pactl set-sink-input-mute %d 0 2>/dev/null", cur);
      (void)std::system(cmd);
    }
    cur = -1;
    browser = ours = false;
    sink_idx = 0;
    have_sink = false;
    app_name.clear();
    app_bin.clear();
  };

  char line[512];
  while (std::fgets(line, sizeof(line), list)) {
    unsigned id = 0;
    if (std::sscanf(line, "Sink Input #%u", &id) == 1) {
      flush();
      cur = static_cast<int>(id);
      continue;
    }
    if (contains_ci(line, "chrome") || contains_ci(line, "chromium") ||
        contains_ci(line, "monasphereyt") || contains_ci(line, "brave") ||
        contains_ci(line, "google-chrome") || contains_ci(line, "firefox") ||
        contains_ci(line, "librewolf") || contains_ci(line, "msedge")) {
      browser = true;
    }
    // Soft / miniaudio on HMD — never move onto yt.
    if (contains_ci(line, "application.process.binary = \"monasphere\"") ||
        contains_ci(line, "application.name = \"monasphere\"") ||
        contains_ci(line, "application.name = \"monaSphereHrtf\"") ||
        contains_ci(line, "application.id = \"monaSphereHrtf\"") ||
        contains_ci(line, "openal") || contains_ci(line, "alsoft") ||
        contains_ci(line, "miniaudio")) {
      ours = true;
    }
    {
      char val[256]{};
      if (std::sscanf(line, "\tapplication.name = \"%255[^\"]\"", val) == 1 ||
          std::sscanf(line, "    application.name = \"%255[^\"]\"", val) == 1)
        app_name = val;
      if (std::sscanf(line, "\tapplication.process.binary = \"%255[^\"]\"", val) == 1 ||
          std::sscanf(line, "    application.process.binary = \"%255[^\"]\"", val) == 1)
        app_bin = val;
    }
    unsigned sidx = 0;
    if (std::sscanf(line, "\tSink: %u", &sidx) == 1 ||
        std::sscanf(line, "    Sink: %u", &sidx) == 1) {
      sink_idx = sidx;
      have_sink = true;
    }
  }
  flush();
  (void)pclose(list);

  if (moved > 0) {
    VRP_LOG("PulseMonitor: moved %d browser stream(s) → %s (seen=%d)", moved, kYtSinkName,
            browser_seen);
  } else if (browser_seen == 0) {
    VRP_LOG("PulseMonitor: no browser sink-input yet (will retry)");
  } else {
    VRP_LOG("PulseMonitor: browser already on %s (seen=%d)", kYtSinkName, browser_seen);
  }
  return moved;
#endif
}

int PulseMonitor::restore_browser_routing() {
#if !defined(VRP_HAS_PULSE)
  return 0;
#else
  PulseSession ps;
  if (!ps.connect("monaSphere-yt-restore")) {
    destroy_yt_null_sink();
    return 0;
  }

  char* def = nullptr;
  ps.wait_op(pa_context_get_server_info(
      ps.ctx,
      [](pa_context*, const pa_server_info* i, void* u) {
        if (i && i->default_sink_name)
          *static_cast<char**>(u) = pa_xstrdup(i->default_sink_name);
      },
      &def));
  if (!def) {
    ps.disconnect();
    destroy_yt_null_sink();
    return 0;
  }
  const std::string dest = def;
  pa_xfree(def);
  const uint32_t yt_idx = sink_index_by_name(ps, kYtSinkName);

  struct MoveSt {
    const char* dest = nullptr;
    uint32_t yt_idx = PA_INVALID_INDEX;
    int moved = 0;
    int unmuted = 0;
  } st{dest.c_str(), yt_idx, 0, 0};
  ps.wait_op(pa_context_get_sink_input_info_list(
      ps.ctx,
      [](pa_context* c, const pa_sink_input_info* i, int eol, void* u) {
        auto* s = static_cast<MoveSt*>(u);
        if (eol || !i) return;
        // Undo session mutes (browser + any other competitors we silenced).
        if (i->mute && !is_mona_openal_input(i)) {
          pa_operation* uop = pa_context_set_sink_input_mute(c, i->index, 0, nullptr, nullptr);
          if (uop) {
            pa_operation_unref(uop);
            ++s->unmuted;
          }
        }
        if (s->yt_idx != PA_INVALID_INDEX && i->sink == s->yt_idx) {
          pa_operation* op =
              pa_context_move_sink_input_by_name(c, i->index, s->dest, nullptr, nullptr);
          if (op) {
            pa_operation_unref(op);
            ++s->moved;
          }
        }
      },
      &st));
  ps.drain(40);
  if (st.moved > 0 || st.unmuted > 0)
    VRP_LOG("PulseMonitor: restored (moved=%d unmuted=%d) → %s", st.moved, st.unmuted, dest.c_str());

  // If the session default was somehow monaSphereYt, pick a real hardware sink.
  if (dest == kYtSinkName || dest.find("monaSphereYt") != std::string::npos) {
    FILE* sinks = popen("pactl list short sinks 2>/dev/null", "r");
    if (sinks) {
      char line[512];
      while (std::fgets(line, sizeof(line), sinks)) {
        char name[256]{};
        unsigned idx = 0;
        if (std::sscanf(line, "%u %255s", &idx, name) != 2) continue;
        if (std::strcmp(name, kYtSinkName) == 0) continue;
        char cmd[384];
        std::snprintf(cmd, sizeof(cmd), "pactl set-default-sink %s 2>/dev/null", name);
        if (std::system(cmd) == 0) {
          VRP_LOG("PulseMonitor: default sink was %s — switched to %s", dest.c_str(), name);
          break;
        }
      }
      (void)pclose(sinks);
    }
  }
  ps.disconnect();
  // Remove null sink from the mixer after streams have left it.
  destroy_yt_null_sink();
  return st.moved;
#endif
}

}  // namespace vrp
