#pragma once

#include <cstdint>
#include <mutex>
#include <string>

namespace vrp {

class EisPointer;

/** X11 helpers for the live Chromium window (WM_CLASS monaSphereYT). */
class ChromiumYtX11 {
 public:
  ChromiumYtX11() = default;
  ~ChromiumYtX11();

  ChromiumYtX11(const ChromiumYtX11&) = delete;
  ChromiumYtX11& operator=(const ChromiumYtX11&) = delete;

  /**
   * Find WM_CLASS monaSphereYT; size / place window.
   * If hide_host, park far off-screen (WINDOW capture). For MONITOR capture,
   * pass hide_host=false and set_place() to the target output origin.
   * If move=false, only bind the window + skip-taskbar (KWin already placed it).
   */
  bool prepare_window(int width, int height, bool hide_host = true, bool move = true);
  void set_place(int x, int y) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    place_x_ = x;
    place_y_ = y;
    have_place_ = true;
  }
  void clear_place() {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    have_place_ = false;
  }
  /**
   * Fallback: treat (x,y,w,h) as both cursor-space and XTest root space.
   * Prefer bind_output_by_name() on Plasma/XWayland (kscreen logical ≠ X11).
   */
  void set_screen_rect(int x, int y, int w, int h);
  /**
   * Cursor clamp = configured virtual-display resolution (cfg_w×cfg_h).
   * XTest absolute motion uses the Xrandr geometry of the named output
   * (physical XWayland coords — required when the primary has scale≠1).
   */
  bool bind_output_by_name(const std::string& output_name_substr, int cfg_w, int cfg_h);

  bool have_window() const {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    return window_ != 0;
  }
  /** True if we can inject pointer (KWin EIS, X11 window, or MONITOR rect). */
  bool can_inject() const;
  void clear();

  /**
   * Prefer EIS absolute pointer for Wayland seat motion (Embedded cursor).
   * X11 XTest cannot move the compositor seat pointer.
   * Origin is desktop logical top-left of the target MONITOR (ignored when
   * EisPointer uses region-local / mapping_id coords).
   */
  void set_eis_inject(EisPointer* eis) { eis_ = eis; }
  void set_eis_origin(int logical_x, int logical_y) {
    eis_ox_ = logical_x;
    eis_oy_ = logical_y;
  }

  /** Configured virtual-display size (cursor clamp). */
  int width() const { return width_; }
  int height() const { return height_; }
  /** Xrandr/XTest output size (may differ from width_/height_ under scale≠1). */
  int xtest_w() const { return xtest_w_ > 0 ? xtest_w_ : width_; }
  int xtest_h() const { return xtest_h_ > 0 ? xtest_h_ : height_; }
  int place_x() const { return place_x_; }
  int place_y() const { return place_y_; }

  /** Client-area cursor in configured virtual-display pixels (origin top-left). */
  void set_cursor(float x, float y);
  void move_cursor(float dx, float dy);
  float cursor_x() const { return cursor_x_; }
  float cursor_y() const { return cursor_y_; }
  /** Normalized 0..1 for drawing an HMD overlay cursor. */
  float cursor_u() const {
    return width_ > 1 ? cursor_x_ / static_cast<float>(width_ - 1) : 0.5f;
  }
  float cursor_v() const {
    return height_ > 1 ? cursor_y_ / static_cast<float>(height_ - 1) : 0.5f;
  }

  /** Left click at current cursor (XTest + XSendEvent). */
  bool click();

  /**
   * Mouse wheel at the current cursor. dy>0 scrolls up (content moves down),
   * dy<0 scrolls down. |dy| is roughly "notches" (can be fractional; accumulates).
   */
  bool scroll(float dy);

  /** Browser Back (Alt+Left). */
  bool key_back();

  /** Ask the app window to close (WM_DELETE_WINDOW) — safe with a shared Chrome. */
  bool close_window();

  /** Re-read XGetGeometry (fullscreen / resize). */
  bool refresh_geometry();

  /** Re-bind Chrome window if the previous xid died (shared profile / Wayland). */
  bool ensure_inject_target();

  /** Root-window origin of the client (for verifying KWin moved it). */
  bool root_origin(int* root_x, int* root_y) const;

  std::string last_error() const { return last_error_; }

 private:
  bool ensure_display();
  bool find_window();
  bool apply_chrome(int width, int height, bool hide_host, bool move);
  bool focus_window();
  bool warp_cursor();
  bool send_motion_event();
  bool root_coords(int* root_x, int* root_y) const;
  bool window_alive_unlocked() const;
  bool find_xrandr_output_unlocked(const std::string& needle, int* x, int* y, int* w,
                                   int* h) const;
  void set_error(const std::string& e);

  /** Qt place-poll and XR pointer inject share one Display*. */
  mutable std::recursive_mutex mu_;
  void* display_ = nullptr;  // Display*
  unsigned long window_ = 0;  // Window
  int width_ = 1280;   // cursor clamp = configured virtual display
  int height_ = 720;
  int xtest_w_ = 0;    // Xrandr output size for XTest (0 → use width_)
  int xtest_h_ = 0;
  int client_w_ = 0;   // Chrome X11 client size (may differ from MONITOR)
  int client_h_ = 0;
  int place_x_ = 80;   // XTest root origin of the Virtual output
  int place_y_ = 80;
  bool have_place_ = false;
  bool screen_input_ = false;
  float cursor_x_ = 640.f;
  float cursor_y_ = 360.f;
  std::string last_error_;
  EisPointer* eis_ = nullptr;  // not owned
  int eis_ox_ = 0;             // MONITOR logical origin (when not region-local)
  int eis_oy_ = 0;

  /** Map MONITOR-space cursor into Chrome client pixels for XSendEvent. */
  void client_cursor(int* cx, int* cy) const;
};

// Software RGBA cursor overlay intentionally omitted — live uses ScreenCast
// cursor_mode=Embedded (DE/compositor cursor in the PipeWire stream).

}  // namespace vrp
