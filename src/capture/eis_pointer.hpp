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
  /** Same as click_left but ignores buttons_enabled (Meta first-paint nudge). */
  void click_left_forced();
  /** Positive steps = scroll up (wheel away). */
  void scroll_discrete(int steps);
  /**
   * Browser history Back (Alt+Left) via EIS keyboard. Needed for Wayland Chrome
   * on Meta-* — XTest Alt+Left never reaches a wl surface.
   */
  bool key_alt_left();
  /**
   * Escape — dismiss GNOME Shell Activities/overview on the focused monitor.
   * Live switch sometimes leaves Meta in a Super-key scaled-desktop state.
   */
  bool key_escape();
  /**
   * When false, motion still works but button/scroll are no-ops.
   * GNOME: keep false until portal grant dialogs are off Meta-* — pad click
   * otherwise activates the remote-control dialog still sitting in the stream.
   */
  void set_buttons_enabled(bool on) { buttons_enabled_.store(on, std::memory_order_release); }
  bool buttons_enabled() const { return buttons_enabled_.load(std::memory_order_acquire); }

  /** True when warp() expects region-local coords (mapping_id matched). */
  bool region_local() const { return region_local_.load(std::memory_order_acquire); }
  uint32_t region_x() const { return region_x_; }
  uint32_t region_y() const { return region_y_; }
  /** libei region size in compositor logical pixels (physical / output scale). */
  uint32_t region_w() const { return region_w_; }
  uint32_t region_h() const { return region_h_; }

  /**
   * Re-pump libei and re-bind the absolute-pointer region.
   * GNOME: ConnectToEIS runs before Meta-* exists, so the first pick is often the
   * primary monitor — warps miss the Embedded cursor in the Meta stream until
   * Meta appears. Call after RecordVirtual Meta is listed (post PW negotiate).
   * @return true when a non-origin (or sole) region is bound and device is resumed.
   */
  bool repick_regions();

 private:
  bool ensure_device();
  bool ensure_keyboard();
  void pump();
  void set_error(const std::string& e);
  void teardown_unlocked();
  void pick_region_for_device(void* device /* struct ei_device* */);

  mutable std::mutex mu_;
  void* ei_ = nullptr;      // struct ei*
  void* device_ = nullptr;  // struct ei_device* (ref'd) — absolute pointer
  void* keyboard_ = nullptr;  // struct ei_device* (ref'd) — may equal device_
  int pending_fd_ = -1;     // only if setup not yet taken ownership
  uint32_t emu_seq_ = 1;
  bool emulating_ = false;
  bool kbd_emulating_ = false;
  bool resumed_ = false;
  bool kbd_resumed_ = false;
  std::atomic<bool> ready_{false};
  std::atomic<bool> region_local_{false};
  std::atomic<bool> buttons_enabled_{true};
  std::string mapping_id_;
  uint32_t region_x_ = 0;
  uint32_t region_y_ = 0;
  uint32_t region_w_ = 0;
  uint32_t region_h_ = 0;
  std::string last_error_;
};

}  // namespace vrp
