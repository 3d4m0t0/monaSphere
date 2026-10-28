#include "input/gamepad.hpp"

#include "common.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>

#if defined(__linux__)
#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/hidraw.h>
#include <linux/input.h>
#endif

#if defined(VRP_HAS_SDL2)
#include <SDL.h>
#endif

namespace vrp {

namespace {
constexpr auto kStartLongPress = std::chrono::milliseconds(700);

#if defined(__linux__)
bool is_ps_code(int code) {
  // DualSense PS (BTN_MODE) / Guide.
  return code == BTN_MODE || code == KEY_HOME;
}

int open_evdev_no_grab(const std::string& path) {
  if (path.empty()) return -1;
  const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) return -1;
  // Intentionally no EVIOCGRAB — RetroArch (or another app) keeps the pad.
  return fd;
}

bool evdev_has_key(int fd, int code) {
  unsigned long key_bits[(KEY_MAX + 1 + 63) / 64] = {};
  if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0) return false;
  return (key_bits[code / 64] >> (code % 64)) & 1UL;
}

/** Open every gamepad-like event node that can report Options and/or PS. */
std::vector<int> open_all_joystick_evdevs(const std::string& prefer_path) {
  std::vector<int> out;
  auto try_add = [&](const std::string& path) {
    if (path.empty() || path.find("/js") != std::string::npos) return;
    const int fd = open_evdev_no_grab(path);
    if (fd < 0) return;
    const bool joy = evdev_has_key(fd, BTN_GAMEPAD) || evdev_has_key(fd, BTN_SOUTH) ||
                     evdev_has_key(fd, BTN_A) || evdev_has_key(fd, BTN_START) ||
                     evdev_has_key(fd, BTN_MODE);
    const bool useful = evdev_has_key(fd, BTN_START) || evdev_has_key(fd, BTN_MODE) ||
                        evdev_has_key(fd, KEY_MENU) || evdev_has_key(fd, KEY_HOME);
    if (!joy || !useful) {
      ::close(fd);
      return;
    }
    out.push_back(fd);
  };

  if (!prefer_path.empty()) try_add(prefer_path);

  DIR* dir = ::opendir("/dev/input");
  if (dir) {
    while (dirent* ent = ::readdir(dir)) {
      if (std::strncmp(ent->d_name, "event", 5) != 0) continue;
      const std::string path = std::string("/dev/input/") + ent->d_name;
      if (!prefer_path.empty() && path == prefer_path) continue;
      try_add(path);
    }
    ::closedir(dir);
  }
  return out;
}

bool sony_hid_pid(unsigned short pid) {
  switch (pid) {
    case 0x05c4:  // DS4
    case 0x09cc:  // DS4 Slim
    case 0x0ba0:  // DS4 USB dongle
    case 0x0ce6:  // DualSense
    case 0x0df2:  // DualSense Edge
      return true;
    default:
      return false;
  }
}

int open_sony_hidraw(bool* out_dualsense) {
  if (out_dualsense) *out_dualsense = false;
  DIR* dir = ::opendir("/sys/class/hidraw");
  if (!dir) return -1;
  int best = -1;
  bool best_ds = false;
  while (dirent* ent = ::readdir(dir)) {
    if (std::strncmp(ent->d_name, "hidraw", 6) != 0) continue;
    const std::string uevent =
        std::string("/sys/class/hidraw/") + ent->d_name + "/device/uevent";
    std::ifstream in(uevent);
    if (!in) continue;
    unsigned vid = 0, pid = 0;
    std::string line;
    while (std::getline(in, line)) {
      if (line.rfind("HID_ID=", 0) != 0) continue;
      // HID_ID=0003:0000054C:00000CE6
      unsigned b = 0, v = 0, p = 0;
      if (std::sscanf(line.c_str(), "HID_ID=%x:%x:%x", &b, &v, &p) == 3) {
        vid = v;
        pid = p;
      }
      (void)b;
    }
    if (vid != 0x054c || !sony_hid_pid(static_cast<unsigned short>(pid))) continue;
    const std::string path = std::string("/dev/") + ent->d_name;
    const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) continue;
    if (best >= 0) ::close(best);
    best = fd;
    best_ds = (pid == 0x0ce6 || pid == 0x0df2);
  }
  ::closedir(dir);
  if (out_dualsense) *out_dualsense = best_ds;
  return best;
}

struct SonyPadLevels {
  bool ps = false;
  bool l1 = false;
  bool r1 = false;
};

