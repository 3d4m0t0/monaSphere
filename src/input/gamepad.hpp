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
    bool confirm = false;  // A edge
    bool confirm_held = false;  // A level
    bool back = false;     // B edge
    bool back_held = false;  // B level (for long-press)
    bool menu_toggle = false;  // Start/Guide short press
    bool recenter = false;     // Start/Guide long press
    bool seek_back = false;    // LB
    bool seek_forward = false; // RB
    /** True while LB/RB key-repeat is active (after initial delay, until release). */
    bool seek_scrubbing = false;
    /** PlayStation face buttons: confirm is ×, back is ○. */
    bool sony_face = false;
  };

  GamepadInput();
  ~GamepadInput();

  GamepadInput(const GamepadInput&) = delete;
  GamepadInput& operator=(const GamepadInput&) = delete;

  bool available() const { return available_; }
  bool ensure_open();
  void close();
  State poll();

  static std::vector<std::string> list_names();

 private:
  bool available_ = false;
  bool sdl_ready_ = false;
  int device_index_ = -1;
  void* controller_ = nullptr;
  bool prev_a_ = false;
  bool prev_b_ = false;
  bool lb_held_ = false;
  bool rb_held_ = false;
  bool lb_repeating_ = false;
  bool rb_repeating_ = false;
  std::chrono::steady_clock::time_point lb_next_{};
  std::chrono::steady_clock::time_point rb_next_{};
  bool start_down_ = false;
  bool start_long_fired_ = false;
  std::chrono::steady_clock::time_point start_down_at_{};
};

}  // namespace vrp
