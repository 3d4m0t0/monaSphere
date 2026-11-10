#pragma once

#include <QString>

namespace vrp {

/**
 * GNOME: keep xdg-desktop-portal / remote-control dialogs off Meta-* (RecordVirtual).
 *
 * KDE uses KWin scripts (arm_portal_dialog_on_primary). On GNOME the VIRTUAL Start
 * creates Meta-* as the newest output, so the permission UI often lands there and
 * is captured into the HMD. Move matching X11/XWayland windows onto the primary.
 */
struct GnomePortalDialog {
  /**
   * Scan the X11 tree; move portal / remote-control dialogs whose origin lies
   * on a Meta-* (or @virtual_x,y,w,h) CRTC onto the primary output.
   * @return number of windows moved (0 = none found / nothing to do).
   */
  static int evacuateToPrimary(int virtual_x, int virtual_y, int virtual_w, int virtual_h,
                               QString* detail = nullptr);

  /**
   * GNOME Shell: move WM_CLASS windows onto the Meta-* / rightmost monitor and
   * fullscreen them there. X11 coords alone often leave XWayland chrome invisible
   * to RecordVirtual even when root@ matches the Meta CRTC.
   * @return true if Shell.Eval ran and reported moved>0 (Eval may be blocked).
   */
  static bool shellMoveClassToMeta(const QString& wm_class, QString* detail = nullptr);
};

}  // namespace vrp
