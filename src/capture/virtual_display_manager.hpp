#pragma once

#include "capture/kde_virtual_display.hpp"

#include <QMap>
#include <QObject>
#include <QString>

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
 *  - output_name: kscreen/KWin name (e.g. Virtual-monaSphere-x3)
 *  - screen_index: workspace.screens[] index for Window Rule Force screen
 *
 * Token policy:
 *  - one restore token per span x1–x3 (conf: live_restore_token[xN])
 *  - ensure() does not clear other spans' tokens
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
    QString name;  // krfb --name (monaSphere-xN)
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

  /** Expected / last matched compositor output name (Virtual-monaSphere-xN). */
  QString output_name() const;
  /** krfb --name (monaSphere-xN). */
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

  /** Derive span 1–3 from monaSphere-xN / Virtual-monaSphere-xN. */
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

 signals:
  void logMessage(const QString& msg);
  void busyStatus(const QString& status);
  /** Persist live_restore_token[xN] (empty = cleared in memory; Host may ignore empty). */
  void tokenChanged(int span, const QString& token);

 private:
  static bool spec_equal(const Spec& a, int w, int h, float sc, const QString& name);
  bool wait_ready(const Spec& want, QString* matched_out);
  bool wait_gone(int timeout_ms);
  /** Set cached name + query KWin index; fill Result fields. */
  void commit_identity(const Spec& want, const QString& matched, Result* r);

  KdeVirtualDisplay* vdisp_ = nullptr;
  Spec current_{};
  bool have_current_ = false;
  int active_span_ = 1;
  QString output_name_;
  int screen_index_ = -1;
  QMap<int, QString> tokens_;
};

}  // namespace vrp
