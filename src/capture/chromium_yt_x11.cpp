#include "capture/chromium_yt_x11.hpp"
#include "capture/eis_pointer.hpp"

#include "common.hpp"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xrandr.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

namespace vrp {
namespace {

constexpr const char* kClass = "monaSphereYT";

bool get_wm_class(Display* dpy, Window w, std::string* name, std::string* cls) {
  XClassHint hint{};
  if (!XGetClassHint(dpy, w, &hint)) return false;
  if (name && hint.res_name) *name = hint.res_name;
  if (cls && hint.res_class) *cls = hint.res_class;
  if (hint.res_name) XFree(hint.res_name);
  if (hint.res_class) XFree(hint.res_class);
  return true;
}

std::string window_title(Display* dpy, Window w) {
  Atom net = XInternAtom(dpy, "_NET_WM_NAME", True);
  Atom utf8 = XInternAtom(dpy, "UTF8_STRING", True);
  if (net != None && utf8 != None) {
    Atom actual = None;
    int fmt = 0;
    unsigned long n = 0, bytes = 0;
    unsigned char* prop = nullptr;
    if (XGetWindowProperty(dpy, w, net, 0, 1024, False, utf8, &actual, &fmt, &n, &bytes, &prop) ==
            Success &&
        prop && n > 0) {
      std::string t(reinterpret_cast<char*>(prop), n);
      XFree(prop);
      return t;
    }
    if (prop) XFree(prop);
  }
  char* name = nullptr;
  if (XFetchName(dpy, w, &name) && name) {
    std::string t(name);
    XFree(name);
    return t;
  }
  return {};
}

bool is_chrome_family(const std::string& name, const std::string& cls) {
  auto low = [](std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };
  const std::string a = low(name), b = low(cls);
  return a.find("chrome") != std::string::npos || a.find("chromium") != std::string::npos ||
         b.find("chrome") != std::string::npos || b.find("chromium") != std::string::npos ||
         a.find("monasphereyt") != std::string::npos || b.find("monasphereyt") != std::string::npos;
}

bool title_is_youtube(const std::string& title) {
  std::string t = title;
  for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return t.find("youtube") != std::string::npos;
}

bool class_is_ours(const std::string& name, const std::string& cls) {
  return name == kClass || cls == kClass;
}

/** Score candidate windows; higher is better. -1 = reject. */
int score_window(Display* dpy, Window w) {
  std::string name, cls;
  if (!get_wm_class(dpy, w, &name, &cls)) return -1;
  const std::string title = window_title(dpy, w);
  const bool ours = class_is_ours(name, cls);
  // Prefer --class=monaSphereYT. Fallback: Chrome --app window (live) only
  // when class was ignored by a shared singleton — still require chrome family.
  const bool chrome = is_chrome_family(name, cls);
  const bool yt = title_is_youtube(title);
  if (!ours && !(chrome && yt)) return -1;

  XWindowAttributes wa{};
  if (!XGetWindowAttributes(dpy, w, &wa) || wa.map_state != IsViewable) return -1;
  if (wa.width < 200 || wa.height < 200) return -1;

  int score = wa.width * wa.height;
  if (ours) score += 50'000'000;  // prefer --class=monaSphereYT
  if (yt) score += 1'000'000;
  return score;
}

bool find_best(Display* dpy, Window root, Window* out, int* best_score) {
  Window rr = 0, parent = 0;
  Window* kids = nullptr;
  unsigned n = 0;
  if (!XQueryTree(dpy, root, &rr, &parent, &kids, &n) || !kids) return false;
  bool any = false;
  for (unsigned i = 0; i < n; ++i) {
    const int sc = score_window(dpy, kids[i]);
    if (sc > *best_score) {
      *best_score = sc;
      *out = kids[i];
      any = true;
    }
    Window nested = 0;
    int nested_score = *best_score;
    if (find_best(dpy, kids[i], &nested, &nested_score) && nested_score > *best_score) {
      *best_score = nested_score;
      *out = nested;
      any = true;
    }
  }
  XFree(kids);
  return any;
}

}  // namespace

ChromiumYtX11::~ChromiumYtX11() { clear(); }

void ChromiumYtX11::set_error(const std::string& e) {
  last_error_ = e;
  VRP_LOG("live-x11: %s", e.c_str());
}

bool ChromiumYtX11::can_inject() const {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  if (eis_ && eis_->ready()) return true;
  return window_ != 0 || screen_input_;
}

void ChromiumYtX11::clear() {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  window_ = 0;
  screen_input_ = false;
  xtest_w_ = xtest_h_ = 0;
  eis_ = nullptr;
  if (display_) {
    XCloseDisplay(static_cast<Display*>(display_));
    display_ = nullptr;
  }
}

void ChromiumYtX11::set_screen_rect(int x, int y, int w, int h) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  place_x_ = x;
  place_y_ = y;
  have_place_ = true;
  if (w > 0) {
    width_ = w;
    xtest_w_ = w;
  }
  if (h > 0) {
    height_ = h;
    xtest_h_ = h;
  }
  screen_input_ = true;
}

