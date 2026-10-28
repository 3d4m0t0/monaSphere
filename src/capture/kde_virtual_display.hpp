#pragma once

#include <QObject>
#include <QProcess>
#include <QRect>
#include <QString>

#include <cmath>

namespace vrp {

/**
 * KDE Plasma Wayland virtual output via `krfb-virtualmonitor`.
 * Process lifetime owns the output: stop/kill removes it from KWin.
 * No-op on non-Wayland or when the binary is missing.
 */
class KdeVirtualDisplay : public QObject {
  Q_OBJECT
 public:
  explicit KdeVirtualDisplay(QObject* parent = nullptr);
  ~KdeVirtualDisplay() override;

  void set_name(const QString& name) { name_ = name; }
  QString name() const { return name_; }

  /** Expected QScreen / kscreen name (usually "Virtual-" + name). */
  QString output_name() const;

  void set_resolution(int w, int h) {
    width_ = w > 0 ? w : 1280;
    height_ = h > 0 ? h : 720;
  }
  int width() const { return width_; }
  int height() const { return height_; }

  /** Device pixel ratio for krfb / kscreen (1.0–3.0). Larger = bigger UI chrome. */
  void set_scale(float s) { scale_ = (s >= 1.f && s <= 3.f) ? s : 1.f; }
  float scale() const { return scale_; }

  bool running() const;
  /** True if this instance started the process (vs. attaching to nothing). */
  bool owned() const { return owned_; }
  /** Resolution/scale the running krfb process was actually started with. */
  int launched_width() const { return launched_w_; }
  int launched_height() const { return launched_h_; }
  float launched_scale() const { return launched_scale_; }
  bool matches_launch(int w, int h, float sc) const {
    return owned_ && running() && launched_w_ == w && launched_h_ == h &&
           std::abs(launched_scale_ - sc) <= 0.05f;
  }

  static QString detectBinary();
  static bool waylandSession();
  /**
   * Per-span krfb --name so ScreenCast Remember tokens stay independent.
   * Same name for all spans made the portal keep only the last-auth'd grant.
   */
  static QString nameForSpan(int span);
  /** Virtual-<nameForSpan> */
  static QString outputNameForSpan(int span);
  /** Legacy + x1–x3 names we may have created. */
  static QStringList allManagedNames();
  /** Kill leftover krfb-virtualmonitor processes for this name (crash orphans). */
  static int killOrphans(const QString& name);
  /** Kill every managed monaSphere virtual (legacy + x1–x3). */
  static int killAllManaged();
  static bool anyManagedOutputExists();

  /** Enable + place + scale output via kscreen-doctor (best-effort). */
  static bool enableOutput(const QString& output_name, int x, int y, float scale = 1.f);
  /**
   * Geometry from kscreen-doctor -j (works even when Qt is xcb and misses
   * compositor-only Virtual-* outputs).
   */
  static bool queryOutputRect(const QString& output_name_substr, QRect* out, QString* matched_name);
  /**
   * Current mode size + scale for a named output (kscreen-doctor -j).
   * mode_w/h are the active mode pixels (not logical geometry/scale).
   */
  static bool queryOutputMode(const QString& output_name_substr, int* mode_w, int* mode_h,
                              float* scale, QString* matched_name = nullptr);
  /** True while kscreen still lists an output whose name contains substr. */
  static bool outputExists(const QString& output_name_substr);

  /**
   * Start krfb-virtualmonitor if needed. Returns true when the process is up
   * (caller should still wait for the output to appear in kscreen / QScreen).
   */
  bool start();
  void stop();

 signals:
  void started();
  void stopped(int exit_code);
  void failed(const QString& error);

 private:
  QString name_ = QStringLiteral("monaSphere");
  int width_ = 1280;
  int height_ = 720;
  float scale_ = 1.5f;
  int launched_w_ = 0;
  int launched_h_ = 0;
  float launched_scale_ = 0.f;
  int port_ = 5917;
  bool owned_ = false;
  /** True while stop()/kill is tearing down — suppress Crashed "failed" noise. */
  bool stopping_ = false;
  QProcess proc_;
};

}  // namespace vrp
