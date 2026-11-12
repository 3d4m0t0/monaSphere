#pragma once

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

/**
 * Persist log gates (monasphere.conf):
 *   live_display_log=0|1  → ~/.config/monasphere/live-display.log (+ /tmp/…)
 *   live_teardown_log=0|1 → ~/.config/monasphere/live-teardown.log (+ /tmp/…)
 * Defaults: both off (Live dma breadcrumbs were heavy; teardown optional).
 */
inline std::atomic<bool>& vrp_persist_display_enabled_flag() {
  static std::atomic<bool> on{false};
  return on;
}
inline std::atomic<bool>& vrp_persist_teardown_enabled_flag() {
  static std::atomic<bool> on{false};
  return on;
}
inline void vrp_set_persist_display_log(bool on) {
  vrp_persist_display_enabled_flag().store(on, std::memory_order_release);
}
inline void vrp_set_persist_teardown_log(bool on) {
  vrp_persist_teardown_enabled_flag().store(on, std::memory_order_release);
}
inline bool vrp_persist_display_enabled() {
  return vrp_persist_display_enabled_flag().load(std::memory_order_acquire);
}
inline bool vrp_persist_teardown_enabled() {
  return vrp_persist_teardown_enabled_flag().load(std::memory_order_acquire);
}

/**
 * Append one line to durable files that survive GNOME logout (stdout alone dies
 * with the session).
 *   /tmp/monasphere-<name>
 *   ~/.config/monasphere/<name>
 */
inline void vrp_persist_named_logv(const char* name, const char* fmt, va_list ap) {
  if (!name || !name[0]) name = "live-teardown.log";
  char body[1024];
  std::vsnprintf(body, sizeof(body), fmt, ap);

  char stamp[64];
  {
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t t = clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d:%02d.%03d", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<int>(ms.count()));
  }
  char line[1200];
  std::snprintf(line, sizeof(line), "%s [vrp] %s\n", stamp, body);

  auto write_path = [&](const char* path) {
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return;
    const size_t n = std::strlen(line);
    (void)::write(fd, line, n);
    (void)::fsync(fd);
    ::close(fd);
  };

  char tmp_path[512];
  std::snprintf(tmp_path, sizeof(tmp_path), "/tmp/monasphere-%s", name);
  write_path(tmp_path);
  const char* home = std::getenv("HOME");
  if (home && home[0]) {
    char cfg[512], dir[512], path[512];
    std::snprintf(cfg, sizeof(cfg), "%s/.config", home);
    std::snprintf(dir, sizeof(dir), "%s/.config/monasphere", home);
    std::snprintf(path, sizeof(path), "%s/.config/monasphere/%s", home, name);
    (void)::mkdir(cfg, 0755);
    (void)::mkdir(dir, 0755);
    write_path(path);
  }
}

inline void vrp_persist_named_log(const char* name, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vrp_persist_named_logv(name, fmt, ap);
  va_end(ap);
}

/** Meta teardown breadcrumbs (logout-safe). No-op when live_teardown_log=0. */
inline void vrp_persist_logv(const char* fmt, va_list ap) {
  if (!vrp_persist_teardown_enabled()) return;
  vrp_persist_named_logv("live-teardown.log", fmt, ap);
}

inline void vrp_persist_log(const char* fmt, ...) {
  if (!vrp_persist_teardown_enabled()) return;
  va_list ap;
  va_start(ap, fmt);
  vrp_persist_logv(fmt, ap);
  va_end(ap);
}

/** Live display-order breadcrumbs (logout-safe). No-op when live_display_log=0. */
inline void vrp_persist_display_log(const char* fmt, ...) {
  if (!vrp_persist_display_enabled()) return;
  va_list ap;
  va_start(ap, fmt);
  vrp_persist_named_logv("live-display.log", fmt, ap);
  va_end(ap);
}

/** Local wall time HH:MM:SS.mmm for stdout / UI log prefixes. */
inline void vrp_format_log_stamp(char* buf, size_t n) {
  if (!buf || n == 0) return;
  using clock = std::chrono::system_clock;
  const auto now = clock::now();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) %
                  1000;
  const std::time_t t = clock::to_time_t(now);
  std::tm tm{};
  localtime_r(&t, &tm);
  std::snprintf(buf, n, "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec,
                static_cast<int>(ms.count()));
}

#define VRP_LOG(...)                         \
  do {                                       \
    char _vrp_ts[16];                        \
    vrp_format_log_stamp(_vrp_ts, sizeof(_vrp_ts)); \
    std::fprintf(stdout, "[vrp %s] ", _vrp_ts); \
    std::fprintf(stdout, __VA_ARGS__);       \
    std::fprintf(stdout, "\n");              \
    std::fflush(stdout);                     \
  } while (0)

/** stdout + durable files when live_teardown_log=1. Prefer for Meta teardown. */
#define VRP_TEARDOWN_LOG(...)                \
  do {                                       \
    if (vrp_persist_teardown_enabled()) {    \
      VRP_LOG(__VA_ARGS__);                  \
      vrp_persist_log(__VA_ARGS__);          \
    }                                        \
  } while (0)

/** stdout + live-display.log when live_display_log=1. Present/flush probes. */
#define VRP_DISPLAY_LOG(...)                 \
  do {                                       \
    if (vrp_persist_display_enabled()) {     \
      VRP_LOG(__VA_ARGS__);                  \
      vrp_persist_display_log(__VA_ARGS__);  \
    }                                        \
  } while (0)

#define VRP_ERR(...)                         \
  do {                                       \
    char _vrp_ts[16];                        \
    vrp_format_log_stamp(_vrp_ts, sizeof(_vrp_ts)); \
    std::fprintf(stderr, "[vrp:error %s] ", _vrp_ts); \
    std::fprintf(stderr, __VA_ARGS__);       \
    std::fprintf(stderr, "\n");              \
    std::fflush(stderr);                     \
  } while (0)

/** Verbose diagnostics when env VRP_DEBUG is set (non-empty, not "0"). */
inline bool vrp_debug_enabled() {
  const char* e = std::getenv("VRP_DEBUG");
  return e && e[0] && std::strcmp(e, "0") != 0;
}

#define VRP_DBG(...)                         \
  do {                                       \
    if (vrp_debug_enabled()) {               \
      char _vrp_ts[16];                      \
      vrp_format_log_stamp(_vrp_ts, sizeof(_vrp_ts)); \
      std::fprintf(stdout, "[vrp:debug %s] ", _vrp_ts); \
      std::fprintf(stdout, __VA_ARGS__);     \
      std::fprintf(stdout, "\n");            \
      std::fflush(stdout);                   \
    }                                        \
  } while (0)

/** Scoped stage timer — debug-only (VRP_DEBUG); silent in normal builds. */
struct VrpStageTimer {
  const char* name;
  int threshold_ms;
  std::chrono::steady_clock::time_point t0;
  explicit VrpStageTimer(const char* n, int thr_ms = 50)
      : name(n), threshold_ms(thr_ms), t0(std::chrono::steady_clock::now()) {}
  ~VrpStageTimer() {
    if (!vrp_debug_enabled()) return;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    if (ms >= threshold_ms) {
      VRP_DBG("slow stage %s %lld ms", name, static_cast<long long>(ms));
    }
  }
};

[[noreturn]] inline void vrp_fatal(const std::string& msg) {
  throw std::runtime_error(msg);
}

#define VRP_CHECK(cond, msg)                 \
  do {                                       \
    if (!(cond)) {                           \
      vrp_fatal(std::string(msg));           \
    }                                        \
  } while (0)
