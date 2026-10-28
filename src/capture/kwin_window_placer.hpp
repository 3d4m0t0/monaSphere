#pragma once

#include <QList>
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
   * Snapshot internalIds of all normal (non-dock / non-desktop) windows.
   * Used before launching a registered desktop GUI ([アプリ]).
   */
  static QStringList list_all_window_ids(QString* error_out = nullptr);

  /**
   * Install a temporary KWin Window Rule (Force Screen + Fullscreen) and arm a
   * windowAdded hook. Call before ChromiumYtLauncher::start().
   * The rule is the compositor-native path; the hook catches Chrome windows
   * whose --class was ignored by a running singleton (common on Wayland).
   */
  /**
   * @param screen_index  ≥0: use this KWin screen index for the Window Rule
   *                      (from VirtualDisplayManager). -1: resolve by name.
   */
  static bool arm_place_on_open(const QString& output_substr, const QStringList& exclude_ids,
                                QString* error_out = nullptr, int screen_index = -1);

  /**
   * Arm windowAdded / scan for desktop GUI windows. Match prefer_pids and/or
   * want_names (resourceClass / resourceName substrings, e.g. "retroarch").
   * Never place arbitrary new windows (that pulled Dolphin onto Virtual).
   */
  static bool arm_place_new_window(const QString& output_substr, const QStringList& exclude_ids,
                                   const QList<qint64>& prefer_pids = {},
                                   const QStringList& want_names = {},
                                   QString* error_out = nullptr);

  /**
   * One-shot: move matching windows (pid and/or want_names) onto the output.
   */
  static bool move_new_window_to_output(const QString& output_substr, const QStringList& exclude_ids,
                                        const QList<qint64>& prefer_pids = {},
                                        const QStringList& want_names = {},
                                        QString* out_window_id = nullptr,
                                        QString* error_out = nullptr, bool* moved_ok = nullptr);

  /** Root pid + current /proc children (one level of descendants, iterative). */
  static QList<qint64> pid_tree(qint64 root_pid);

  /**
   * Read resourceClass (and optional resourceName) for a KWin internalId.
   * Used to install a session Window Rule after the first desktop-app window maps.
   */
  static bool query_window_wmclass(const QString& internal_id, QString* resource_class,
                                   QString* resource_name = nullptr, QString* error_out = nullptr);

  /**
   * KWin workspace.screens[] index for an output name substring
   * (e.g. Virtual-monaSphere-x3). -1 if missing.
   */
  static int query_output_index(const QString& output_substr, QString* error_out = nullptr);

  /**
   * Force Screen only for wmclass on Virtual for the [アプリ] session.
   * Call AFTER the window maps with Exact + wmclasscomplete (real class string).
   * Does not Force fullscreen — apps like RetroArch own that.
   * Prefer the screen_index overload when VirtualDisplayManager already resolved it.
   */
  static bool install_desktop_window_rule(const QString& wmclass, int screen_index,
                                          QString* error_out = nullptr);
  static bool install_desktop_window_rule(const QString& wmclass, const QString& output_substr,
                                          QString* error_out = nullptr);
  static void remove_desktop_window_rule();
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
