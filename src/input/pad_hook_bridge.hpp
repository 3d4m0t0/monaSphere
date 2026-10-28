#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace vrp {

/**
 * Host side of the LD_PRELOAD pad hook: Unix socket server.
 * Guest (libmonasphere_padhook.so) connects and sends MENU / RECENTER;
 * host sends BLOCK 1/0 while the VR menu owns the pad.
 */
class PadHookBridge {
 public:
  struct Edges {
    bool menu_toggle = false;
    bool recenter = false;
    bool client_hello = false;
  };

  PadHookBridge() = default;
  ~PadHookBridge();

  PadHookBridge(const PadHookBridge&) = delete;
  PadHookBridge& operator=(const PadHookBridge&) = delete;

  /** Listen on a fresh socket path under /tmp. */
  bool start();
  void stop();
  bool running() const { return running_.load(std::memory_order_acquire); }
  std::string socket_path() const;

  /** While true, guest hook zeros gamepad buttons/axes. */
  void set_block_guest(bool block);

  /** Non-blocking: accept, read client lines, apply pending block. Call from XR thread. */
  Edges pump();

 private:
  void close_all();
  void broadcast(const char* line);
  void drain_client(int fd);

  mutable std::mutex mu_;
  std::atomic<bool> running_{false};
  int listen_fd_ = -1;
  std::string sock_path_;
  std::vector<int> clients_;
  bool block_guest_ = false;
  bool block_sent_ = false;
  bool pending_menu_ = false;
  bool pending_recenter_ = false;
  bool pending_hello_ = false;
};

}  // namespace vrp
