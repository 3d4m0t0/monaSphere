#pragma once

#include "capture/kde_virtual_display.hpp"
#include "capture/mutter_virtual_screencast.hpp"

#include <QMap>
#include <QObject>
#include <QString>

#include <functional>
#include <mutex>

namespace vrp {

/**
 * Owns the live Virtual MONITOR topology for both desktops:
 *  - KDE: krfb-virtualmonitor → Virtual-monaSphere (+ ScreenCast restore tokens)
 *  - GNOME: Mutter RecordVirtual(is-platform) → Meta-* (stream-tied MONITOR)
 *
 * ## Root causes this design absorbs (2026-11 Meta live)
 *
 * 1. **SHM pool recycle / 前後ブレ** — RecordVirtual Hidden cursor only emits on
 *    stage damage; PW then republishes the same ≤8 SHM buffers. Mitigations live
 *    in the capture/display path (Embedded cursor, sticky content_fp ban, SHM
 *    staging). The manager does not own those filters.
 *
 * 2. **Cursor dead until Meta "glitches"** — Mutter ConnectToEIS runs *before*
 *    PipeWire negotiates size, so Meta-* does not exist when libei snapshots
 *    regions. Warps land on a stale primary region until Meta appears and the
 *    device is re-added. Session must `EisPointer::repick_regions()` after Meta
 *    is listed, then clear the spinner (see YouTubeSession gnome_live_ready_).
 *
 * 3. **Early spinner clear** — first PW frame is Meta wallpaper; KDE's first
 *    frame is already the browser. GNOME must keep busy/loading until Chrome is
 *    on Meta and EIS is rebound (KDE-shaped ensure → place → first usable frame).
 *
 * 4. **Split ownership** — historically YouTubeSession owned MutterVirtual while
 *    the manager only knew krfb. Host `matches()`/`destroy()` then lied on GNOME
 *    (always 「削除中」, no reuse). Meta create/destroy now go through this
 *    manager like KDE Virtual.
 *
 * Capture (PipeWire consumer / Chrome place) stays in YouTubeSession — different
 * lifetime from display topology. On GNOME the MONITOR *is* the RecordVirtual
 * session, so destroy() Session.Stop after clients/PW are stopped (session
 * cleanupStreaming order).
 *
 * ## Why GNOME Live start often feels faster than KDE
 *
 * KDE ensure() waits on: kscreen describe/enable polls (≤10s, 150ms ticks),
 * optional recreate path (wait_gone ≤8s + 400ms plasmashell settle), then
 * Chrome place *before* ScreenCast, then portal Start (token/restore). GNOME
 * ensureGnomeMeta is one Mutter D-Bus RecordVirtual + ConnectToEIS (no kscreen
 * wait_ready); Chrome is deferred until after PW — fewer serial waits on the
 * critical path to first HMD spinner, though Meta wallpaper is held off-HMD
 * until Chrome places.
 */
class VirtualDisplayManager : public QObject {
  Q_OBJECT
 public:
  struct Spec {
    QString name;  // krfb --name (monaSphere); GNOME uses Meta hint
    int width = 1280;
    int height = 720;
    float scale = 2.f;
    /** 1–3; 0 → derive from name. */
    int span = 0;
  };

  enum class Action {
    Reused,
    Created,
    Recreated,
  };

  enum class Backend {
    None,
    KdeKrfb,
    GnomeMutter,
  };

  struct Result {
    bool ok = false;
    Action action = Action::Created;
    Backend backend = Backend::None;
    int span = 1;
    QString output_name;
    /** KWin workspace.screens[] index; -1 if unresolved / GNOME. */
    int screen_index = -1;
    /** ScreenCast restore token for this span (KDE; empty on GNOME). */
    QString restore_token;
    /** GNOME Mutter: PipeWire node + EIS fd from RecordVirtual / RD. */
    uint32_t node_id = 0;
    int eis_fd = -1;  // ownership transfers to caller (EisPointer::start_from_fd)
    QString mapping_id;
    QString error;
  };

  explicit VirtualDisplayManager(QObject* parent = nullptr);

  /** KDE Virtual policy (reuse / recreate / create). */
  Result ensure(const Spec& want);
  /**
   * GNOME Meta-* via Mutter RecordVirtual(is-platform, cursor=embedded).
   * Blocks (local QEventLoop) until PipeWireStreamAdded + ConnectToEIS succeed.
   * Meta output itself appears only after the session starts the PW consumer.
   * Emits busyStatus("Virtual 作成中") + progress like KDE ensure().
   */
  Result ensureGnomeMeta(const Spec& want);
  /**
   * Tear down Virtual / Meta. Tokens kept (KDE).
   * GNOME: Mutter Session.Stop (removes Meta-*). Call only after clients + PW
   * consumer are stopped (YouTubeSession::cleanupStreaming order).
   */
  void destroy();

  bool alive() const;
  bool matches(const Spec& want) const;
  /** False while a GNOME Meta RecordVirtual session is open (stream-tied). */
  bool reusable() const;

  Backend backend() const { return backend_; }
  bool gnome_meta_alive() const { return gnome_meta_alive_; }
  /** Last successful ensureGnomeMeta result (eis_fd may already be taken). */
  const Result& last_gnome_result() const { return last_gnome_; }

  MutterVirtualScreencast* mutter() { return mutter_; }
  const MutterVirtualScreencast* mutter() const { return mutter_; }

  /** Expected / last matched compositor output name. */
  QString output_name() const;
  /** krfb --name (monaSphere) or Meta hint. */
  QString name() const;
  int screen_index() const { return screen_index_; }
  int width() const;
  int height() const;
  float scale() const;
  int active_span() const { return active_span_; }

  bool refresh_identity(QString* error_out = nullptr);

  static int spanFromName(const QString& name_or_output);
  static Spec normalize(Spec want);

  QString token(int span) const;
  void loadToken(int span, const QString& token);
  void setToken(int span, const QString& token);
  void clearToken(int span);
  void clearAllTokens();

  KdeVirtualDisplay* display() { return vdisp_; }
  const KdeVirtualDisplay* display() const { return vdisp_; }

  void set_progress_sink(std::function<void(int)> sink);

 signals:
  void logMessage(const QString& msg);
  void busyStatus(const QString& status);
  void tokenChanged(int span, const QString& token);

 private:
  static bool spec_equal(const Spec& a, int w, int h, float sc, const QString& name);
  bool wait_ready(const Spec& want, QString* matched_out, int* screen_index_out);
  bool wait_gone(int timeout_ms, int gone_budget);
  void begin_progress();
  void add_progress(int delta);
  void commit_identity(const Spec& want, const QString& matched, Result* r, int known_index,
                       bool index_checked);
  void stopGnomeMetaUnlocked();

  KdeVirtualDisplay* vdisp_ = nullptr;
  MutterVirtualScreencast* mutter_ = nullptr;
  Backend backend_ = Backend::None;
  bool gnome_meta_alive_ = false;
  Result last_gnome_{};
  Spec current_{};
  bool have_current_ = false;
  int active_span_ = 1;
  QString output_name_;
  int screen_index_ = -1;
  QMap<int, QString> tokens_;
  std::function<void(int)> progress_sink_;
  std::mutex progress_mu_;
  int progress_ = -1;
};

}  // namespace vrp
