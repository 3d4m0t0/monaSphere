#include "input/pad_filter.hpp"

#include "common.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

namespace vrp {
namespace {

constexpr int kPsLongMs = 700;
constexpr int kVendorSony = 0x054c;

bool has_bit(const unsigned long* bits, int bit) {
  return (bits[bit / (sizeof(unsigned long) * 8)] >> (bit % (sizeof(unsigned long) * 8))) & 1UL;
}

bool is_sony_gamepad(int fd) {
  struct input_id id {};
  if (ioctl(fd, EVIOCGID, &id) < 0) return false;
  if (static_cast<int>(id.vendor) != kVendorSony) return false;
  unsigned long keyb[(KEY_MAX / (sizeof(unsigned long) * 8)) + 1]{};
  if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keyb)), keyb) < 0) return false;
  return has_bit(keyb, BTN_MODE) && has_bit(keyb, BTN_SOUTH);
}

std::string find_sony_event_path() {
  DIR* d = opendir("/dev/input");
  if (!d) return {};
  std::string best;
  dirent* ent = nullptr;
  while ((ent = readdir(d)) != nullptr) {
    if (std::strncmp(ent->d_name, "event", 5) != 0) continue;
    const std::string path = std::string("/dev/input/") + ent->d_name;
    const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) continue;
    const bool ok = is_sony_gamepad(fd);
    ::close(fd);
    if (ok) {
      best = path;
      break;
    }
  }
  closedir(d);
  return best;
}

void copy_bits(int src, int dst, int type, int max_code) {
  const size_t nlong = (max_code / (sizeof(unsigned long) * 8)) + 1;
  std::vector<unsigned long> bits(nlong, 0);
  if (ioctl(src, EVIOCGBIT(type, bits.size() * sizeof(unsigned long)), bits.data()) < 0) return;
  for (int code = 0; code <= max_code; ++code) {
    if (!has_bit(bits.data(), code)) continue;
    if (type == EV_KEY) {
      ioctl(dst, UI_SET_KEYBIT, code);
    } else if (type == EV_ABS) {
      ioctl(dst, UI_SET_ABSBIT, code);
      struct input_absinfo abs {};
      if (ioctl(src, EVIOCGABS(code), &abs) == 0) {
        struct uinput_abs_setup setup {};
        setup.code = static_cast<__u16>(code);
        setup.absinfo = abs;
        ioctl(dst, UI_ABS_SETUP, &setup);
      }
    }
  }
}

float axis_norm(int v, const input_absinfo& abs) {
  const int min_v = abs.minimum;
  const int max_v = abs.maximum;
  if (max_v == min_v) return 0.f;
  const float mid = 0.5f * (static_cast<float>(min_v) + static_cast<float>(max_v));
  const float half = 0.5f * static_cast<float>(max_v - min_v);
  if (half <= 0.f) return 0.f;
  float n = (static_cast<float>(v) - mid) / half;
  if (n > 1.f) n = 1.f;
  if (n < -1.f) n = -1.f;
  constexpr float dead = 0.18f;
  if (n > -dead && n < dead) return 0.f;
  return n;
}

}  // namespace

PadFilter::~PadFilter() { stop(); }

bool PadFilter::start() {
  std::lock_guard<std::mutex> lock(mu_);
  if (running_.load(std::memory_order_relaxed)) return true;
  if (!open_physical()) return false;
  if (!create_uinput()) {
    close_all();
    return false;
  }
  resolve_uinput_event_path();
  running_.store(true, std::memory_order_release);
  std::cerr << "[monaSphere] PadFilter: grab " << phys_path_ << " → "
            << (uinput_event_path_.empty() ? "uinput" : uinput_event_path_)
            << " (PS long=menu, L3+R3=recenter)\n";
  return true;
}

void PadFilter::stop() {
  std::lock_guard<std::mutex> lock(mu_);
  if (!running_.load(std::memory_order_relaxed) && phys_fd_ < 0 && uinput_fd_ < 0) return;
  running_.store(false, std::memory_order_release);
  close_all();
  std::cerr << "[monaSphere] PadFilter: stopped\n";
}

std::string PadFilter::uinput_event_path() const {
  std::lock_guard<std::mutex> lock(mu_);
  return uinput_event_path_;
}