/**
 * Parse DualSense / DS4 hidraw → PS / L1 / R1 (hid-playstation.c layout).
 * DualSense USB 0x01: payload @ buf[1]; BT 0x31: payload @ buf[2].
 * buttons[1] BIT(0)=L1 BIT(1)=R1; buttons[2] BIT(0)=PS.
 */
bool parse_sony_hid_levels(const uint8_t* buf, size_t n, bool dualsense, SonyPadLevels* out) {
  if (!buf || !out || n < 7) return false;
  const uint8_t id = buf[0];
  if (dualsense) {
    size_t base = 0;
    if (id == 0x01)
      base = 1;
    else if (id == 0x31)
      base = 2;
    else
      return false;
    if (n < base + 10) return false;
    const uint8_t b1 = buf[base + 8];
    const uint8_t b2 = buf[base + 9];
    out->l1 = (b1 & 0x01) != 0;
    out->r1 = (b1 & 0x02) != 0;
    out->ps = (b2 & 0x01) != 0;
    return true;
  }
  if (id == 0x01 && n >= 9) {
    out->l1 = (buf[6] & 0x01) != 0;
    out->r1 = (buf[6] & 0x02) != 0;
    out->ps = (buf[7] & 0x01) != 0;
    return true;
  }
  if (id == 0x11 && n >= 11) {
    out->l1 = (buf[8] & 0x01) != 0;
    out->r1 = (buf[8] & 0x02) != 0;
    out->ps = (buf[9] & 0x01) != 0;
    return true;
  }
  return false;
}

/** DualSense joydev order (BTN_GAMEPAD sequence → js0). */
constexpr int kJsL1Btn = 4;   // BTN_TL
constexpr int kJsR1Btn = 5;   // BTN_TR
constexpr int kJsPsBtn = 10;  // BTN_MODE
constexpr auto kChordHold = std::chrono::milliseconds(500);

int open_joystick_js_for_dualsense() {
  // Prefer js node paired with DualSense gamepad (Handlers=event10 js0).
  std::ifstream in("/proc/bus/input/devices");
  if (!in) return -1;
  std::string line, name, handlers;
  auto flush = [&]() {
    name.clear();
    handlers.clear();
  };
  while (std::getline(in, line)) {
    if (line.rfind("N: Name=", 0) == 0) {
      flush();
      name = line.substr(8);
      if (!name.empty() && name.front() == '"') name.erase(0, 1);
      if (!name.empty() && name.back() == '"') name.pop_back();
    } else if (line.rfind("H: Handlers=", 0) == 0) {
      handlers = line.substr(12);
    } else if (line.empty()) {
      const bool ds = name.find("DualSense") != std::string::npos &&
                      name.find("Motion") == std::string::npos &&
                      name.find("Touchpad") == std::string::npos &&
                      name.find("Headset") == std::string::npos;
      if (ds) {
        // Pick jsN from handlers.
        const auto pos = handlers.find("js");
        if (pos != std::string::npos) {
          const auto end = handlers.find_first_of(" \t", pos);
          const std::string js =
              handlers.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
          const std::string path = "/dev/input/" + js;
          const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
          if (fd >= 0) return fd;
        }
      }
      flush();
    }
  }
  return -1;
}

#endif

#if defined(VRP_HAS_SDL2)
std::string connection_from_bustype(unsigned bus) {
  switch (bus) {
    case 0x03:
      return "USB";
    case 0x05:
      return "Bluetooth";
    case 0x06:
    case 0xff:
      return "virtual";
    default:
      return {};
  }
}

std::string gamepad_connection(SDL_GameController* gc) {
  if (!gc) return {};
  if (const char* path = SDL_GameControllerPath(gc)) {
    const char* slash = std::strrchr(path, '/');
    const char* base = slash ? slash + 1 : path;
    const bool evdev = std::strncmp(base, "event", 5) == 0;
    const bool js = std::strncmp(base, "js", 2) == 0;
    if (evdev || js) {
      std::ifstream in(std::string("/sys/class/input/") + base + "/device/id/bustype");
      unsigned bus = 0;
      if (in >> std::hex >> bus) {
        if (const std::string how = connection_from_bustype(bus); !how.empty()) return how;
      }
    }
  }
  if (SDL_Joystick* joy = SDL_GameControllerGetJoystick(gc)) {
    const SDL_JoystickGUID guid = SDL_JoystickGetGUID(joy);
    const unsigned bus = static_cast<unsigned>(guid.data[0]) |
                         (static_cast<unsigned>(guid.data[1]) << 8);
    if (const std::string how = connection_from_bustype(bus); !how.empty()) return how;
    if (SDL_JoystickCurrentPowerLevel(joy) == SDL_JOYSTICK_POWER_WIRED) return "USB";
  }
  return {};
}
#endif
}

