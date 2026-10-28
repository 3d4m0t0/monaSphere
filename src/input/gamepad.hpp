#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace vrp {

/** Desktop / USB / Bluetooth gamepad via SDL2 GameController. */
class GamepadInput {
 public:
  struct State {
    bool present = false;
    std::string name;
    /** "USB", "Bluetooth", "virtual", or empty when the bus is unknown. */
    std::string connection;
    float stick_x = 0.f;
    float stick_y = 0.f;  // +up
    /** Right stick. +x right, +y up. Turns the view; not saved. */
    float stick_rx = 0.f;
    float stick_ry = 0.f;
    /** Cross key buttons. Kept off the stick axis so a full tilt stays the fast analog glide. */
    bool nav_up = false;
    bool nav_down = false;
    bool nav_left = false;
    bool nav_right = false;
    bool confirm = false;  // A edge
    bool confirm_held = false;  // A level
    bool back = false;     // B edge
    bool back_held = false;  // B level (for long-press)
    bool menu_toggle = false;  // PS / Guide long press (Options long also)
    /** Y / △ edge. Flat scale cycle when menus are closed. */
    bool face_y = false;
    bool recenter = false;     // L3+R3 chord
    /** Right stick click (R3) alone. Clears stick look offset. */
    bool view_reset = false;
    bool seek_back = false;    // LB edge (repeat)
    bool seek_forward = false; // RB edge (repeat)
    bool seek_back_held = false;
    bool seek_forward_held = false;
    /** Left / right trigger (L2 / R2). */
    bool l2_held = false;
    bool r2_held = false;
    /** True while LB/RB key-repeat is active (after initial delay, until release). */
    bool seek_scrubbing = false;
    /** PlayStation face buttons: confirm is ×, back is ○. */
    bool sony_face = false;
  };

  struct PassThroughEdges {
    bool menu_toggle = false;  // Options long
    bool recenter = false;     // PS / Guide long
  };

  GamepadInput();
  ~GamepadInput();

  GamepadInput(const GamepadInput&) = delete;
  GamepadInput& operator=(const GamepadInput&) = delete;

  bool available() const { return available_; }
  bool ensure_open();
  void close();
  State poll();

  /**
   * Release SDL so the desktop app owns the pad. Watch chords only (do not steal
   * single Options/PS — those stay with RetroArch etc.):
   *   PS+R1 hold → menu,  PS+L1 hold → recenter.
   */
  void begin_pass_through_watch();
  void end_pass_through_watch();
  bool pass_through_watching() const { return pass_active_; }
  /** Edges for PS+R1 menu and PS+L1 recenter. */
  PassThroughEdges poll_pass_through();

  static std::vector<std::string> list_names();

 private:
  bool available_ = false;
  bool sdl_ready_ = false;
  int device_index_ = -1;
  void* controller_ = nullptr;
  std::string last_device_path_;
  /** Armed while desktop-app pass-through is active (even if open failed once). */
  bool pass_active_ = false;
  std::vector<int> pass_fds_;
  int pass_hidraw_fd_ = -1;
  /** True when pass_hidraw_fd_ is DualSense / Edge. */
  bool pass_hidraw_dualsense_ = false;
  /** /dev/input/jsN for DualSense — survives EVIOCGRAB on event*. */
  int pass_js_fd_ = -1;
  bool pass_ps_down_ = false;
  bool pass_l1_down_ = false;
  bool pass_r1_down_ = false;
  bool pass_menu_chord_fired_ = false;
  bool pass_recenter_chord_fired_ = false;
  std::chrono::steady_clock::time_point pass_menu_chord_at_{};
  std::chrono::steady_clock::time_point pass_recenter_chord_at_{};
  bool prev_a_ = false;
  bool prev_b_ = false;
  bool prev_y_ = false;
  bool prev_r3_ = false;
  bool lb_held_ = false;
  bool rb_held_ = false;
  bool lb_repeating_ = false;
  bool rb_repeating_ = false;
  std::chrono::steady_clock::time_point lb_next_{};
  std::chrono::steady_clock::time_point rb_next_{};
  bool options_down_ = false;
  bool options_long_fired_ = false;
  std::chrono::steady_clock::time_point options_down_at_{};
  bool ps_down_ = false;
  bool ps_long_fired_ = false;
  std::chrono::steady_clock::time_point ps_down_at_{};
  bool l3_down_ = false;
  bool r3_down_ = false;
  bool l3r3_recenter_fired_ = false;
};

}  // namespace vrp
