#include "common/crash_guard.hpp"

#include "common.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

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

}  // namespace crash_guard
}  // namespace vrp