GamepadInput::GamepadInput() {
#if defined(VRP_HAS_SDL2)
  available_ = true;
#else
  available_ = false;
#endif
}

GamepadInput::~GamepadInput() { close(); }

void GamepadInput::close() {
  end_pass_through_watch();
#if defined(VRP_HAS_SDL2)
  if (controller_) {
    SDL_GameControllerClose(static_cast<SDL_GameController*>(controller_));
    controller_ = nullptr;
  }
  device_index_ = -1;
  if (sdl_ready_) {
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
    sdl_ready_ = false;
  }
#endif
}

void GamepadInput::end_pass_through_watch() {
#if defined(__linux__)
  for (int fd : pass_fds_) {
    if (fd >= 0) ::close(fd);
  }
  pass_fds_.clear();
  if (pass_hidraw_fd_ >= 0) {
    ::close(pass_hidraw_fd_);
    pass_hidraw_fd_ = -1;
  }
  pass_hidraw_dualsense_ = false;
  if (pass_js_fd_ >= 0) {
    ::close(pass_js_fd_);
    pass_js_fd_ = -1;
  }
#endif
  pass_active_ = false;
  pass_ps_down_ = false;
  pass_l1_down_ = false;
  pass_r1_down_ = false;
  pass_menu_chord_fired_ = false;
  pass_recenter_chord_fired_ = false;
}

void GamepadInput::begin_pass_through_watch() {
#if defined(__linux__)
  std::string path = last_device_path_;
#if defined(VRP_HAS_SDL2)
  if (path.empty() && controller_) {
    if (const char* p = SDL_GameControllerPath(static_cast<SDL_GameController*>(controller_))) {
      path = p;
      last_device_path_ = path;
    }
  }
#endif
  // Drop exclusive SDL so the desktop app can open the pad.
#if defined(VRP_HAS_SDL2)
  if (controller_) {
    SDL_GameControllerClose(static_cast<SDL_GameController*>(controller_));
    controller_ = nullptr;
  }
  device_index_ = -1;
  if (sdl_ready_) {
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
    sdl_ready_ = false;
  }
#endif
  end_pass_through_watch();
  pass_fds_ = open_all_joystick_evdevs(path);
  pass_hidraw_fd_ = open_sony_hidraw(&pass_hidraw_dualsense_);
  pass_js_fd_ = open_joystick_js_for_dualsense();
  pass_active_ = true;
  if (!pass_fds_.empty() || pass_hidraw_fd_ >= 0 || pass_js_fd_ >= 0) {
    VRP_LOG("Gamepad pass-through watch: %zu evdev + hidraw=%s js=%s (PS+R1=menu, PS+L1=recenter)",
            pass_fds_.size(), pass_hidraw_fd_ >= 0 ? "yes" : "no",
            pass_js_fd_ >= 0 ? "yes" : "no");
  } else {
    VRP_LOG("Gamepad pass-through watch: no evdev/hidraw/js (XR Options long=menu; HMD system=recenter)");
  }
#else
  close();
  pass_active_ = true;
#endif
}