bool ChromiumYtX11::find_xrandr_output_unlocked(const std::string& needle, int* x, int* y, int* w,
                                                int* h) const {
  if (!display_ || needle.empty() || !x || !y || !w || !h) return false;
  auto* dpy = static_cast<Display*>(display_);
  int evt = 0, err = 0;
  if (!XRRQueryExtension(dpy, &evt, &err)) return false;

  std::string needle_l = needle;
  for (char& c : needle_l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  Window root = DefaultRootWindow(dpy);
  XRRScreenResources* res = XRRGetScreenResourcesCurrent(dpy, root);
  if (!res) return false;

  bool found = false;
  for (int i = 0; i < res->noutput; ++i) {
    XRROutputInfo* oi = XRRGetOutputInfo(dpy, res, res->outputs[i]);
    if (!oi) continue;
    const bool connected = oi->connection == RR_Connected && oi->crtc != None && oi->name;
    if (connected) {
      std::string name = oi->name;
      for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (name.find(needle_l) != std::string::npos) {
        XRRCrtcInfo* ci = XRRGetCrtcInfo(dpy, res, oi->crtc);
        if (ci && ci->width > 0 && ci->height > 0) {
          *x = ci->x;
          *y = ci->y;
          *w = static_cast<int>(ci->width);
          *h = static_cast<int>(ci->height);
          found = true;
        }
        if (ci) XRRFreeCrtcInfo(ci);
      }
    }
    XRRFreeOutputInfo(oi);
    if (found) break;
  }
  XRRFreeScreenResources(res);
  return found;
}

bool ChromiumYtX11::bind_output_by_name(const std::string& output_name_substr, int cfg_w,
                                        int cfg_h) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  if (!ensure_display()) return false;
  const int cw = cfg_w > 0 ? cfg_w : 1280;
  const int ch = cfg_h > 0 ? cfg_h : 720;
  int x = 0, y = 0, w = 0, h = 0;
  // Match short needle ("monaSphereYT") as well as full "Virtual-monaSphereYT".
  std::string needle = output_name_substr;
  if (!find_xrandr_output_unlocked(needle, &x, &y, &w, &h)) {
    const auto pos = needle.find("monaSphere");
    if (pos != std::string::npos) {
      needle = needle.substr(pos);
      if (!find_xrandr_output_unlocked(needle, &x, &y, &w, &h)) return false;
    } else {
      return false;
    }
  }
  place_x_ = x;
  place_y_ = y;
  have_place_ = true;
  width_ = cw;
  height_ = ch;
  // Fallback inject span = cfg (not Xrandr CRTC). Under primary scale≠1 the CRTC is
  // often 2× the Virtual mode; mapping into that threw the seat pointer off the
  // XWayland surface → Embedded cursor vanished and sticks looked dead.
  xtest_w_ = cw;
  xtest_h_ = ch;
  screen_input_ = true;
  cursor_x_ = std::clamp(cursor_x_, 0.f, static_cast<float>(std::max(1, width_ - 1)));
  cursor_y_ = std::clamp(cursor_y_, 0.f, static_cast<float>(std::max(1, height_ - 1)));
  VRP_LOG("live-x11: bind Xrandr origin @%d,%d (crtc %dx%d) cfg clamp %dx%d", x, y, w, h, cw,
          ch);
  return true;
}