bool PadFilter::open_physical() {
  phys_path_ = find_sony_event_path();
  if (phys_path_.empty()) {
    std::cerr << "[monaSphere] PadFilter: no Sony pad event node\n";
    return false;
  }
  phys_fd_ = ::open(phys_path_.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (phys_fd_ < 0) {
    std::cerr << "[monaSphere] PadFilter: open " << phys_path_ << " failed: " << std::strerror(errno)
              << '\n';
    return false;
  }
  if (ioctl(phys_fd_, EVIOCGRAB, 1) < 0) {
    std::cerr << "[monaSphere] PadFilter: EVIOCGRAB failed: " << std::strerror(errno) << '\n';
    ::close(phys_fd_);
    phys_fd_ = -1;
    return false;
  }
  for (int code : {ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ}) {
    ioctl(phys_fd_, EVIOCGABS(code), &absinfo_[code]);
  }
  return true;
}

bool PadFilter::create_uinput() {
  uinput_fd_ = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
  if (uinput_fd_ < 0) {
    std::cerr << "[monaSphere] PadFilter: open /dev/uinput failed: " << std::strerror(errno)
              << " (need uinput + write ACL)\n";
    return false;
  }

  ioctl(uinput_fd_, UI_SET_EVBIT, EV_KEY);
  ioctl(uinput_fd_, UI_SET_EVBIT, EV_ABS);
  ioctl(uinput_fd_, UI_SET_EVBIT, EV_SYN);
  copy_bits(phys_fd_, uinput_fd_, EV_KEY, KEY_MAX);
  copy_bits(phys_fd_, uinput_fd_, EV_ABS, ABS_MAX);

  struct input_id id {};
  ioctl(phys_fd_, EVIOCGID, &id);

  struct uinput_setup usetup {};
  usetup.id.bustype = id.bustype ? id.bustype : BUS_USB;
  usetup.id.vendor = id.vendor;
  usetup.id.product = id.product;
  usetup.id.version = id.version;
  std::snprintf(usetup.name, UINPUT_MAX_NAME_SIZE, "monaSphere Virtual Pad");

  if (ioctl(uinput_fd_, UI_DEV_SETUP, &usetup) < 0) {
    std::cerr << "[monaSphere] PadFilter: UI_DEV_SETUP failed: " << std::strerror(errno) << '\n';
    ::close(uinput_fd_);
    uinput_fd_ = -1;
    return false;
  }
  if (ioctl(uinput_fd_, UI_DEV_CREATE) < 0) {
    std::cerr << "[monaSphere] PadFilter: UI_DEV_CREATE failed: " << std::strerror(errno) << '\n';
    ::close(uinput_fd_);
    uinput_fd_ = -1;
    return false;
  }

  return true;
}

void PadFilter::resolve_uinput_event_path() {
  uinput_event_path_.clear();
  // udev can lag a few ms after UI_DEV_CREATE.
  for (int attempt = 0; attempt < 20; ++attempt) {
    if (attempt > 0) ::usleep(5000);
    DIR* d = opendir("/dev/input");
    if (!d) return;
    dirent* ent = nullptr;
    while ((ent = readdir(d)) != nullptr) {
      if (std::strncmp(ent->d_name, "event", 5) != 0) continue;
      const std::string path = std::string("/dev/input/") + ent->d_name;
      const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
      if (fd < 0) continue;
      char name[256]{};
      const bool ok = ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 &&
                      std::strstr(name, "monaSphere Virtual Pad") != nullptr;
      ::close(fd);
      if (ok) {
        uinput_event_path_ = path;
        closedir(d);
        return;
      }
    }
    closedir(d);
  }
  std::cerr << "[monaSphere] PadFilter: could not resolve uinput event node\n";
}

void PadFilter::close_all() {
  if (uinput_fd_ >= 0) {
    ioctl(uinput_fd_, UI_DEV_DESTROY);
    ::close(uinput_fd_);
    uinput_fd_ = -1;
  }
  if (phys_fd_ >= 0) {
    ioctl(phys_fd_, EVIOCGRAB, 0);
    ::close(phys_fd_);
    phys_fd_ = -1;
  }
  phys_path_.clear();
  uinput_event_path_.clear();
  ps_down_ = false;
  ps_long_fired_ = false;
  ps_menu_gesture_ = false;
  ps_release_pending_ = false;
  ps_ignore_until_ = {};
  last_guest_forward_ = true;
  dbg_forward_count_ = 0;
  l3_down_ = false;
  r3_down_ = false;
  recenter_chord_fired_ = false;
  snap_ = {};
  prev_a_ = prev_b_ = prev_y_ = false;
}

void PadFilter::stamp(struct input_event* ev) {
  if (!ev) return;
  gettimeofday(&ev->time, nullptr);
}

void PadFilter::write_uinput(const struct input_event& ev) {
  if (uinput_fd_ < 0) return;
  struct input_event out = ev;
  if (out.time.tv_sec == 0 && out.time.tv_usec == 0) stamp(&out);
  if (out.type == EV_KEY) {
    VRP_LOG("PadFilter: uinput KEY code=%d value=%d%s", static_cast<int>(out.code),
            static_cast<int>(out.value),
            out.code == BTN_MODE ? " (BTN_MODE/PS)" : "");
  }
  (void)::write(uinput_fd_, &out, sizeof(out));
}

void PadFilter::flush_uinput_sync() {
  struct input_event syn {};
  syn.type = EV_SYN;
  syn.code = SYN_REPORT;
  syn.value = 0;
  write_uinput(syn);
}

void PadFilter::inject_key(int code, int value) {
  VRP_LOG("PadFilter: INJECT key=%d value=%d%s", code, value,
          code == BTN_MODE ? " (BTN_MODE/PS)" : "");
  struct input_event ev {};
  stamp(&ev);
  ev.type = EV_KEY;
  ev.code = static_cast<__u16>(code);
  ev.value = value;
  write_uinput(ev);
}

void PadFilter::apply_key_to_snap(int code, int value) {
  const bool down = value != 0;
  auto edge = [](bool now, bool& prev) {
    const bool e = now && !prev;
    prev = now;
    return e;
  };
  switch (code) {
    case BTN_SOUTH:
      snap_.confirm = edge(down, prev_a_);
      snap_.confirm_held = down;
      break;
    case BTN_EAST:
      snap_.back = edge(down, prev_b_);
      snap_.back_held = down;
      break;
    case BTN_NORTH:
      snap_.face_y = edge(down, prev_y_);
      break;
    case BTN_TL:
      snap_.seek_back_held = down;
      break;
    case BTN_TR:
      snap_.seek_forward_held = down;
      break;
    case BTN_DPAD_UP:
      snap_.nav_up = down;
      break;
    case BTN_DPAD_DOWN:
      snap_.nav_down = down;
      break;
    case BTN_DPAD_LEFT:
      snap_.nav_left = down;
      break;
    case BTN_DPAD_RIGHT:
      snap_.nav_right = down;
      break;
    default:
      break;
  }
}

PadFilter::Edges PadFilter::pump(bool guest_forward) {
  Edges edges{};
  std::lock_guard<std::mutex> lock(mu_);
  if (!running_.load(std::memory_order_relaxed) || phys_fd_ < 0) return edges;

  snap_.confirm = false;
  snap_.back = false;
  snap_.face_y = false;
  snap_.menu_toggle = false;
  snap_.recenter = false;
  snap_.view_reset = false;
  snap_.present = true;
  snap_.sony_face = true;
  snap_.name = "monaSphere Virtual Pad";

  const auto now = std::chrono::steady_clock::now();

  // End menu-gesture block after PS is up and cooldown elapsed.
  if (ps_menu_gesture_ && !ps_down_ && now >= ps_ignore_until_) {
    VRP_LOG("PadFilter: menu-gesture end (guest may receive again)");
    ps_menu_gesture_ = false;
    ps_long_fired_ = false;
  }

  // Guest sees pad only when menu says so AND we are not inside a PS-long gesture.
  // Re-evaluate after long-fire in this pump (do not capture once).
  auto allow_guest = [&]() { return guest_forward && !ps_menu_gesture_; };
  if (guest_forward != last_guest_forward_) {
    VRP_LOG("PadFilter: guest_forward %d → %d (menu_gesture=%d allow_guest=%d)",
            last_guest_forward_ ? 1 : 0, guest_forward ? 1 : 0, ps_menu_gesture_ ? 1 : 0,
            allow_guest() ? 1 : 0);
    last_guest_forward_ = guest_forward;
  }

  // Short-PS up half: only when guest is allowed.
  if (ps_release_pending_) {
    ps_release_pending_ = false;
    if (allow_guest()) {
      VRP_LOG("PadFilter: short-PS pending UP → guest");
      inject_key(BTN_MODE, 0);
      flush_uinput_sync();
    } else {
      VRP_LOG("PadFilter: short-PS pending UP DROPPED (allow_guest=0)");
    }
  }

  if (ps_down_ && !ps_long_fired_ && now >= ps_ignore_until_) {
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - ps_down_at_).count();
    if (ms >= kPsLongMs) {
      ps_long_fired_ = true;
      ps_menu_gesture_ = true;      // block all uinput until up+cooldown
      ps_release_pending_ = false;
      edges.menu_toggle = true;
      snap_.menu_toggle = true;
      VRP_LOG("PadFilter: PS LONG fired (hold %lldms) — menu_toggle, block guest",
              static_cast<long long>(ms));
    }
  }

  for (;;) {
    struct input_event ev {};
    const ssize_t n = ::read(phys_fd_, &ev, sizeof(ev));
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      break;
    }
    if (n != static_cast<ssize_t>(sizeof(ev))) break;

    if (ev.type == EV_KEY) {
      const int code = ev.code;
      const int value = ev.value;

      if (code == BTN_MODE) {
        VRP_LOG("PadFilter: phys PS value=%d down=%d long=%d gesture=%d ignore=%d allow=%d",
                value, ps_down_ ? 1 : 0, ps_long_fired_ ? 1 : 0, ps_menu_gesture_ ? 1 : 0,
                (now < ps_ignore_until_) ? 1 : 0, allow_guest() ? 1 : 0);
        // Post-long cooldown: drop bounce / re-press so it cannot become a false short inject.
        if (now < ps_ignore_until_) {
          if (value == 0) ps_down_ = false;
          VRP_LOG("PadFilter: PS swallowed (cooldown)");
          continue;
        }
        if (value == 1) {
          // Do not start a new short gesture while a long menu-gesture is still active.
          if (ps_menu_gesture_) {
            VRP_LOG("PadFilter: PS down ignored (menu-gesture active)");
            continue;
          }
          ps_down_ = true;
          ps_long_fired_ = false;
          ps_release_pending_ = false;
          ps_down_at_ = now;
          VRP_LOG("PadFilter: PS down (arm short/long)");
          continue;  // never forward PS down
        }
        if (value == 0 && ps_down_) {
          const bool was_long = ps_long_fired_ || ps_menu_gesture_;
          ps_down_ = false;
          // Short → guest pulse on release (only if guest owns the pad).
          // Long → never inject; keep menu-gesture until cooldown ends.
          if (was_long) {
            ps_release_pending_ = false;
            ps_long_fired_ = true;
            ps_menu_gesture_ = true;
            ps_ignore_until_ = now + std::chrono::milliseconds(400);
            VRP_LOG("PadFilter: PS up after LONG — no inject, cooldown 400ms");
          } else if (allow_guest()) {
            VRP_LOG("PadFilter: PS up SHORT → inject pulse to guest");
            inject_key(BTN_MODE, 1);
            flush_uinput_sync();
            ps_release_pending_ = true;
          } else {
            VRP_LOG("PadFilter: PS up SHORT but allow_guest=0 — no inject");
          }
          continue;
        }
        if (value == 0 && !ps_down_) {
          VRP_LOG("PadFilter: PS up ignored (not down)");
        }
        continue;
      }

      if (code == BTN_THUMBL) {
        if (value == 1) l3_down_ = true;
        if (value == 0) {
          l3_down_ = false;
          recenter_chord_fired_ = false;
        }
      }
      if (code == BTN_THUMBR) {
        if (value == 1) r3_down_ = true;
        if (value == 0) {
          r3_down_ = false;
          recenter_chord_fired_ = false;
        }
        if (value == 1 && !l3_down_) {
          snap_.view_reset = true;
        }
      }
      if (l3_down_ && r3_down_ && !recenter_chord_fired_) {
        recenter_chord_fired_ = true;
        edges.recenter = true;
        snap_.recenter = true;
      }

      apply_key_to_snap(code, value);
      if (allow_guest()) {
        write_uinput(ev);
      } else if (value != 0 && (++dbg_forward_count_ % 30) == 1) {
        VRP_LOG("PadFilter: KEY code=%d blocked (allow_guest=0)", code);
      }
      continue;
    }

    if (ev.type == EV_ABS) {
      switch (ev.code) {
        case ABS_X:
          snap_.stick_x = axis_norm(ev.value, absinfo_[ABS_X]);
          break;
        case ABS_Y:
          snap_.stick_y = -axis_norm(ev.value, absinfo_[ABS_Y]);
          break;
        case ABS_RX:
          snap_.stick_rx = axis_norm(ev.value, absinfo_[ABS_RX]);
          break;
        case ABS_RY:
          snap_.stick_ry = -axis_norm(ev.value, absinfo_[ABS_RY]);
          break;
        case ABS_Z:
          snap_.l2_held = ev.value > (absinfo_[ABS_Z].maximum / 4);
          break;
        case ABS_RZ:
          snap_.r2_held = ev.value > (absinfo_[ABS_RZ].maximum / 4);
          break;
        case ABS_HAT0X:
          snap_.nav_left = ev.value < 0;
          snap_.nav_right = ev.value > 0;
          break;
        case ABS_HAT0Y:
          snap_.nav_up = ev.value < 0;
          snap_.nav_down = ev.value > 0;
          break;
        default:
          break;
      }
      if (allow_guest()) write_uinput(ev);
      continue;
    }

    if (allow_guest()) write_uinput(ev);
  }

  return edges;
}

GamepadInput::State PadFilter::snapshot() const {
  std::lock_guard<std::mutex> lock(mu_);
  return snap_;
}

}  // namespace vrp
