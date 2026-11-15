#include "common/crash_guard.hpp"

#include "common.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <thread>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

namespace vrp {
namespace crash_guard {
namespace {

// Fixed buffer for signal handler (no heap). Filled once by install()/mark.
char g_cookie_path[512]{};
volatile sig_atomic_t g_installed = 0;

void fill_cookie_path() {
  if (g_cookie_path[0]) return;
  const char* base = std::getenv("XDG_CONFIG_HOME");
  char tmp[512];
  if (base && base[0]) {
    std::snprintf(tmp, sizeof(tmp), "%s/monasphere/xr_session.lock", base);
  } else {
    const char* home = std::getenv("HOME");
    if (!home || !home[0]) return;
    std::snprintf(tmp, sizeof(tmp), "%s/.config/monasphere/xr_session.lock", home);
  }
  // Copy into static buffer (truncates if absurdly long).
  std::snprintf(g_cookie_path, sizeof(g_cookie_path), "%s", tmp);
}

void ensure_parent_dir(const char* path) {
  // path = .../monasphere/xr_session.lock — mkdir parent only.
  char dir[512];
  std::snprintf(dir, sizeof(dir), "%s", path);
  char* slash = std::strrchr(dir, '/');
  if (!slash) return;
  *slash = '\0';
  // Best-effort; ignore EEXIST.
  (void)::mkdir(dir, 0700);
}

// Async-signal-safe: mark cookie crashed then _exit.
void on_fatal_signal(int sig) {
  const char prefix[] = "monasphere: fatal signal, exiting\n";
  (void)::write(STDERR_FILENO, prefix, sizeof(prefix) - 1);

  if (g_cookie_path[0]) {
    // O_CREAT|O_TRUNC|O_WRONLY — overwrite active cookie with crashed marker.
    const int fd = ::open(g_cookie_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
      const char body[] = "crashed\n";
      (void)::write(fd, body, sizeof(body) - 1);
      (void)::close(fd);
    }
  }
  ::_exit(128 + sig);
}

void on_terminate() {
  const char msg[] = "monasphere: uncaught exception, exiting\n";
  (void)::write(STDERR_FILENO, msg, sizeof(msg) - 1);
  if (g_cookie_path[0]) {
    const int fd = ::open(g_cookie_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
      const char body[] = "crashed\n";
      (void)::write(fd, body, sizeof(body) - 1);
      (void)::close(fd);
    }
  }
  ::_Exit(1);
}

}  // namespace

void install() {
  if (g_installed) return;
  fill_cookie_path();
  if (g_cookie_path[0]) ensure_parent_dir(g_cookie_path);

  struct sigaction sa {};
  sa.sa_handler = on_fatal_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESETHAND;  // avoid recursive handler if we fault again
  (void)sigaction(SIGSEGV, &sa, nullptr);
  (void)sigaction(SIGABRT, &sa, nullptr);
  (void)sigaction(SIGBUS, &sa, nullptr);
  (void)sigaction(SIGFPE, &sa, nullptr);
  (void)sigaction(SIGILL, &sa, nullptr);

  std::set_terminate(on_terminate);
  g_installed = 1;
  VRP_LOG("crash_guard: installed (cookie=%s)", g_cookie_path[0] ? g_cookie_path : "(none)");
}

std::string cookie_path() {
  fill_cookie_path();
  return g_cookie_path[0] ? std::string(g_cookie_path) : std::string();
}

void mark_session_active() {
  fill_cookie_path();
  if (!g_cookie_path[0]) return;
  ensure_parent_dir(g_cookie_path);
  const int fd = ::open(g_cookie_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    VRP_ERR("crash_guard: cannot write cookie (%s)", std::strerror(errno));
    return;
  }
  char body[64];
  const int n = std::snprintf(body, sizeof(body), "active\n%d\n", static_cast<int>(::getpid()));
  if (n > 0) (void)::write(fd, body, static_cast<size_t>(n));
  (void)::close(fd);
}

void clear_session_cookie() {
  fill_cookie_path();
  if (!g_cookie_path[0]) return;
  if (::unlink(g_cookie_path) == 0) {
    VRP_DBG("crash_guard: cookie cleared");
  }
}

bool previous_session_unclean() {
  fill_cookie_path();
  if (!g_cookie_path[0]) return false;
  const int fd = ::open(g_cookie_path, O_RDONLY);
  if (fd < 0) return false;
  (void)::close(fd);
  return true;
}

bool consume_unclean_session() {
  if (!previous_session_unclean()) return false;
  clear_session_cookie();
  return true;
}

namespace {

void fill_vk_settle_path(char* out, size_t out_sz) {
  out[0] = '\0';
  const char* base = std::getenv("XDG_CONFIG_HOME");
  if (base && base[0]) {
    std::snprintf(out, out_sz, "%s/monasphere/vk_client_released", base);
  } else {
    const char* home = std::getenv("HOME");
    if (!home || !home[0]) return;
    std::snprintf(out, out_sz, "%s/.config/monasphere/vk_client_released", home);
  }
}

bool read_vk_settle_ms(int64_t* out_ms) {
  char path[512];
  fill_vk_settle_path(path, sizeof(path));
  if (!path[0]) return false;
  const int fd = ::open(path, O_RDONLY);
  if (fd < 0) return false;
  char buf[64]{};
  const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
  (void)::close(fd);
  if (n <= 0) return false;
  buf[n] = '\0';
  char* end = nullptr;
  const long long v = std::strtoll(buf, &end, 10);
  if (end == buf || v <= 0) return false;
  *out_ms = static_cast<int64_t>(v);
  return true;
}

}  // namespace

void mark_vulkan_client_released() {
  char path[512];
  fill_vk_settle_path(path, sizeof(path));
  if (!path[0]) return;
  ensure_parent_dir(path);
  const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return;
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
  char body[64];
  const int n = std::snprintf(body, sizeof(body), "%lld\n", static_cast<long long>(ms));
  if (n > 0) (void)::write(fd, body, static_cast<size_t>(n));
  (void)::close(fd);
  VRP_LOG("crash_guard: vk client released stamped");
}

bool vulkan_client_release_recent(int max_age_ms) {
  int64_t stamped = 0;
  if (!read_vk_settle_ms(&stamped)) return false;
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  const int64_t age = now - stamped;
  return age >= 0 && age < max_age_ms;
}

bool settle_vulkan_client_release(int min_age_ms) {
  int64_t stamped = 0;
  if (!read_vk_settle_ms(&stamped)) return false;
  const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  int64_t age = now_ms - stamped;
  char path[512];
  fill_vk_settle_path(path, sizeof(path));
  if (age < 0) age = 0;
  if (age >= min_age_ms) {
    VRP_LOG("crash_guard: vk settle stamp age %lld ms (>=%d) — no wait",
            static_cast<long long>(age), min_age_ms);
    if (path[0]) (void)::unlink(path);
    return false;
  }
  const int64_t wait_ms = min_age_ms - age;
  VRP_LOG("crash_guard: settle vk client %lld ms (age was %lld, need %d; NVIDIA residual)",
          static_cast<long long>(wait_ms), static_cast<long long>(age), min_age_ms);
  std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
  if (path[0]) (void)::unlink(path);
  return true;
}

}  // namespace crash_guard
}  // namespace vrp