bool ChromiumYtX11::ensure_display() {
  if (display_) return true;
  Display* dpy = XOpenDisplay(nullptr);
  if (!dpy) {
    set_error("XOpenDisplay failed");
    return false;
  }
  int evt = 0, err = 0, maj = 0, min = 0;
  if (!XTestQueryExtension(dpy, &evt, &err, &maj, &min)) {
    XCloseDisplay(dpy);
    set_error("XTest extension missing");
    return false;
  }
  display_ = dpy;
  return true;
}

bool ChromiumYtX11::find_window() {
  if (!ensure_display()) return false;
  auto* dpy = static_cast<Display*>(display_);
  Window found = 0;
  int best = -1;
  // Shared Chrome profile often ignores --class; also match Chrome --app title.
  if (!find_best(dpy, DefaultRootWindow(dpy), &found, &best) || found == 0) {
    set_error("Live Chrome window not found yet");
    return false;
  }
  window_ = found;
  return true;
}

bool ChromiumYtX11::refresh_geometry() {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  if (!display_ || !window_) return false;
  auto* dpy = static_cast<Display*>(display_);
  Window root = 0;
  int x = 0, y = 0;
  unsigned w = 0, h = 0, bw = 0, depth = 0;
  if (!XGetGeometry(dpy, window_, &root, &x, &y, &w, &h, &bw, &depth)) return false;
  if (w < 2 || h < 2) return false;
  // Remember Chrome client size for XSendEvent mapping. Do not overwrite
  // width_/height_ when MONITOR screen_input_ is active — those stay Virtual-*.
  client_w_ = static_cast<int>(w);
  client_h_ = static_cast<int>(h);
  if (!screen_input_) {
    width_ = client_w_;
    height_ = client_h_;
    cursor_x_ = std::clamp(cursor_x_, 0.f, static_cast<float>(width_ - 1));
    cursor_y_ = std::clamp(cursor_y_, 0.f, static_cast<float>(height_ - 1));
  }
  return true;
}

void ChromiumYtX11::client_cursor(int* cx, int* cy) const {
  if (!cx || !cy) return;
  const int cw = client_w_ > 1 ? client_w_ : width_;
  const int ch = client_h_ > 1 ? client_h_ : height_;
  if (width_ > 1 && height_ > 1 && (cw != width_ || ch != height_)) {
    const float u = cursor_x_ / static_cast<float>(width_ - 1);
    const float v = cursor_y_ / static_cast<float>(height_ - 1);
    *cx = std::clamp(static_cast<int>(u * static_cast<float>(cw - 1) + 0.5f), 0, cw - 1);
    *cy = std::clamp(static_cast<int>(v * static_cast<float>(ch - 1) + 0.5f), 0, ch - 1);
  } else {
    *cx = static_cast<int>(cursor_x_);
    *cy = static_cast<int>(cursor_y_);
  }
}

bool ChromiumYtX11::root_origin(int* root_x, int* root_y) const {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  if (!display_ || !window_ || !root_x || !root_y) return false;
  auto* dpy = static_cast<Display*>(display_);
  Window child = 0;
  return XTranslateCoordinates(dpy, window_, DefaultRootWindow(dpy), 0, 0, root_x, root_y,
                               &child) != 0;
}

bool ChromiumYtX11::apply_chrome(int width, int height, bool hide_host, bool move) {
  if (!window_ || !display_) return false;
  auto* dpy = static_cast<Display*>(display_);
  // MONITOR mode: keep configured virtual-display clamp (set via bind_output_by_name).
  if (!screen_input_) {
    width_ = std::max(320, width);
    height_ = std::max(240, height);
  }

  // Skip taskbar / pager so the host stays clean. Keep WM frame (--app chrome is enough).
  Atom state = XInternAtom(dpy, "_NET_WM_STATE", False);
  Atom skip_task = XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
  Atom skip_pager = XInternAtom(dpy, "_NET_WM_STATE_SKIP_PAGER", False);
  Atom atoms[2] = {skip_task, skip_pager};
  XChangeProperty(dpy, window_, state, XA_ATOM, 32, PropModeReplace,
                  reinterpret_cast<unsigned char*>(atoms), 2);

  int x = 80;
  int y = 80;
  if (move) {
    if (hide_host) {
      x = -32000;
      y = 0;
    } else if (have_place_) {
      x = place_x_;
      y = place_y_;
    }
    XMoveResizeWindow(dpy, window_, x, y, static_cast<unsigned>(std::max(320, width)),
                      static_cast<unsigned>(std::max(240, height)));
    XMapRaised(dpy, window_);
  }
  // MONITOR + fullscreen Chrome: do not XResizeWindow — KWin owns placement.
  XFlush(dpy);
  (void)refresh_geometry();

  cursor_x_ = width_ * 0.5f;
  cursor_y_ = height_ * 0.5f;
  (void)warp_cursor();
  VRP_LOG("live-x11: prepared 0x%lx clamp %dx%d xtest %dx%d %s", window_, width_, height_,
          xtest_w() > 0 ? xtest_w() : width_, xtest_h() > 0 ? xtest_h() : height_,
          !move ? "input-only (KWin placed)" : (hide_host ? "off-screen" : "on-output"));
  return true;
}

