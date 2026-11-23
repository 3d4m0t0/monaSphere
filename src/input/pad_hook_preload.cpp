/**
 * LD_PRELOAD library for [アプリ] guests (RetroArch etc.).
 * Interposes SDL2 GameController / Joystick getters to:
 *   - PS (GUIDE) short → notify monaSphere MENU, never expose GUIDE to the app
 *   - PS long → pulse GUIDE to the app (short press)
 *   - Options (START) long → notify RECENTER; short Options still visible to the app
 *   - BLOCK 1 from host → all buttons/axes zero (VR menu owns the pad)
 *
 * No /dev/uinput required. Socket path: env MONASPHERE_PAD_SOCK.
 */
#define _GNU_SOURCE 1

#include "input/pad_hook_proto.hpp"

#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kBtnGuide = 5;  // SDL_CONTROLLER_BUTTON_GUIDE (PS)
constexpr int kBtnStart = 6;  // SDL_CONTROLLER_BUTTON_START (Options)
constexpr int kPsLongMs = 700;
constexpr int kCooldownMs = 400;
constexpr int kShortPulsePolls = 4;

using GetButtonFn = std::uint8_t (*)(void*, int);
using GetAxisFn = std::int16_t (*)(void*, int);
using JoyGetButtonFn = std::uint8_t (*)(void*, int);
using JoyGetAxisFn = std::int16_t (*)(void*, int);

GetButtonFn real_gc_get_button = nullptr;
GetAxisFn real_gc_get_axis = nullptr;
JoyGetButtonFn real_joy_get_button = nullptr;
JoyGetAxisFn real_joy_get_axis = nullptr;

std::atomic<bool> g_block{false};
std::atomic<bool> g_sock_ready{false};
std::atomic<int> g_sock_fd{-1};

std::mutex g_ps_mu;
bool g_ps_down = false;
bool g_ps_long = false;
bool g_ps_host_gesture = false;  // short PS → menu; block GUIDE until cooldown
int g_short_pulse = 0;           // app sees GUIDE=1 for N polls (PS long → short)
Clock::time_point g_ps_down_at{};
Clock::time_point g_ps_ignore_until{};

std::mutex g_opt_mu;
bool g_opt_down = false;
bool g_opt_long = false;
bool g_opt_swallow = false;
int g_opt_short_pulse = 0;
Clock::time_point g_opt_down_at{};

void log_msg(const char* s) {
  std::fprintf(stderr, "[monasphere_padhook] %s\n", s);
}

void send_line(const char* line) {
  const int fd = g_sock_fd.load(std::memory_order_acquire);
  if (fd < 0 || !line) return;
  const std::string msg = std::string(line) + "\n";
  const ssize_t n = ::write(fd, msg.data(), msg.size());
  (void)n;
}

void resolve_sdl() {
  if (!real_gc_get_button) {
    real_gc_get_button =
        reinterpret_cast<GetButtonFn>(dlsym(RTLD_NEXT, "SDL_GameControllerGetButton"));
  }
  if (!real_gc_get_axis) {
    real_gc_get_axis = reinterpret_cast<GetAxisFn>(dlsym(RTLD_NEXT, "SDL_GameControllerGetAxis"));
  }
  if (!real_joy_get_button) {
    real_joy_get_button =
        reinterpret_cast<JoyGetButtonFn>(dlsym(RTLD_NEXT, "SDL_JoystickGetButton"));
  }
  if (!real_joy_get_axis) {
    real_joy_get_axis = reinterpret_cast<JoyGetAxisFn>(dlsym(RTLD_NEXT, "SDL_JoystickGetAxis"));
  }
}

std::uint8_t filter_guide(std::uint8_t raw) {
  const auto now = Clock::now();
  std::lock_guard<std::mutex> lock(g_ps_mu);

  if (g_short_pulse > 0) {
    --g_short_pulse;
    return 1;
  }

  if (now < g_ps_ignore_until) {
    if (raw == 0) g_ps_down = false;
    return 0;
  }

  if (raw != 0) {
    if (g_ps_host_gesture) return 0;
    if (!g_ps_down) {
      g_ps_down = true;
      g_ps_long = false;
      g_ps_down_at = now;
      log_msg("PS down (arm)");
    } else if (!g_ps_long) {
      const auto ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(now - g_ps_down_at).count();
      if (ms >= kPsLongMs) {
        g_ps_long = true;
        g_short_pulse = kShortPulsePolls;
        log_msg("PS LONG → app short PS");
      }
    }
    return 0;  // never expose held GUIDE while deciding
  }

  // raw == 0
  if (!g_ps_down) return 0;
  const bool was_long = g_ps_long;
  g_ps_down = false;
  if (was_long) {
    g_ps_ignore_until = now + std::chrono::milliseconds(kCooldownMs);
    log_msg("PS up after LONG — app pulse already started");
    return 0;
  }
  g_ps_host_gesture = true;
  g_ps_ignore_until = now + std::chrono::milliseconds(kCooldownMs);
  log_msg("PS up SHORT → MENU");
  send_line(vrp::pad_hook::kMsgMenu);
  return 0;
}