GamepadInput::PassThroughEdges GamepadInput::poll_pass_through() {
  PassThroughEdges out;
#if !defined(__linux__)
  return out;
#else
  if (!pass_active_) return out;
  const auto now = std::chrono::steady_clock::now();

  // Evdev (silent if the app EVIOCGRAB'd the node). Track PS / L1 / R1 only.
  for (int fd : pass_fds_) {
    if (fd < 0) continue;
    input_event ev{};
    while (true) {
      const ssize_t n = ::read(fd, &ev, sizeof(ev));
      if (n != static_cast<ssize_t>(sizeof(ev))) break;
      if (ev.type != EV_KEY) continue;
      const int code = static_cast<int>(ev.code);
      const bool down = ev.value != 0;
      if (is_ps_code(code))
        pass_ps_down_ = down;
      else if (code == BTN_TL)
        pass_l1_down_ = down;
      else if (code == BTN_TR)
        pass_r1_down_ = down;
    }
  }

  // Hidraw — works while RetroArch grabs event*.
  if (pass_hidraw_fd_ >= 0) {
    uint8_t buf[128];
    while (true) {
      const ssize_t n = ::read(pass_hidraw_fd_, buf, sizeof(buf));
      if (n <= 0) break;
      SonyPadLevels lv;
      if (!parse_sony_hid_levels(buf, static_cast<size_t>(n), pass_hidraw_dualsense_, &lv)) continue;
      pass_ps_down_ = lv.ps;
      pass_l1_down_ = lv.l1;
      pass_r1_down_ = lv.r1;
    }
  }

  // joydev survives event EVIOCGRAB.
  if (pass_js_fd_ >= 0) {
    uint8_t raw[8];
    while (true) {
      const ssize_t n = ::read(pass_js_fd_, raw, sizeof(raw));
      if (n != 8) break;
      const uint8_t type = raw[6];
      const uint8_t number = raw[7];
      const int16_t value = static_cast<int16_t>(raw[4] | (raw[5] << 8));
      if ((type & 0x01) == 0) continue;
      const bool down = value != 0;
      if (number == kJsPsBtn)
        pass_ps_down_ = down;
      else if (number == kJsL1Btn)
        pass_l1_down_ = down;
      else if (number == kJsR1Btn)
        pass_r1_down_ = down;
    }
  }

  // Chords: do not bind single Options/PS (those stay with the app — no 暴発).
  // R1 is easier with PS for menu; L1 → recenter.
  const bool menu_chord = pass_ps_down_ && pass_r1_down_;
  const bool recenter_chord = pass_ps_down_ && pass_l1_down_;
  if (menu_chord) {
    if (!pass_menu_chord_fired_) {
      if (pass_menu_chord_at_.time_since_epoch().count() == 0) pass_menu_chord_at_ = now;
      if (now - pass_menu_chord_at_ >= kChordHold) {
        pass_menu_chord_fired_ = true;
        out.menu_toggle = true;
      }
    }
  } else {
    pass_menu_chord_fired_ = false;
    pass_menu_chord_at_ = {};
  }
  if (recenter_chord) {
    if (!pass_recenter_chord_fired_) {
      if (pass_recenter_chord_at_.time_since_epoch().count() == 0) pass_recenter_chord_at_ = now;
      if (now - pass_recenter_chord_at_ >= kChordHold) {
        pass_recenter_chord_fired_ = true;
        out.recenter = true;
      }
    }
  } else {
    pass_recenter_chord_fired_ = false;
    pass_recenter_chord_at_ = {};
  }
  return out;
#endif
}

bool GamepadInput::ensure_open() {
#if !defined(VRP_HAS_SDL2)
  return false;
#else
  if (!available_) return false;
  if (!sdl_ready_) {
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
      VRP_ERR("SDL_InitSubSystem(GAMECONTROLLER): %s", SDL_GetError());
      available_ = false;
      return false;
    }
    SDL_GameControllerEventState(SDL_ENABLE);
    sdl_ready_ = true;
  }

  SDL_PumpEvents();
  SDL_JoystickUpdate();

  if (controller_) {
    auto* gc = static_cast<SDL_GameController*>(controller_);
    if (SDL_GameControllerGetAttached(gc)) return true;
    SDL_GameControllerClose(gc);
    controller_ = nullptr;
    device_index_ = -1;
  }

  const int n = SDL_NumJoysticks();
  for (int i = 0; i < n; ++i) {
    if (!SDL_IsGameController(i)) continue;
    SDL_GameController* gc = SDL_GameControllerOpen(i);
    if (!gc) continue;
    controller_ = gc;
    device_index_ = i;
    if (const char* path = SDL_GameControllerPath(gc)) last_device_path_ = path;
    const char* name = SDL_GameControllerName(gc);
    const std::string how = gamepad_connection(gc);
    VRP_LOG("Gamepad opened: %s (%s)", name ? name : "(unnamed)", how.empty() ? "unknown" : how.c_str());
    return true;
  }
  return false;
#endif
}

std::vector<std::string> GamepadInput::list_names() {
  std::vector<std::string> out;
#if defined(VRP_HAS_SDL2)
  if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) return out;
  const int n = SDL_NumJoysticks();
  for (int i = 0; i < n; ++i) {
    if (!SDL_IsGameController(i)) continue;
    const char* name = SDL_GameControllerNameForIndex(i);
    out.push_back(name ? name : "Game Controller");
  }
  SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
#endif
  return out;
}

