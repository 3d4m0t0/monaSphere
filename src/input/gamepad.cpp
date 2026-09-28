#include "input/gamepad.hpp"

#include "common.hpp"

#include <fstream>

#if defined(VRP_HAS_SDL2)
#include <SDL.h>
#endif

namespace vrp {

namespace {
constexpr auto kStartLongPress = std::chrono::milliseconds(700);

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

  auto axis = [&](SDL_GameControllerAxis a) -> float {
    const Sint16 v = SDL_GameControllerGetAxis(gc, a);
    constexpr float dead = 8000.f;
    if (v > -dead && v < dead) return 0.f;
    return static_cast<float>(v) / 32767.f;
  };

  float sx = axis(SDL_CONTROLLER_AXIS_LEFTX);
  float sy = -axis(SDL_CONTROLLER_AXIS_LEFTY);
  if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) sx = -1.f;
  if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) sx = 1.f;
  if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP)) sy = 1.f;
  if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) sy = -1.f;
  st.stick_x = sx;
  st.stick_y = sy;

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
  const bool start = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_START) != 0 ||
                     SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_GUIDE) != 0;
  const bool lb = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) != 0;
  const bool rb = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) != 0;

  st.confirm = edge(a, prev_a_);
  st.confirm_held = a;
  st.back = edge(b, prev_b_);
  st.back_held = b;
  const auto now = std::chrono::steady_clock::now();
  st.seek_back = key_repeat(lb, lb_held_, lb_repeating_, lb_next_, now);
  st.seek_forward = key_repeat(rb, rb_held_, rb_repeating_, rb_next_, now);
  st.seek_scrubbing = lb_repeating_ || rb_repeating_;

  if (start) {
    if (!start_down_) {
      start_down_ = true;
      start_long_fired_ = false;
      start_down_at_ = now;
    } else if (!start_long_fired_ && now - start_down_at_ >= kStartLongPress) {
      st.recenter = true;
      start_long_fired_ = true;
    }
  } else if (start_down_) {
    if (!start_long_fired_) st.menu_toggle = true;
    start_down_ = false;
    start_long_fired_ = false;
  }

  return st;
#endif
}

}  // namespace vrp