/** Options short → app pulse; Options long → recenter (hidden from app during hold). */
std::uint8_t filter_start(std::uint8_t raw) {
  const auto now = Clock::now();
  std::lock_guard<std::mutex> lock(g_opt_mu);
  if (g_opt_short_pulse > 0) {
    --g_opt_short_pulse;
    return 1;
  }
  if (raw != 0) {
    if (!g_opt_down) {
      g_opt_down = true;
      g_opt_long = false;
      g_opt_swallow = false;
      g_opt_down_at = now;
      log_msg("Options down (arm)");
    } else if (!g_opt_long) {
      const auto ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(now - g_opt_down_at).count();
      if (ms >= kPsLongMs) {
        g_opt_long = true;
        g_opt_swallow = true;
        log_msg("Options LONG → RECENTER");
        send_line(vrp::pad_hook::kMsgRecenter);
      }
    }
    return 0;  // delay short until release
  }
  if (!g_opt_down) return 0;
  const bool was_long = g_opt_long || g_opt_swallow;
  g_opt_down = false;
  g_opt_long = false;
  g_opt_swallow = false;
  if (was_long) {
    log_msg("Options up after LONG — suppress");
    return 0;
  }
  log_msg("Options SHORT → app");
  g_opt_short_pulse = kShortPulsePolls;
  return 1;
}

void end_host_gesture_if_idle() {
  const auto now = Clock::now();
  std::lock_guard<std::mutex> lock(g_ps_mu);
  if (g_ps_host_gesture && !g_ps_down && now >= g_ps_ignore_until) {
    g_ps_host_gesture = false;
    g_ps_long = false;
    log_msg("host-menu gesture end");
  }
}

void* sock_thread_main(void* /*arg*/) {
  const char* path = std::getenv(vrp::pad_hook::kEnvSock);
  if (!path || !path[0]) {
    log_msg("no MONASPHERE_PAD_SOCK — filter local-only");
    return nullptr;
  }

  for (int attempt = 0; attempt < 50; ++attempt) {
    if (attempt > 0) ::usleep(100000);
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) continue;
    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
      ::close(fd);
      continue;
    }
    g_sock_fd.store(fd, std::memory_order_release);
    g_sock_ready.store(true, std::memory_order_release);
    log_msg("connected to monaSphere");
    send_line(vrp::pad_hook::kMsgHello);

    std::string acc;
    char buf[256];
    for (;;) {
      const ssize_t n = ::read(fd, buf, sizeof(buf));
      if (n <= 0) break;
      acc.append(buf, static_cast<size_t>(n));
      for (;;) {
        const auto pos = acc.find('\n');
        if (pos == std::string::npos) break;
        std::string line = acc.substr(0, pos);
        acc.erase(0, pos + 1);
        while (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == vrp::pad_hook::kMsgBlockOn) {
          g_block.store(true, std::memory_order_release);
          log_msg("BLOCK 1 (menu owns pad)");
        } else if (line == vrp::pad_hook::kMsgBlockOff) {
          g_block.store(false, std::memory_order_release);
          log_msg("BLOCK 0 (guest owns pad)");
        }
      }
    }
    g_sock_fd.store(-1, std::memory_order_release);
    ::close(fd);
    log_msg("socket closed");
    break;
  }
  return nullptr;
}

void ensure_sock_thread() {
  static std::once_flag once;
  std::call_once(once, [] {
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &attr, sock_thread_main, nullptr) != 0) {
      log_msg("sock thread failed");
    }
    pthread_attr_destroy(&attr);
  });
}

}  // namespace

extern "C" {

__attribute__((constructor)) void monasphere_padhook_init() {
  resolve_sdl();
  ensure_sock_thread();
  std::fprintf(stderr, "[monasphere_padhook] loaded (SDL GC=%p Joy=%p sock=%s)\n",
               reinterpret_cast<void*>(real_gc_get_button),
               reinterpret_cast<void*>(real_joy_get_button),
               std::getenv(vrp::pad_hook::kEnvSock) ? std::getenv(vrp::pad_hook::kEnvSock)
                                                     : "(none)");
}

std::uint8_t SDL_GameControllerGetButton(void* gc, int button) {
  resolve_sdl();
  ensure_sock_thread();
  end_host_gesture_if_idle();
  if (!real_gc_get_button) return 0;
  const std::uint8_t raw = real_gc_get_button(gc, button);
  if (g_block.load(std::memory_order_acquire)) return 0;
  if (button == kBtnGuide) return filter_guide(raw);
  if (button == kBtnStart) return filter_start(raw);
  return raw;
}

std::int16_t SDL_GameControllerGetAxis(void* gc, int axis) {
  resolve_sdl();
  ensure_sock_thread();
  if (!real_gc_get_axis) return 0;
  if (g_block.load(std::memory_order_acquire)) return 0;
  return real_gc_get_axis(gc, axis);
}

std::uint8_t SDL_JoystickGetButton(void* joy, int button) {
  resolve_sdl();
  ensure_sock_thread();
  end_host_gesture_if_idle();
  if (!real_joy_get_button) return 0;
  const std::uint8_t raw = real_joy_get_button(joy, button);
  if (g_block.load(std::memory_order_acquire)) return 0;
  // DualSense via joystick API: Guide/Start often match GC indices.
  if (button == kBtnGuide) return filter_guide(raw);
  if (button == kBtnStart) return filter_start(raw);
  return raw;
}

std::int16_t SDL_JoystickGetAxis(void* joy, int axis) {
  resolve_sdl();
  ensure_sock_thread();
  if (!real_joy_get_axis) return 0;
  if (g_block.load(std::memory_order_acquire)) return 0;
  return real_joy_get_axis(joy, axis);
}

}  // extern "C"
