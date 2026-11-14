#include "capture/eis_pointer.hpp"

#include "common.hpp"

#include <libei.h>
#include <linux/input-event-codes.h>

#include <unistd.h>

#include <chrono>
#include <cstring>
#include <thread>

namespace vrp {

EisPointer::~EisPointer() { stop(); }

std::string EisPointer::last_error() const {
  std::lock_guard<std::mutex> lock(mu_);
  return last_error_;
}

void EisPointer::set_error(const std::string& e) {
  last_error_ = e;
  VRP_LOG("eis-pointer: %s", e.c_str());
}

void EisPointer::teardown_unlocked() {
  ready_.store(false, std::memory_order_release);
  region_local_.store(false, std::memory_order_release);
  if (device_ && emulating_) {
    ei_device_stop_emulating(static_cast<struct ei_device*>(device_));
    emulating_ = false;
    if (keyboard_ == device_) kbd_emulating_ = false;
  }
  if (keyboard_ && kbd_emulating_) {
    ei_device_stop_emulating(static_cast<struct ei_device*>(keyboard_));
    kbd_emulating_ = false;
  }
  if (keyboard_) {
    ei_device_unref(static_cast<struct ei_device*>(keyboard_));
    keyboard_ = nullptr;
  }
  kbd_resumed_ = false;
  if (device_) {
    ei_device_unref(static_cast<struct ei_device*>(device_));
    device_ = nullptr;
  }
  resumed_ = false;
  region_x_ = region_y_ = region_w_ = region_h_ = 0;
  if (ei_) {
    ei_unref(static_cast<struct ei*>(ei_));
    ei_ = nullptr;
  }
  if (pending_fd_ >= 0) {
    ::close(pending_fd_);
    pending_fd_ = -1;
  }
}

void EisPointer::pick_region_for_device(void* device) {
  auto* dev = static_cast<struct ei_device*>(device);
  region_local_.store(false, std::memory_order_release);
  region_x_ = region_y_ = region_w_ = region_h_ = 0;
  if (!dev) return;

  struct ei_region* matched = nullptr;
  struct ei_region* rightmost = nullptr;
  struct ei_region* last = nullptr;
  size_t nreg = 0;
  uint32_t best_x = 0;
  for (size_t i = 0;; ++i) {
    struct ei_region* reg = ei_device_get_region(dev, i);
    if (!reg) break;
    ++nreg;
    last = reg;
    const uint32_t rx = ei_region_get_x(reg);
    const uint32_t ry = ei_region_get_y(reg);
    const char* mid = ei_region_get_mapping_id(reg);
    VRP_LOG("eis-pointer: region %zu %ux%u+%u+%u mapping_id=%s", i, ei_region_get_width(reg),
            ei_region_get_height(reg), rx, ry, mid && *mid ? mid : "(none)");
    // RecordVirtual Meta is usually the rightmost / non-origin output.
    if (rx > 0 || ry > 0) {
      if (!rightmost || rx >= best_x) {
        rightmost = reg;
        best_x = rx;
      }
    }
    if (matched || mapping_id_.empty() || !mid || !*mid) continue;
    const std::string m(mid);
    if (m == mapping_id_ || m.find(mapping_id_) != std::string::npos ||
        mapping_id_.find(m) != std::string::npos) {
      matched = reg;
    }
  }

  if (!matched && !mapping_id_.empty()) {
    // Single region → that MONITOR.
    if (nreg == 1) matched = last;
  }
  // Mutter RD often leaves mapping_id empty — pick Meta (rightmost non-origin).
  if (!matched && rightmost) {
    matched = rightmost;
    VRP_LOG("eis-pointer: no mapping_id — using rightmost region as Meta");
  }
  if (!matched && last) matched = last;

  if (matched) {
    region_x_ = ei_region_get_x(matched);
    region_y_ = ei_region_get_y(matched);
    region_w_ = ei_region_get_width(matched);
    region_h_ = ei_region_get_height(matched);
    // Caller passes MONITOR-local; warp() adds region origin → desktop-global.
    if (region_w_ > 0 && region_h_ > 0) {
      region_local_.store(true, std::memory_order_release);
      VRP_LOG("eis-pointer: MONITOR-local -> global via region %ux%u @%u,%u", region_w_,
              region_h_, region_x_, region_y_);
    }
  }
}

bool EisPointer::repick_regions() {
  std::lock_guard<std::mutex> lock(mu_);
  if (!ei_ || !ready_.load(std::memory_order_acquire)) return false;
  for (int i = 0; i < 25; ++i) {
    pump();
    if (device_) {
      pick_region_for_device(device_);
      // pick_region prefers rightmost non-origin (Meta). Accept once sized + resumed.
      if (resumed_ && region_w_ > 0 && region_h_ > 0) {
        VRP_LOG("eis-pointer: repick -> %ux%u @%u,%u local=%d", region_w_, region_h_, region_x_,
                region_y_, region_local_.load() ? 1 : 0);
        return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
  }
  VRP_LOG("eis-pointer: repick failed (region %ux%u @%u,%u resumed=%d)", region_w_, region_h_,
          region_x_, region_y_, resumed_ ? 1 : 0);
  return false;
}

bool EisPointer::start_from_fd(int eis_fd, const std::string& mapping_id) {
  std::lock_guard<std::mutex> lock(mu_);
  if (eis_fd < 0) {
    set_error("start_from_fd: invalid fd");
    return false;
  }
  teardown_unlocked();
  mapping_id_ = mapping_id;
  pending_fd_ = eis_fd;

  ei_ = ei_new_sender(nullptr);
  if (!ei_) {
    set_error("ei_new_sender failed");
    teardown_unlocked();
    return false;
  }
  if (ei_setup_backend_fd(static_cast<struct ei*>(ei_), pending_fd_) != 0) {
    set_error("ei_setup_backend_fd failed");
    teardown_unlocked();
    return false;
  }
  pending_fd_ = -1;

  for (int i = 0; i < 50 && !resumed_; ++i) {
    pump();
    if (!resumed_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  if (!device_ || !resumed_) {
    set_error("EIS: no absolute pointer device (timed out)");
    teardown_unlocked();
    return false;
  }
  ready_.store(true, std::memory_order_release);
  VRP_LOG("eis-pointer: ready (mapping_id=%s region_local=%s)",
          mapping_id_.empty() ? "(none)" : mapping_id_.c_str(),
          region_local_.load() ? "yes" : "no");
  return true;
}

void EisPointer::stop() {
  std::lock_guard<std::mutex> lock(mu_);
  teardown_unlocked();
}

void EisPointer::pump() {
  if (!ei_) return;
  auto* ei = static_cast<struct ei*>(ei_);
  ei_dispatch(ei);
  while (struct ei_event* ev = ei_get_event(ei)) {
    const enum ei_event_type t = ei_event_get_type(ev);
    if (t == EI_EVENT_DISCONNECT) {
      VRP_LOG("eis-pointer: disconnected by EIS");
      ready_.store(false, std::memory_order_release);
      if (keyboard_ && keyboard_ != device_) {
        ei_device_unref(static_cast<struct ei_device*>(keyboard_));
      }
      keyboard_ = nullptr;
      kbd_resumed_ = false;
      kbd_emulating_ = false;
      if (device_) {
        ei_device_unref(static_cast<struct ei_device*>(device_));
        device_ = nullptr;
      }
      resumed_ = false;
      emulating_ = false;
    } else if (t == EI_EVENT_SEAT_ADDED) {
      struct ei_seat* seat = ei_event_get_seat(ev);
      ei_seat_bind_capabilities(seat, EI_DEVICE_CAP_POINTER, EI_DEVICE_CAP_POINTER_ABSOLUTE,
                                EI_DEVICE_CAP_BUTTON, EI_DEVICE_CAP_SCROLL, EI_DEVICE_CAP_KEYBOARD,
                                nullptr);
    } else if (t == EI_EVENT_DEVICE_ADDED) {
      struct ei_device* dev = ei_event_get_device(ev);
      const bool abs = ei_device_has_capability(dev, EI_DEVICE_CAP_POINTER_ABSOLUTE);
      const bool kbd = ei_device_has_capability(dev, EI_DEVICE_CAP_KEYBOARD);
      if (abs) {
        if (device_) {
          ei_device_unref(static_cast<struct ei_device*>(device_));
          device_ = nullptr;
        }
        device_ = ei_device_ref(dev);
        resumed_ = false;
        emulating_ = false;
        pick_region_for_device(dev);
      }
      if (kbd) {
        if (keyboard_ && keyboard_ != device_) {
          ei_device_unref(static_cast<struct ei_device*>(keyboard_));
        }
        // Own a dedicated ref even when the same struct also has pointer caps.
        keyboard_ = ei_device_ref(dev);
        kbd_resumed_ = false;
        kbd_emulating_ = false;
      }
    } else if (t == EI_EVENT_DEVICE_RESUMED) {
      struct ei_device* dev = ei_event_get_device(ev);
      if (!device_ && ei_device_has_capability(dev, EI_DEVICE_CAP_POINTER_ABSOLUTE)) {
        device_ = ei_device_ref(dev);
        pick_region_for_device(dev);
      }
      if (!keyboard_ && ei_device_has_capability(dev, EI_DEVICE_CAP_KEYBOARD)) {
        keyboard_ = ei_device_ref(dev);
      }
      if (device_ && dev == device_) resumed_ = true;
      if (keyboard_ && dev == keyboard_) kbd_resumed_ = true;
    } else if (t == EI_EVENT_DEVICE_PAUSED) {
      struct ei_device* dev = ei_event_get_device(ev);
      if (dev == device_) {
        resumed_ = false;
        emulating_ = false;
      }
      if (dev == keyboard_) {
        kbd_resumed_ = false;
        kbd_emulating_ = false;
      }
    } else if (t == EI_EVENT_DEVICE_REMOVED) {
      struct ei_device* dev = ei_event_get_device(ev);
      if (dev == keyboard_) {
        ei_device_unref(static_cast<struct ei_device*>(keyboard_));
        keyboard_ = nullptr;
        kbd_resumed_ = false;
        kbd_emulating_ = false;
      }
      if (dev == device_) {
        ei_device_unref(static_cast<struct ei_device*>(device_));
        device_ = nullptr;
        resumed_ = false;
        emulating_ = false;
      }
    }
    ei_event_unref(ev);
  }
}

bool EisPointer::ensure_device() {
  pump();
  if (!device_ || !resumed_) return false;
  if (!emulating_) {
    ei_device_start_emulating(static_cast<struct ei_device*>(device_), emu_seq_++);
    emulating_ = true;
  }
  return true;
}

bool EisPointer::ensure_keyboard() {
  pump();
  if (!keyboard_ || !kbd_resumed_) return false;
  if (!kbd_emulating_) {
    // Pointer+keyboard on one device: start_emulating once.
    if (keyboard_ == device_ && emulating_) {
      kbd_emulating_ = true;
    } else {
      ei_device_start_emulating(static_cast<struct ei_device*>(keyboard_), emu_seq_++);
      kbd_emulating_ = true;
      if (keyboard_ == device_) emulating_ = true;
    }
  }
  return true;
}

bool EisPointer::key_alt_left() {
  std::lock_guard<std::mutex> lock(mu_);
  if (!ei_ || !ensure_keyboard()) return false;
  auto* ei = static_cast<struct ei*>(ei_);
  auto* kbd = static_cast<struct ei_device*>(keyboard_);
  // Evdev scancodes (keymap-independent): Alt+Left = browser Back.
  ei_device_keyboard_key(kbd, KEY_LEFTALT, true);
  ei_device_keyboard_key(kbd, KEY_LEFT, true);
  ei_device_frame(kbd, ei_now(ei));
  ei_device_keyboard_key(kbd, KEY_LEFT, false);
  ei_device_keyboard_key(kbd, KEY_LEFTALT, false);
  ei_device_frame(kbd, ei_now(ei));
  pump();
  return true;
}

bool EisPointer::key_escape() {
  std::lock_guard<std::mutex> lock(mu_);
  if (!ei_ || !ensure_keyboard()) return false;
  auto* ei = static_cast<struct ei*>(ei_);
  auto* kbd = static_cast<struct ei_device*>(keyboard_);
  ei_device_keyboard_key(kbd, KEY_ESC, true);
  ei_device_frame(kbd, ei_now(ei));
  ei_device_keyboard_key(kbd, KEY_ESC, false);
  ei_device_frame(kbd, ei_now(ei));
  pump();
  return true;
}

void EisPointer::warp(double x, double y) {
  double ax = x;
  double ay = y;
  if (region_local_.load(std::memory_order_acquire) && region_w_ > 0 && region_h_ > 0) {
    // Caller: MONITOR-local pixels. Protocol: desktop-global within region.
    if (ax < 0) ax = 0;
    if (ay < 0) ay = 0;
    if (ax > static_cast<double>(region_w_ - 1)) ax = static_cast<double>(region_w_ - 1);
    if (ay > static_cast<double>(region_h_ - 1)) ay = static_cast<double>(region_h_ - 1);
    ax += static_cast<double>(region_x_);
    ay += static_cast<double>(region_y_);
  }
  warp_desktop(ax, ay);
}

void EisPointer::warp_desktop(double x, double y) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!ei_ || !ensure_device()) return;
  auto* ei = static_cast<struct ei*>(ei_);
  auto* dev = static_cast<struct ei_device*>(device_);
  ei_device_pointer_motion_absolute(dev, x, y);
  ei_device_frame(dev, ei_now(ei));
  pump();
}

void EisPointer::click_left() {
  if (!buttons_enabled_.load(std::memory_order_acquire)) return;
  click_left_forced();
}

void EisPointer::click_left_forced() {
  std::lock_guard<std::mutex> lock(mu_);
  if (!ei_ || !ensure_device()) return;
  auto* ei = static_cast<struct ei*>(ei_);
  auto* dev = static_cast<struct ei_device*>(device_);
  ei_device_button_button(dev, 0x110 /* BTN_LEFT */, true);
  ei_device_frame(dev, ei_now(ei));
  ei_device_button_button(dev, 0x110, false);
  ei_device_frame(dev, ei_now(ei));
  pump();
}

void EisPointer::scroll_discrete(int steps) {
  if (steps == 0) return;
  if (!buttons_enabled_.load(std::memory_order_acquire)) return;
  std::lock_guard<std::mutex> lock(mu_);
  if (!ei_ || !ensure_device()) return;
  auto* ei = static_cast<struct ei*>(ei_);
  auto* dev = static_cast<struct ei_device*>(device_);
  // One wheel click = 120 (libei); |steps| is notch count from the stick.
  const int32_t discrete = static_cast<int32_t>(-steps) * 120;
  ei_device_scroll_discrete(dev, 0, discrete);
  ei_device_frame(dev, ei_now(ei));
  pump();
}

}  // namespace vrp
