#pragma once

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
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
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

/** Meta teardown breadcrumbs (logout-safe). */
inline void vrp_persist_logv(const char* fmt, va_list ap) {
  vrp_persist_named_logv("live-teardown.log", fmt, ap);
}

inline void vrp_persist_log(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vrp_persist_logv(fmt, ap);
  va_end(ap);
}

/** Live display-order breadcrumbs (logout-safe). */
inline void vrp_persist_display_log(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vrp_persist_named_logv("live-display.log", fmt, ap);
  va_end(ap);
}

#define VRP_LOG(...)                         \
  do {                                       \
    std::fprintf(stdout, "[vrp] ");          \
    std::fprintf(stdout, __VA_ARGS__);       \
    std::fprintf(stdout, "\n");              \
    std::fflush(stdout);                     \
  } while (0)

/** stdout + durable files (logout-safe). Prefer for Meta teardown steps. */
#define VRP_TEARDOWN_LOG(...)                \
  do {                                       \
    VRP_LOG(__VA_ARGS__);                    \
    vrp_persist_log(__VA_ARGS__);            \
  } while (0)

/** stdout + live-display.log (logout-safe). Present/flush order probes. */
#define VRP_DISPLAY_LOG(...)                 \
  do {                                       \
    VRP_LOG(__VA_ARGS__);                    \
    vrp_persist_display_log(__VA_ARGS__);    \
  } while (0)

#define VRP_ERR(...)                         \
  do {                                       \
    std::fprintf(stderr, "[vrp:error] ");    \
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
      std::fprintf(stdout, "[vrp:debug] ");  \
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
