#pragma once

#include <QString>
#include <QStringList>

namespace vrp {

/**
 * Place / close live Chrome --app windows via the compositor (KWin), not Chrome.
 *
 * Plasma provides:
 *  - Window Rules (Force Screen + Fullscreen) — applied at map time
 *  - Scripting (windowAdded → sendClientToScreen)
 *
 * Chrome cannot open on a named Wayland output; the compositor must do it.
 */
class KwinWindowPlacer {
 public:
  /**
   * Snapshot internalIds of Chrome-family windows (call before launching).
   * Broader than class-only so a brand-new --app window (empty caption) is
   * still distinguished from already-open Chrome windows.
   */
  static QStringList list_candidate_ids(QString* error_out = nullptr);

  /**
   * Install a temporary KWin Window Rule (Force Screen + Fullscreen) and arm a
   * windowAdded hook. Call before ChromiumYtLauncher::start().
   * The rule is the compositor-native path; the hook catches Chrome windows
   * whose --class was ignored by a running singleton (common on Wayland).
   */
  static bool arm_place_on_open(const QString& output_substr, const QStringList& exclude_ids,
                                QString* error_out = nullptr);
  /** Non-blocking: true once placement reported success. */
  static bool poll_armed_place(QString* out_window_id, QString* error_out = nullptr);
  /**
   * Unload place script. If keep_fullscreen_rule, leave a Force fullscreen /
   * noborder / screen Window Rule for the session; otherwise remove it.
   */
  static void disarm_place_on_open(bool keep_fullscreen_rule = false);

  /**
   * One-shot fallback: find and move an already-mapped window.
   * Prefer arm_place_on_open for MONITOR capture.
   */
  static bool move_youtube_to_output(const QString& output_substr, const QStringList& exclude_ids,
                                     QString* out_window_id, QString* error_out = nullptr,
                                     bool* moved_ok = nullptr);

  static bool close_window_by_id(const QString& internal_id, QString* error_out = nullptr);
  static bool close_youtube_window(QString* error_out = nullptr);
  /** Raise + activate the live Chrome window so pointer/scroll reach it. */
  static bool activate_youtube_window(const QString& internal_id = {},
                                      QString* error_out = nullptr);
  /**
   * Close Chrome-family windows whose internalId is not in keep_ids.
   * Used when place fails so a default-profile --new-window does not linger
   * on the primary (class is often google-chrome, not monaSphereYT).
   */
  static bool close_chrome_except(const QStringList& keep_ids, QString* error_out = nullptr);

  /**
   * Force xdg-desktop-portal / ScreenCast dialogs onto the primary (non-Virtual)
   * output. Call before interactive ScreenCast Start; disarm after started/failed.
   */
  static bool arm_portal_dialog_on_primary(QString* error_out = nullptr);
  static void disarm_portal_dialog_on_primary();
  /** One-shot: move any matching portal dialogs to primary now. */
  static bool move_portal_dialogs_to_primary(QString* error_out = nullptr);

  /** Seat cursor in KWin logical desktop coords (workspace.cursorPos). */
  static bool query_cursor_pos(double* x, double* y, QString* error_out = nullptr);

  // Compatibility wrappers
  static bool move_class_to_output(const QString& class_substr, const QString& output_substr,
                                   QString* error_out = nullptr);
  static bool move_to_output(const QString& class_substr, const QString& output_substr, qint64 pid,
                             QString* error_out = nullptr);
  static bool move_youtube_to_output(const QString& output_substr, QString* error_out = nullptr,
                                     bool* moved_ok = nullptr);
};

}  // namespace vrp
