#pragma once

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
  } while (0)

#define VRP_ERR(...)                         \
  do {                                       \
    std::fprintf(stderr, "[vrp:error] ");    \
    std::fprintf(stderr, __VA_ARGS__);       \
    std::fprintf(stderr, "\n");              \
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
    }                                        \
  } while (0)

[[noreturn]] inline void vrp_fatal(const std::string& msg) {
  throw std::runtime_error(msg);
}

#define VRP_CHECK(cond, msg)                 \
  do {                                       \
    if (!(cond)) {                           \
      vrp_fatal(std::string(msg));           \
    }                                        \
  } while (0)
