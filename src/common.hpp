#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

#define VRP_LOG(...)                         \
  do {                                       \
    std::fprintf(stdout, "[vrp] ");          \
    std::fprintf(stdout, __VA_ARGS__);       \
    std::fprintf(stdout, "\n");              \
    std::fflush(stdout);                     \
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
