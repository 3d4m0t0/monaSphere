#include "input/pad_hook_bridge.hpp"
#include "input/pad_hook_proto.hpp"

#include "common.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <sstream>

namespace vrp {
namespace {

int set_nonblock(int fd) {
  const int fl = fcntl(fd, F_GETFL, 0);
  if (fl < 0) return -1;
  return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

}  // namespace

PadHookBridge::~PadHookBridge() { stop(); }

bool PadHookBridge::start() {
  std::lock_guard<std::mutex> lock(mu_);
  if (running_.load(std::memory_order_relaxed)) return true;
  close_all();

  listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listen_fd_ < 0) {
    VRP_ERR("PadHookBridge: socket failed: %s", std::strerror(errno));
    return false;
  }
  (void)set_nonblock(listen_fd_);

  sock_path_ = std::string("/tmp/monasphere-pad-") + std::to_string(static_cast<long>(::getuid())) +
               "-" + std::to_string(static_cast<long>(::getpid()));
  ::unlink(sock_path_.c_str());

  sockaddr_un addr {};
  addr.sun_family = AF_UNIX;
  if (sock_path_.size() >= sizeof(addr.sun_path)) {
    VRP_ERR("PadHookBridge: socket path too long");
    close_all();
    return false;
  }
  std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path_.c_str());
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    VRP_ERR("PadHookBridge: bind %s failed: %s", sock_path_.c_str(), std::strerror(errno));
    close_all();
    return false;
  }
  if (::listen(listen_fd_, 4) < 0) {
    VRP_ERR("PadHookBridge: listen failed: %s", std::strerror(errno));
    close_all();
    return false;
  }

  block_guest_ = false;
  block_sent_ = false;
  pending_menu_ = pending_recenter_ = pending_hello_ = false;
  running_.store(true, std::memory_order_release);
  VRP_LOG("PadHookBridge: listening on %s", sock_path_.c_str());
  return true;
}

void PadHookBridge::stop() {
  std::lock_guard<std::mutex> lock(mu_);
  if (!running_.load(std::memory_order_relaxed) && listen_fd_ < 0 && clients_.empty()) return;
  running_.store(false, std::memory_order_release);
  close_all();
  VRP_LOG("PadHookBridge: stopped");
}

std::string PadHookBridge::socket_path() const {
  std::lock_guard<std::mutex> lock(mu_);
  return sock_path_;
}

void PadHookBridge::set_block_guest(bool block) {
  std::lock_guard<std::mutex> lock(mu_);
  if (block_guest_ == block) return;
  block_guest_ = block;
  block_sent_ = false;  // force rebroadcast
  VRP_LOG("PadHookBridge: block_guest → %d", block ? 1 : 0);
}

void PadHookBridge::close_all() {
  for (int fd : clients_) {
    if (fd >= 0) ::close(fd);
  }
  clients_.clear();
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  if (!sock_path_.empty()) {
    ::unlink(sock_path_.c_str());
    sock_path_.clear();
  }
}

void PadHookBridge::broadcast(const char* line) {
  if (!line) return;
  const std::string msg = std::string(line) + "\n";
  for (auto it = clients_.begin(); it != clients_.end();) {
    const ssize_t n = ::write(*it, msg.data(), msg.size());
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      ++it;
      continue;
    }
    if (n < 0) {
      VRP_LOG("PadHookBridge: client write failed — drop");
      ::close(*it);
      it = clients_.erase(it);
      continue;
    }
    ++it;
  }
}

void PadHookBridge::drain_client(int fd) {
  char buf[256];
  for (;;) {
    const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      return;
    }
    if (n == 0) return;
    buf[n] = '\0';
    std::istringstream in(buf);
    std::string line;
    while (std::getline(in, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
      if (line.empty()) continue;
      if (line == pad_hook::kMsgMenu) {
        pending_menu_ = true;
        VRP_LOG("PadHookBridge: ← MENU");
      } else if (line == pad_hook::kMsgRecenter) {
        pending_recenter_ = true;
        VRP_LOG("PadHookBridge: ← RECENTER");
      } else if (line == pad_hook::kMsgHello) {
        pending_hello_ = true;
        block_sent_ = false;
        VRP_LOG("PadHookBridge: ← HELLO");
      } else {
        VRP_LOG("PadHookBridge: ← unknown '%s'", line.c_str());
      }
    }
  }
}

PadHookBridge::Edges PadHookBridge::pump() {
  Edges edges{};
  std::lock_guard<std::mutex> lock(mu_);
  if (!running_.load(std::memory_order_relaxed) || listen_fd_ < 0) return edges;

  for (;;) {
    const int cfd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (cfd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      break;
    }
    clients_.push_back(cfd);
    block_sent_ = false;
    VRP_LOG("PadHookBridge: guest connected (fd=%d, n=%zu)", cfd, clients_.size());
  }

  for (auto it = clients_.begin(); it != clients_.end();) {
    char probe;
    const ssize_t peek = ::recv(*it, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
    if (peek == 0) {
      VRP_LOG("PadHookBridge: guest disconnected");
      ::close(*it);
      it = clients_.erase(it);
      continue;
    }
    drain_client(*it);
    ++it;
  }

  if (!block_sent_ && !clients_.empty()) {
    broadcast(block_guest_ ? pad_hook::kMsgBlockOn : pad_hook::kMsgBlockOff);
    block_sent_ = true;
  }

  edges.menu_toggle = pending_menu_;
  edges.recenter = pending_recenter_;
  edges.client_hello = pending_hello_;
  pending_menu_ = pending_recenter_ = pending_hello_ = false;
  return edges;
}

}  // namespace vrp
