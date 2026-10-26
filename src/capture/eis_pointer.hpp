#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace vrp {

/**
 * libei sender absolute pointer (fd from KWin EIS or portal RemoteDesktop.ConnectToEIS).
 * X11 XTest cannot move the Wayland seat cursor.
 */
class EisPointer {
 public:
  EisPointer() = default;
  ~EisPointer();

  EisPointer(const EisPointer&) = delete;
  EisPointer& operator=(const EisPointer&) = delete;

  /**
   * Take ownership of eis_fd (closed on failure / stop).
   * mapping_id: optional ScreenCast stream prop to pick a libei region (GNOME).
   */
  bool start_from_fd(int eis_fd, const std::string& mapping_id = {});
  void stop();

  bool ready() const { return ready_.load(std::memory_order_acquire); }
  std::string last_error() const;

  /**
   * Absolute motion. If a mapping_id region was bound, (x,y) are region-local
   * (stream / Virtual MONITOR pixels). Otherwise desktop-global logical coords.
   */
  void warp(double x, double y);
  /** Desktop-global logical pixels (ignore mapping_id region offset). */
  void warp_desktop(double x, double y);
  void click_left();
  /** Positive steps = scroll up (wheel away). */
  void scroll_discrete(int steps);

  /** True when warp() expects region-local coords (mapping_id matched). */
  bool region_local() const { return region_local_.load(std::memory_order_acquire); }
  uint32_t region_x() const { return region_x_; }
  uint32_t region_y() const { return region_y_; }

 private:
  bool ensure_device();
  void pump();
  void set_error(const std::string& e);
  void teardown_unlocked();
  void pick_region_for_device(void* device /* struct ei_device* */);

  mutable std::mutex mu_;
  void* ei_ = nullptr;      // struct ei*
  void* device_ = nullptr;  // struct ei_device* (ref'd)
  int pending_fd_ = -1;     // only if setup not yet taken ownership
  uint32_t emu_seq_ = 1;
  bool emulating_ = false;
  bool resumed_ = false;
  std::atomic<bool> ready_{false};
  std::atomic<bool> region_local_{false};
  std::string mapping_id_;
  uint32_t region_x_ = 0;
  uint32_t region_y_ = 0;
  uint32_t region_w_ = 0;
  uint32_t region_h_ = 0;
  std::string last_error_;
};

}  // namespace vrp