bool ChromiumYtX11::prepare_window(int width, int height, bool hide_host, bool move) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  if (!find_window()) return false;
  return apply_chrome(width, height, hide_host, move);
}

bool ChromiumYtX11::window_alive_unlocked() const {
  if (!display_ || !window_) return false;
  auto* dpy = static_cast<Display*>(display_);
  XWindowAttributes attrs{};
  XErrorHandler prev = XSetErrorHandler([](Display*, XErrorEvent*) -> int { return 0; });
  const bool ok = XGetWindowAttributes(dpy, window_, &attrs) != 0;
  XSync(dpy, False);
  XSetErrorHandler(prev);
  return ok;
}

bool ChromiumYtX11::ensure_inject_target() {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  if (!ensure_display()) return false;
  if (window_ && !window_alive_unlocked()) {
    VRP_LOG("live-x11: inject target xid 0x%lx gone — rebinding", window_);
    window_ = 0;
  }
  if (!window_) {
    if (!find_window()) {
      // screen_input_ alone can still drive XTest on the virtual output.
      return screen_input_;
    }
  }
  if (window_) (void)refresh_geometry();
  return window_ != 0 || screen_input_;
}

void ChromiumYtX11::eis_abs_from_cursor(double* ax, double* ay) const {
  if (!ax || !ay || !eis_) return;
  if (!eis_->region_local()) {
    *ax = static_cast<double>(eis_ox_) + static_cast<double>(cursor_x_);
    *ay = static_cast<double>(eis_oy_) + static_cast<double>(cursor_y_);
    return;
  }
  // cursor_* is configured Virtual size (often physical mode). libei regions
  // are compositor-logical (mode / scale). Map so 4K@scale3 → 1280×720 region.
  const uint32_t rw = eis_->region_w();
  const uint32_t rh = eis_->region_h();
  if (rw > 1 && rh > 1 && width_ > 1 && height_ > 1 &&
      (static_cast<int>(rw) != width_ || static_cast<int>(rh) != height_)) {
    *ax = static_cast<double>(cursor_x_) * static_cast<double>(rw - 1) /
          static_cast<double>(width_ - 1);
    *ay = static_cast<double>(cursor_y_) * static_cast<double>(rh - 1) /
          static_cast<double>(height_ - 1);
  } else {
    *ax = static_cast<double>(cursor_x_);
    *ay = static_cast<double>(cursor_y_);
  }
}

void ChromiumYtX11::set_cursor(float x, float y) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  (void)ensure_inject_target();
  cursor_x_ = std::clamp(x, 0.f, static_cast<float>(std::max(1, width_ - 1)));
  cursor_y_ = std::clamp(y, 0.f, static_cast<float>(std::max(1, height_ - 1)));
  if (eis_ && eis_->ready()) {
    double ax = 0, ay = 0;
    eis_abs_from_cursor(&ax, &ay);
    eis_->warp(ax, ay);
    return;
  }
  if (window_) (void)focus_window();
  (void)warp_cursor();
  if (!screen_input_) (void)send_motion_event();
}

void ChromiumYtX11::move_cursor(float dx, float dy) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  if (!ensure_inject_target() && !(eis_ && eis_->ready())) return;
  cursor_x_ = std::clamp(cursor_x_ + dx, 0.f, static_cast<float>(std::max(1, width_ - 1)));
  cursor_y_ = std::clamp(cursor_y_ + dy, 0.f, static_cast<float>(std::max(1, height_ - 1)));
  if (eis_ && eis_->ready()) {
    double ax = 0, ay = 0;
    eis_abs_from_cursor(&ax, &ay);
    eis_->warp(ax, ay);
    return;
  }
  if (window_) (void)focus_window();
  (void)warp_cursor();
  if (!screen_input_) (void)send_motion_event();
}

