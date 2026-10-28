#pragma once

#include "input/gamepad.hpp"

#include <linux/input.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>

namespace vrp {

/**
 * Exclusive DualSense (etc.) grab + uinput mirror for [アプリ].
 * Forwards the pad to a virtual device the guest app opens, while:
 *   - PS (BTN_MODE) long-press → menu (not forwarded)
 *   - short PS → press on release, release next pump (guest must see a real pulse)
 *   - L3+R3 → recenter (clicks still forwarded)
 */
class PadFilter {
 public:
  struct Edges {
    bool menu_toggle = false;
    bool recenter = false;
  };

  PadFilter() = default;
  ~PadFilter();

  PadFilter(const PadFilter&) = delete;
  PadFilter& operator=(const PadFilter&) = delete;

  /** Grab physical pad and create uinput. Call before guest app launch. */
  bool start();
  void stop();
  bool running() const { return running_.load(std::memory_order_acquire); }

  /** /dev/input/eventN for the uinput device (empty if unresolved). */
  std::string uinput_event_path() const;

  /**
   * Read physical → filter → optional uinput. Call once per XR frame while desktop app is live.
   * @param guest_forward when false (VR menu open), update snapshot only — no uinput
   *   (menu owns the pad). PS long never reaches the guest; short PS is injected on release
   *   only when guest_forward is true.
   */
  Edges pump(bool guest_forward = true);

  /** Last decoded pad state (from physical). Valid after pump(). */
  GamepadInput::State snapshot() const;

 private:
  bool open_physical();
  bool create_uinput();
  void resolve_uinput_event_path();
  void close_all();
  void stamp(struct input_event* ev);
  void write_uinput(const struct input_event& ev);
  void flush_uinput_sync();
  void inject_key(int code, int value);
  void apply_key_to_snap(int code, int value);

  mutable std::mutex mu_;
  std::atomic<bool> running_{false};
  int phys_fd_ = -1;
  int uinput_fd_ = -1;
  std::string phys_path_;
  std::string uinput_event_path_;
  struct input_absinfo absinfo_[ABS_CNT]{};

  bool ps_down_ = false;
  bool ps_long_fired_ = false;       // this hold crossed long threshold
  /**
   * Set when long fires; cleared after PS up + cooldown. While set, never write uinput
   * (blocks release-inject and stick leak in the frame before menu.visible flips).
   */
  bool ps_menu_gesture_ = false;
  bool ps_release_pending_ = false;  // inject BTN_MODE=0 on next pump (short only)
  /** After a long gesture, ignore BTN_MODE until this time (release bounce → false short). */
  std::chrono::steady_clock::time_point ps_ignore_until_{};
  std::chrono::steady_clock::time_point ps_down_at_{};
  bool last_guest_forward_ = true;
  int dbg_forward_count_ = 0;

  bool l3_down_ = false;
  bool r3_down_ = false;
  bool recenter_chord_fired_ = false;

  GamepadInput::State snap_{};
  bool prev_a_ = false;
  bool prev_b_ = false;
  bool prev_y_ = false;
};

}  // namespace vrp
