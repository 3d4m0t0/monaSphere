#pragma once

#include "capture/kde_virtual_display.hpp"

#include <QMap>
#include <QObject>
#include <QString>

#include <functional>
#include <mutex>

namespace vrp {

/**
 * Owns monaSphere Virtual (krfb) + per-span ScreenCast restore tokens +
 * the live KWin identity (output name + screen index) for placement rules.
 *
 * Virtual policy (ensure):
 *  - up + same name/resolution/scale → reuse
 *  - up + different → delete, then create
 *  - none → create
 *
 * Identity (after ensure / refresh):
 *  - output_name: always Virtual-monaSphere (span only changes WxH)
 *  - screen_index: workspace.screens[] index for Window Rule Force screen
 *
 * Token policy:
 *  - one restore token for Virtual-monaSphere (conf: live_restore_token)
 *  - span x1–x3 share that token; resolution changes do not need re-auth
 *  - setToken() after portal Start; clearToken() on reject / wrong-stream
 *  - Host persists via tokenChanged (manager is in-memory source of truth)
 *
 * Capture (portal / PipeWire / Chrome place) stays in YouTubeSession — different
 * lifetime (stream session) from display topology + auth grants.
 */
class VirtualDisplayManager : public QObject {
  Q_OBJECT
 public:
  struct Spec {
    QString name;  // krfb --name (monaSphere; span ignored for naming)
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

  struct Result {
    bool ok = false;
    Action action = Action::Created;
    int span = 1;
    QString output_name;
    /** KWin workspace.screens[] index; -1 if unresolved. */
    int screen_index = -1;
    /** ScreenCast restore token for this span (may be empty → interactive). */
    QString restore_token;
    QString error;
  };

  explicit VirtualDisplayManager(QObject* parent = nullptr);

  /** Apply Virtual policy. Fills Result.restore_token from the span store. */
  Result ensure(const Spec& want);
  /** Tear down Virtual only — tokens kept. Clears name/index. */
  void destroy();

  bool alive() const;
  bool matches(const Spec& want) const;

  /** Expected / last matched compositor output name (Virtual-monaSphere). */
  QString output_name() const;
  /** krfb --name (monaSphere). */
  QString name() const;
  /**
   * KWin screen index for Window Rules. Valid after successful ensure/refresh.
   * -1 if Virtual is down or index could not be resolved.
   */
  int screen_index() const { return screen_index_; }
  int width() const;
  int height() const;
  float scale() const;
  int active_span() const { return active_span_; }

  /**
   * Re-resolve output_name (kscreen) + screen_index (KWin) for the current Virtual.
   * Call before install Window Rule if topology may have changed.
   */
  bool refresh_identity(QString* error_out = nullptr);

  /** Legacy monaSphere-xN → 1–3; plain Virtual-monaSphere → 1. */
  static int spanFromName(const QString& name_or_output);
  static Spec normalize(Spec want);

  // --- ScreenCast restore tokens (per span) ---
  QString token(int span) const;
  /** Load from conf at startup (no tokenChanged). */
  void loadToken(int span, const QString& token);
  /** Portal returned a token — emit tokenChanged for conf write. */
  void setToken(int span, const QString& token);
  /** Drop one span (reject / wrong MONITOR). Other spans kept. */
  void clearToken(int span);
  void clearAllTokens();

  KdeVirtualDisplay* display() { return vdisp_; }
  const KdeVirtualDisplay* display() const { return vdisp_; }

  /**
   * Thread-safe. Called with 0–100 as each Virtual step's completion is confirmed.
   * -1 is not used here; the sink owner clears the readout when busy status clears.
   */
  void set_progress_sink(std::function<void(int)> sink);

 signals:
  void logMessage(const QString& msg);
  void busyStatus(const QString& status);
  /** Persist live_restore_token (span arg kept for API; always the shared grant). */
  void tokenChanged(int span, const QString& token);

 private:
  static bool spec_equal(const Spec& a, int w, int h, float sc, const QString& name);
  /**
   * Poll until mode+rect match. screen_index_out, when non-null, receives a KWin
   * index queried in parallel once the output name is known (-1 if that attempt
   * missed and the caller must query again).
   */
  bool wait_ready(const Spec& want, QString* matched_out, int* screen_index_out);
  /** gone_budget is added across confirmed kscreen polls, remainder when the wait ends. */
  bool wait_gone(int timeout_ms, int gone_budget);
  void begin_progress();
  void add_progress(int delta);
  /**
   * Set cached name + KWin index. index_checked means known_index is the result
   * of a query that ran after the output was listed — do not query again.
   */
  void commit_identity(const Spec& want, const QString& matched, Result* r, int known_index,
                       bool index_checked);

  KdeVirtualDisplay* vdisp_ = nullptr;
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
