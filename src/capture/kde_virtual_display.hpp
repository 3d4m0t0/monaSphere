#pragma once

#include <QObject>
#include <QProcess>
#include <QRect>
#include <QString>

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

  static QString detectBinary();
  static bool waylandSession();
  /** Kill leftover krfb-virtualmonitor processes for this name (crash orphans). */
  static int killOrphans(const QString& name);

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
  int port_ = 5917;
  bool owned_ = false;
  /** True while stop()/kill is tearing down — suppress Crashed "failed" noise. */
  bool stopping_ = false;
  QProcess proc_;
};

}  // namespace vrp