bool ChromiumYtX11::root_coords(int* root_x, int* root_y) const {
  if (!root_x || !root_y) return false;
  // Prefer Chrome window origin + client mapping. Absolute Xrandr/CRTC coords
  // (especially 2× under primary scale≠1) often miss the XWayland surface, so
  // the Wayland seat pointer never enters the Virtual MONITOR and Embedded
  // cursor stays hidden.
  if (window_ && display_ && window_alive_unlocked()) {
    auto* dpy = static_cast<Display*>(display_);
    Window child = 0;
    int ox = 0, oy = 0;
    if (XTranslateCoordinates(dpy, window_, DefaultRootWindow(dpy), 0, 0, &ox, &oy, &child)) {
      int cx = 0, cy = 0;
      client_cursor(&cx, &cy);
      *root_x = ox + cx;
      *root_y = oy + cy;
      return true;
    }
  }
  if (screen_input_ && have_place_) {
    const int tw = xtest_w_ > 0 ? xtest_w_ : width_;
    const int th = xtest_h_ > 0 ? xtest_h_ : height_;
    if (width_ > 1 && tw > 1 && tw != width_) {
      *root_x = place_x_ + static_cast<int>(cursor_x_ * static_cast<float>(tw - 1) /
                                                static_cast<float>(width_ - 1) +
                                            0.5f);
    } else {
      *root_x = place_x_ + static_cast<int>(cursor_x_);
    }
    if (height_ > 1 && th > 1 && th != height_) {
      *root_y = place_y_ + static_cast<int>(cursor_y_ * static_cast<float>(th - 1) /
                                                static_cast<float>(height_ - 1) +
                                            0.5f);
    } else {
      *root_y = place_y_ + static_cast<int>(cursor_y_);
    }
    return true;
  }
  return false;
}

bool ChromiumYtX11::warp_cursor() {
  if (!ensure_display()) return false;
  auto* dpy = static_cast<Display*>(display_);
  int rx = 0, ry = 0;
  const bool have_root = root_coords(&rx, &ry);
  if (have_root) {
    XTestFakeMotionEvent(dpy, -1, rx, ry, CurrentTime);
  }
  // XWarpPointer into the XWayland Chrome surface is what actually moves the
  // Wayland seat pointer (Embedded ScreenCast cursor). Map cfg-space cursor
  // across the full client so range matches the Virtual / configured size.
  if (window_ && window_alive_unlocked()) {
    int cx = 0, cy = 0;
    client_cursor(&cx, &cy);
    XWarpPointer(dpy, None, window_, 0, 0, 0, 0, cx, cy);
    XFlush(dpy);
    return true;
  }
  if (have_root) {
    XFlush(dpy);
    return true;
  }
  return false;
}

bool ChromiumYtX11::send_motion_event() {
  if (!display_ || !window_ || !window_alive_unlocked()) return false;
  auto* dpy = static_cast<Display*>(display_);
  int rx = 0, ry = 0;
  (void)root_coords(&rx, &ry);
  int cx = 0, cy = 0;
  client_cursor(&cx, &cy);
  XMotionEvent ev{};
  ev.type = MotionNotify;
  ev.display = dpy;
  ev.window = window_;
  ev.root = DefaultRootWindow(dpy);
  ev.subwindow = None;
  ev.time = CurrentTime;
  ev.x = cx;
  ev.y = cy;
  ev.x_root = rx;
  ev.y_root = ry;
  ev.state = 0;
  ev.is_hint = NotifyNormal;
  ev.same_screen = True;
  XErrorHandler prev = XSetErrorHandler([](Display*, XErrorEvent*) -> int { return 0; });
  XSendEvent(dpy, window_, True, PointerMotionMask, reinterpret_cast<XEvent*>(&ev));
  XFlush(dpy);
  XSetErrorHandler(prev);
  return true;
}