GamepadInput::State GamepadInput::poll() {
  State st;
#if !defined(VRP_HAS_SDL2)
  return st;
#else
  if (!ensure_open()) return st;
  auto* gc = static_cast<SDL_GameController*>(controller_);
  st.present = true;
  if (const char* name = SDL_GameControllerName(gc)) st.name = name;
  st.connection = gamepad_connection(gc);
  const SDL_GameControllerType pad_type = SDL_GameControllerGetType(gc);
  st.sony_face = pad_type == SDL_CONTROLLER_TYPE_PS3 || pad_type == SDL_CONTROLLER_TYPE_PS4 ||
                 pad_type == SDL_CONTROLLER_TYPE_PS5 || SDL_GameControllerGetVendor(gc) == 0x054C;

  // Stick full-scale (±32767) stays analog. The cross key is the dpad buttons, not that sample.
  auto axis = [&](SDL_GameControllerAxis a) -> float {
    const Sint16 v = SDL_GameControllerGetAxis(gc, a);
    constexpr float dead = 8000.f;
    if (v > -dead && v < dead) return 0.f;
    return static_cast<float>(v) / 32767.f;
  };

  st.stick_x = axis(SDL_CONTROLLER_AXIS_LEFTX);
  st.stick_y = -axis(SDL_CONTROLLER_AXIS_LEFTY);
  st.stick_rx = axis(SDL_CONTROLLER_AXIS_RIGHTX);
  st.stick_ry = -axis(SDL_CONTROLLER_AXIS_RIGHTY);
  st.nav_left = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT) != 0;
  st.nav_right = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) != 0;
  st.nav_up = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP) != 0;
  st.nav_down = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN) != 0;

  auto edge = [](bool now, bool& prev) {
    const bool e = now && !prev;
    prev = now;
    return e;
  };

  // Keyboard-style repeat for LB/RB seek: immediate on press, then after delay, then interval.
  // repeating becomes true on the first auto-repeat and stays until release (prefetch hold).
  auto key_repeat = [](bool down, bool& held, bool& repeating,
                       std::chrono::steady_clock::time_point& next,
                       std::chrono::steady_clock::time_point now) -> bool {
    constexpr auto kInitial = std::chrono::milliseconds(400);
    constexpr auto kInterval = std::chrono::milliseconds(120);
    if (!down) {
      held = false;
      repeating = false;
      return false;
    }
    if (!held) {
      held = true;
      repeating = false;
      next = now + kInitial;
      return true;
    }
    if (now >= next) {
      next = now + kInterval;
      repeating = true;
      return true;
    }
    return false;
  };

  const bool a = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_A) != 0;
  const bool b = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_B) != 0;
  const bool y = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_Y) != 0;
  const bool l3 = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSTICK) != 0;
  const bool r3 = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSTICK) != 0;
  // Global: PS long → menu, L3+R3 → recenter. Options long also opens menu.
  const bool options = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_START) != 0;
  const bool ps = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_GUIDE) != 0;
  const bool lb = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) != 0;
  const bool rb = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) != 0;

  st.confirm = edge(a, prev_a_);
  st.confirm_held = a;
  st.back = edge(b, prev_b_);
  st.back_held = b;
  st.face_y = edge(y, prev_y_);
  // R3 alone resets view look; L3+R3 is recenter (handled below).
  st.view_reset = edge(r3 && !l3, prev_r3_);
  const auto now = std::chrono::steady_clock::now();
  st.seek_back_held = lb;
  st.seek_forward_held = rb;
  st.l2_held = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8000;
  st.r2_held = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8000;
  st.seek_back = key_repeat(lb, lb_held_, lb_repeating_, lb_next_, now);
  st.seek_forward = key_repeat(rb, rb_held_, rb_repeating_, rb_next_, now);
  st.seek_scrubbing = lb_repeating_ || rb_repeating_;

  if (options) {
    if (!options_down_) {
      options_down_ = true;
      options_long_fired_ = false;
      options_down_at_ = now;
    } else if (!options_long_fired_ && now - options_down_at_ >= kStartLongPress) {
      st.menu_toggle = true;
      options_long_fired_ = true;
    }
  } else if (options_down_) {
    options_down_ = false;
    options_long_fired_ = false;
  }

  if (ps) {
    if (!ps_down_) {
      ps_down_ = true;
      ps_long_fired_ = false;
      ps_down_at_ = now;
    } else if (!ps_long_fired_ && now - ps_down_at_ >= kStartLongPress) {
      st.menu_toggle = true;
      ps_long_fired_ = true;
    }
  } else if (ps_down_) {
    ps_down_ = false;
    ps_long_fired_ = false;
  }

  l3_down_ = l3;
  r3_down_ = r3;
  if (l3 && r3) {
    if (!l3r3_recenter_fired_) {
      l3r3_recenter_fired_ = true;
      st.recenter = true;
    }
  } else {
    l3r3_recenter_fired_ = false;
  }

  return st;
#endif
}

}  // namespace vrp
