#pragma once

#include <QRect>
#include <QString>

namespace vrp {

/**
 * GNOME Wayland managed virtual MONITOR helpers.
 *
 * Unlike Plasma (krfb-virtualmonitor + later portal MONITOR share), Mutter creates
 * the virtual output as part of ScreenCast RecordVirtual(is-platform).
 *
 * Lifecycle is owned by VirtualDisplayManager::ensureGnomeMeta / destroy (same
 * busyStatus "Virtual 作成中/削除中" path as KDE). YouTubeSession only runs the
 * PW consumer, EIS rebind after Meta appears, and Chrome place.
 *
 * Critical ordering (root cause of "cursor dead until Meta glitches"):
 *   ConnectToEIS → (regions snapshotted without Meta) → PW negotiate → Meta-*
 *   → EisPointer::repick_regions() → Chrome. Skipping repick warps the seat onto
 *   the primary monitor while Embedded cursor in the Meta stream stays frozen.
 */
struct GnomeVirtualDisplay {
  /** Portal AvailableSourceTypes includes VIRTUAL (0x4). */
  static bool portalSupportsVirtual();
  /** Mutter ScreenCast well-known name is on the session bus. */
  static bool mutterScreenCastAvailable();
  /** True when managed live_virtual=1 can run on this GNOME session. */
  static bool managedVirtualAvailable();
  /** Connector prefix Mutter uses for RecordVirtual outputs (Meta-0, …). */
  static QString outputNameHint();

  /**
   * Optional DisplayConfig lookup (currently unused — QDBus demarshal aborted on
   * some Mutter builds). Live placement uses portal EIS region instead.
   */
  static bool queryVirtualRect(QRect* geo, QString* matched_connector = nullptr);
};

}  // namespace vrp