bool ChromiumYtX11::focus_window() {
  if (!display_ || !window_) return false;
  auto* dpy = static_cast<Display*>(display_);
  XErrorHandler prev = XSetErrorHandler([](Display*, XErrorEvent*) -> int { return 0; });
  XMapRaised(dpy, window_);
  // EWMH activate — more reliable than XSetInputFocus alone under KWin/XWayland.
  Atom net_active = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
  if (net_active != None) {
    XClientMessageEvent ev{};
    ev.type = ClientMessage;
    ev.window = window_;
    ev.message_type = net_active;
    ev.format = 32;
    ev.data.l[0] = 2;  // pager/application
    ev.data.l[1] = static_cast<long>(CurrentTime);
    ev.data.l[2] = 0;
    XSendEvent(dpy, DefaultRootWindow(dpy), False,
               SubstructureRedirectMask | SubstructureNotifyMask, reinterpret_cast<XEvent*>(&ev));
  }
  XSetInputFocus(dpy, window_, RevertToParent, CurrentTime);
  XSync(dpy, False);
  XSetErrorHandler(prev);
  return true;
}

bool ChromiumYtX11::click() {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  if (eis_ && eis_->ready()) {
    double ax = 0, ay = 0;
    eis_abs_from_cursor(&ax, &ay);
    eis_->warp(ax, ay);
    eis_->click_left();
    VRP_LOG("live-x11: click at %.0f,%.0f → eis %.0f,%.0f", static_cast<double>(cursor_x_),
            static_cast<double>(cursor_y_), ax, ay);
    return true;
  }
  if (!ensure_inject_target()) return false;
  if (!ensure_display()) return false;
  auto* dpy = static_cast<Display*>(display_);
  if (window_) (void)focus_window();
  if (!warp_cursor()) return false;

  int rx = 0, ry = 0;
  (void)root_coords(&rx, &ry);

  if (screen_input_) {
    // Same seat position as Embedded DE cursor — one path only.
    XTestFakeButtonEvent(dpy, 1, True, CurrentTime);
    XTestFakeButtonEvent(dpy, 1, False, CurrentTime);
  } else {
    (void)send_motion_event();
    int cx = 0, cy = 0;
    client_cursor(&cx, &cy);
    if (window_) {
      auto send_btn = [&](int type, unsigned button) {
        XButtonEvent ev{};
        ev.type = type;
        ev.display = dpy;
        ev.window = window_;
        ev.root = DefaultRootWindow(dpy);
        ev.subwindow = None;
        ev.time = CurrentTime;
        ev.x = cx;
        ev.y = cy;
        ev.x_root = rx;
        ev.y_root = ry;
        ev.state = (type == ButtonPress) ? Button1Mask : 0;
        ev.button = button;
        ev.same_screen = True;
        const long mask = (type == ButtonPress) ? ButtonPressMask : ButtonReleaseMask;
        XSendEvent(dpy, window_, True, mask, reinterpret_cast<XEvent*>(&ev));
      };
      send_btn(ButtonPress, Button1);
      send_btn(ButtonRelease, Button1);
    }
    XTestFakeButtonEvent(dpy, 1, True, CurrentTime);
    XTestFakeButtonEvent(dpy, 1, False, CurrentTime);
  }
  XFlush(dpy);
  VRP_LOG("live-x11: click at %.0f,%.0f (root %d,%d)%s", static_cast<double>(cursor_x_),
          static_cast<double>(cursor_y_), rx, ry, screen_input_ ? " [monitor]" : "");
  return true;
}

