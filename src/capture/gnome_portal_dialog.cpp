#include "capture/gnome_portal_dialog.hpp"

#include "common.hpp"

#include <QProcess>
#include <QString>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

namespace vrp {
namespace {

std::string to_lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

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

bool is_portal_dialog(Display* dpy, Window w) {
  std::string name, cls;
  (void)get_wm_class(dpy, w, &name, &cls);
  const std::string title = to_lower(window_title(dpy, w));
  const std::string a = to_lower(name);
  const std::string b = to_lower(cls);

  if (a.find("xdg-desktop-portal") != std::string::npos ||
      b.find("xdg-desktop-portal") != std::string::npos)
    return true;
  if (a.find("xdg_desktop_portal") != std::string::npos ||
      b.find("xdg_desktop_portal") != std::string::npos)
    return true;
  // GNOME Shell / portal GTK dialogs often use generic class; match captions.
  if (title.find("remote") != std::string::npos &&
      (title.find("control") != std::string::npos || title.find("desktop") != std::string::npos))
    return true;
  if (title.find("screen sharing") != std::string::npos ||
      title.find("share this") != std::string::npos ||
      title.find("select what to share") != std::string::npos ||
      title.find("choose what to share") != std::string::npos)
    return true;
  if (title.find("画面共有") != std::string::npos || title.find("画面を共有") != std::string::npos ||
      title.find("共有する内容") != std::string::npos ||
      title.find("共有する画面") != std::string::npos ||
      title.find("リモート制御") != std::string::npos ||
      title.find("リモートデスクトップ") != std::string::npos)
    return true;
  // gnome-shell styled grant dialogs
  if ((a.find("gnome-shell") != std::string::npos || b.find("gnome-shell") != std::string::npos) &&
      (title.find("share") != std::string::npos || title.find("remote") != std::string::npos ||
       title.find("共有") != std::string::npos || title.find("リモート") != std::string::npos))
    return true;
  return false;
}

bool root_origin(Display* dpy, Window w, int* rx, int* ry) {
  Window child = 0;
  return XTranslateCoordinates(dpy, w, DefaultRootWindow(dpy), 0, 0, rx, ry, &child) != 0;
}

bool point_in_rect(int x, int y, int rx, int ry, int rw, int rh) {
  return x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

bool find_meta_crtc(Display* dpy, int* x, int* y, int* w, int* h) {
  int evt = 0, err = 0;
  if (!XRRQueryExtension(dpy, &evt, &err)) return false;
  Window root = DefaultRootWindow(dpy);
  XRRScreenResources* res = XRRGetScreenResourcesCurrent(dpy, root);
  if (!res) return false;
  bool found = false;
  for (int i = 0; i < res->noutput; ++i) {
    XRROutputInfo* oi = XRRGetOutputInfo(dpy, res, res->outputs[i]);
    if (!oi) continue;
    if (oi->connection == RR_Connected && oi->crtc != None && oi->name) {
      const std::string n = to_lower(oi->name);
      if (n.find("meta") != std::string::npos || n.find("virtual") != std::string::npos) {
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

bool find_primary_crtc(Display* dpy, int* x, int* y, int* w, int* h) {
  int evt = 0, err = 0;
  if (!XRRQueryExtension(dpy, &evt, &err)) return false;
  Window root = DefaultRootWindow(dpy);
  RROutput primary = XRRGetOutputPrimary(dpy, root);
  XRRScreenResources* res = XRRGetScreenResourcesCurrent(dpy, root);
  if (!res) return false;
  bool found = false;
  auto take = [&](RRCrtc crtc) {
    if (crtc == None) return;
    XRRCrtcInfo* ci = XRRGetCrtcInfo(dpy, res, crtc);
    if (ci && ci->width > 0 && ci->height > 0) {
      *x = ci->x;
      *y = ci->y;
      *w = static_cast<int>(ci->width);
      *h = static_cast<int>(ci->height);
      found = true;
    }
    if (ci) XRRFreeCrtcInfo(ci);
  };
  if (primary != None) {
    for (int i = 0; i < res->noutput; ++i) {
      if (res->outputs[i] != primary) continue;
      XRROutputInfo* oi = XRRGetOutputInfo(dpy, res, res->outputs[i]);
      if (oi) {
        take(oi->crtc);
        XRRFreeOutputInfo(oi);
      }
      break;
    }
  }
  if (!found) {
    for (int i = 0; i < res->noutput; ++i) {
      XRROutputInfo* oi = XRRGetOutputInfo(dpy, res, res->outputs[i]);
      if (!oi) continue;
      if (oi->connection == RR_Connected && oi->crtc != None && oi->name) {
        const std::string n = to_lower(oi->name);
        if (n.find("meta") == std::string::npos && n.find("virtual") == std::string::npos) {
          take(oi->crtc);
        }
      }
      XRRFreeOutputInfo(oi);
      if (found) break;
    }
  }
  XRRFreeScreenResources(res);
  return found;
}

int evacuate_tree(Display* dpy, Window root, int vx, int vy, int vw, int vh, int px, int py,
                  QString* detail) {
  Window rr = 0, parent = 0;
  Window* kids = nullptr;
  unsigned n = 0;
  if (!XQueryTree(dpy, root, &rr, &parent, &kids, &n) || !kids) return 0;
  int moved = 0;
  for (unsigned i = 0; i < n; ++i) {
    moved += evacuate_tree(dpy, kids[i], vx, vy, vw, vh, px, py, detail);
    if (!is_portal_dialog(dpy, kids[i])) continue;
    XWindowAttributes wa{};
    if (!XGetWindowAttributes(dpy, kids[i], &wa) || wa.map_state != IsViewable) continue;
    if (wa.width < 80 || wa.height < 60) continue;
    int ox = 0, oy = 0;
    if (!root_origin(dpy, kids[i], &ox, &oy)) continue;
    // On Meta / virtual CRTC (origin or center).
    const bool on_virt =
        point_in_rect(ox + 8, oy + 8, vx, vy, vw, vh) ||
        point_in_rect(ox + wa.width / 2, oy + wa.height / 2, vx, vy, vw, vh);
    if (!on_virt) continue;
    XMoveWindow(dpy, kids[i], px + 48, py + 48);
    XRaiseWindow(dpy, kids[i]);
    XFlush(dpy);
    ++moved;
    VRP_LOG("gnome-portal: moved dialog 0x%lx from %d,%d -> primary %d,%d",
            static_cast<unsigned long>(kids[i]), ox, oy, px + 48, py + 48);
    if (detail) {
      if (!detail->isEmpty()) *detail += QLatin1Char(' ');
      *detail += QStringLiteral("0x%1").arg(static_cast<qulonglong>(kids[i]), 0, 16);
    }
  }
  XFree(kids);
  return moved;
}

}  // namespace

int GnomePortalDialog::evacuateToPrimary(int virtual_x, int virtual_y, int virtual_w,
                                         int virtual_h, QString* detail) {
  Display* dpy = XOpenDisplay(nullptr);
  if (!dpy) {
    VRP_LOG("gnome-portal: XOpenDisplay failed");
    return 0;
  }

  int vx = virtual_x, vy = virtual_y, vw = virtual_w, vh = virtual_h;
  if (vw <= 0 || vh <= 0) {
    if (!find_meta_crtc(dpy, &vx, &vy, &vw, &vh)) {
      XCloseDisplay(dpy);
      return 0;
    }
  }

  int px = 0, py = 0, pw = 0, ph = 0;
  if (!find_primary_crtc(dpy, &px, &py, &pw, &ph)) {
    px = 0;
    py = 0;
  }

  // Avoid no-op when Meta is somehow primary (should not happen).
  if (vw > 0 && vh > 0 && px == vx && py == vy) {
    XCloseDisplay(dpy);
    return 0;
  }

  const int moved =
      evacuate_tree(dpy, DefaultRootWindow(dpy), vx, vy, vw, vh, px, py, detail);
  XCloseDisplay(dpy);
  if (moved > 0) {
    VRP_LOG("gnome-portal: evacuated %d dialog(s) off Meta/virtual @%d,%d %dx%d -> primary %d,%d",
            moved, vx, vy, vw, vh, px, py);
  }
  return moved;
}

bool GnomePortalDialog::shellMoveClassToMeta(const QString& wm_class, QString* detail) {
  if (wm_class.isEmpty()) return false;
  // Escape for single-quoted JS string.
  QString cls = wm_class;
  cls.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
  cls.replace(QLatin1Char('\''), QStringLiteral("\\'"));

  // Shell logical geometry (not Xrandr physical). Pick the rightmost non-primary
  // monitor — RecordVirtual Meta-* is typically that. Then move + fullscreen.
  const QString js = QStringLiteral(
                         "(() => {"
                         "  const Meta = imports.gi.Meta;"
                         "  const className = '%1';"
                         "  let mon = -1;"
                         "  let bestX = -1;"
                         "  const n = global.display.get_n_monitors();"
                         "  const primary = global.display.get_primary_monitor();"
                         "  for (let i = 0; i < n; i++) {"
                         "    if (n > 1 && i === primary) continue;"
                         "    const g = global.display.get_monitor_geometry(i);"
                         "    if (g.x > bestX) { bestX = g.x; mon = i; }"
                         "  }"
                         "  if (mon < 0) return 'meta-not-found n=' + n;"
                         "  let moved = 0;"
                         "  const actors = global.get_window_actors();"
                         "  for (let i = 0; i < actors.length; i++) {"
                         "    const w = actors[i].meta_window;"
                         "    const c = w.get_wm_class() || '';"
                         "    const inst = w.get_wm_class_instance() || '';"
                         "    if (c !== className && inst !== className) continue;"
                         "    try { w.unmake_fullscreen(); } catch (e) {}"
                         "    w.move_to_monitor(mon);"
                         "    const g = global.display.get_monitor_geometry(mon);"
                         "    w.move_resize_frame(true, g.x, g.y, g.width, g.height);"
                         "    try { w.make_fullscreen(); } catch (e) {"
                         "      w.maximize(Meta.MaximizeFlags.HORIZONTAL | Meta.MaximizeFlags.VERTICAL);"
                         "    }"
                         "    moved++;"
                         "  }"
                         "  return 'moved=' + moved + ' mon=' + mon + ' x=' + bestX + ' n=' + n;"
                         "})()")
                         .arg(cls);

  QProcess proc;
  proc.start(QStringLiteral("gdbus"),
             {QStringLiteral("call"), QStringLiteral("--session"), QStringLiteral("--dest"),
              QStringLiteral("org.gnome.Shell"), QStringLiteral("--object-path"),
              QStringLiteral("/org/gnome/Shell"), QStringLiteral("--method"),
              QStringLiteral("org.gnome.Shell.Eval"), js});
  if (!proc.waitForFinished(4000)) {
    proc.kill();
    if (detail) *detail = QStringLiteral("gdbus timeout");
    VRP_LOG("gnome-shell: Eval timeout moving %s", wm_class.toUtf8().constData());
    return false;
  }
  const QString out = QString::fromUtf8(proc.readAllStandardOutput()).trimmed() +
                      QString::fromUtf8(proc.readAllStandardError()).trimmed();
  if (detail) *detail = out;
  VRP_LOG("gnome-shell: move %s → Meta: %s", wm_class.toUtf8().constData(),
          out.toUtf8().constData());
  // gdbus prints: (true, 'moved=1 mon=1 …') or (false, '') when Eval disabled.
  const bool ok = out.contains(QLatin1String("true")) && out.contains(QLatin1String("moved=")) &&
                  !out.contains(QLatin1String("moved=0"));
  return ok;
}

}  // namespace vrp