bool ChromiumYtX11::scroll(float dy) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  if (std::fabs(dy) < 1e-4f) return false;

  // Accumulate fractional notches so slow stick motion still ticks the wheel.
  static float accum = 0.f;
  accum += dy;
  int notches = 0;
  while (accum >= 1.f) {
    ++notches;
    accum -= 1.f;
  }
  while (accum <= -1.f) {
    --notches;
    accum += 1.f;
  }
  if (notches == 0) return true;

  if (eis_ && eis_->ready()) {
    double ax = 0, ay = 0;
    eis_abs_from_cursor(&ax, &ay);
    eis_->warp(ax, ay);
    eis_->scroll_discrete(notches);
    return true;
  }

  if (!ensure_inject_target()) return false;
  if (!ensure_display()) return false;
  auto* dpy = static_cast<Display*>(display_);
  if (window_) (void)focus_window();
  if (!warp_cursor()) return false;
  if (!screen_input_) (void)send_motion_event();

  // MONITOR: XTest only at the Embedded cursor seat position.
  for (int i = 0; i < std::abs(notches); ++i) {
    const unsigned btn = notches > 0 ? 4u : 5u;
    if (!screen_input_ && window_ && window_alive_unlocked()) {
      int rx = 0, ry = 0, cx = 0, cy = 0;
      (void)root_coords(&rx, &ry);
      client_cursor(&cx, &cy);
      auto send_btn = [&](int type) {
        XButtonEvent ev{};
        ev.type = type;
        ev.display = dpy;
        ev.window = window_;
        ev.root = DefaultRootWindow(dpy);
        ev.subwindow = None;
        ev.time = CurrentTime;
        ev.x = cx;
        ev.y = cy;
        ev.x_root = rx;
        ev.y_root = ry;
        ev.state = 0;
        ev.button = btn;
        ev.same_screen = True;
        const long mask = (type == ButtonPress) ? ButtonPressMask : ButtonReleaseMask;
        XSendEvent(dpy, window_, True, mask, reinterpret_cast<XEvent*>(&ev));
      };
      send_btn(ButtonPress);
      send_btn(ButtonRelease);
    }
    XTestFakeButtonEvent(dpy, btn, True, CurrentTime);
    XTestFakeButtonEvent(dpy, btn, False, CurrentTime);
  }
  XFlush(dpy);
  return true;
}

bool ChromiumYtX11::key_back() {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  // EIS keysyms need a keymap; always use XTest for Alt+Left.
  if (!(window_ != 0 || screen_input_ || (eis_ && eis_->ready()))) return false;
  if (!ensure_display()) return false;
  auto* dpy = static_cast<Display*>(display_);
  if (window_) (void)focus_window();
  else (void)warp_cursor();  // focus follows pointer onto the virtual output
  const KeyCode left = XKeysymToKeycode(dpy, XK_Left);
  const KeyCode alt_l = XKeysymToKeycode(dpy, XK_Alt_L);
  if (left == 0) return false;
  // Alt+Left — Chromium / Chrome history back.
  if (alt_l != 0) XTestFakeKeyEvent(dpy, alt_l, True, CurrentTime);
  XTestFakeKeyEvent(dpy, left, True, CurrentTime);
  XTestFakeKeyEvent(dpy, left, False, CurrentTime);
  if (alt_l != 0) XTestFakeKeyEvent(dpy, alt_l, False, CurrentTime);
  XFlush(dpy);
  VRP_LOG("live-x11: browser Back (Alt+Left)");
  return true;
}

bool ChromiumYtX11::close_window() {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  // Prefer the window we already bound (from prepare). Re-scanning can latch onto
  // the user's normal Chrome tab and close that instead.
  if (!window_ && !find_window()) return false;
  if (!display_ || !window_) return false;
  auto* dpy = static_cast<Display*>(display_);
  const Window w = window_;
  window_ = 0;  // clear first — KWin may already have destroyed this xid

  // Trap BadWindow: compositor often closes before we send WM_DELETE; the default
  // X error path has been observed to cascade into process SEGV on stop.
  XErrorHandler prev = XSetErrorHandler([](Display*, XErrorEvent*) -> int { return 0; });
  XWindowAttributes attrs{};
  if (!XGetWindowAttributes(dpy, w, &attrs)) {
    XSync(dpy, False);
    XSetErrorHandler(prev);
    VRP_LOG("live-x11: close skipped — window 0x%lx already gone", w);
    return false;
  }
  Atom wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
  Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
  XEvent ev{};
  ev.xclient.type = ClientMessage;
  ev.xclient.window = w;
  ev.xclient.message_type = wm_protocols;
  ev.xclient.format = 32;
  ev.xclient.data.l[0] = static_cast<long>(wm_delete);
  ev.xclient.data.l[1] = CurrentTime;
  XSendEvent(dpy, w, False, NoEventMask, &ev);
  XSync(dpy, False);
  XSetErrorHandler(prev);
  VRP_LOG("live-x11: WM_DELETE_WINDOW 0x%lx", w);
  return true;
}

}  // namespace vrp
