#pragma once

#include <QObject>
#include <QProcess>
#include <QString>

namespace vrp {

/** Chromium/Chrome --app launcher for live ScreenCast capture. */
class ChromiumYtLauncher : public QObject {
  Q_OBJECT
 public:
  explicit ChromiumYtLauncher(QObject* parent = nullptr);
  ~ChromiumYtLauncher() override;

  /** Override binary path (empty = auto-detect). */
  void set_browser_path(const QString& path) { browser_path_ = path; }
  QString browser_path() const { return browser_path_; }

  void set_url(const QString& url) { url_ = url; }
  QString url() const { return url_; }

  /** Capture / output size. */
  void set_window_size(int w, int h) {
    window_w_ = w;
    window_h_ = h;
  }
  int window_width() const { return window_w_; }
  int window_height() const { return window_h_; }

  /** Absolute desktop position (e.g. secondary / virtual monitor origin). */
  void set_window_position(int x, int y) {
    window_x_ = x;
    window_y_ = y;
    have_pos_ = true;
  }
  void clear_window_position() { have_pos_ = false; }

  void set_start_fullscreen(bool on) { start_fullscreen_ = on; }

  /** Chromium --start-minimized (map hidden, then KWin moves + fullscreen). */
  void set_start_minimized(bool on) { start_minimized_ = on; }
  bool start_minimized() const { return start_minimized_; }

  /** Chromium --force-device-scale-factor (UI chrome size). */
  void set_device_scale_factor(float s) { device_scale_ = (s >= 1.f && s <= 3.f) ? s : 1.f; }
  float device_scale_factor() const { return device_scale_; }

  /**
   * true (default): system Chrome profile (subscription logins).
   * false: isolated ~/.config/monasphere/chromium-yt (scale always applies).
   */
  void set_use_default_profile(bool on) { use_default_profile_ = on; }
  bool use_default_profile() const { return use_default_profile_; }

  bool running() const;
  qint64 process_id() const;
  /** Start or attach; returns false if no browser found. */
  bool start();
  /**
   * Stop our live window. Shared (default) profile: do not kill system Chrome.
   * Isolated: terminate the monasphere Chrome process.
   */
  void stop();

  static QString detectBrowser();
  /** Isolated profile path (live_chrome_profile=isolated). */
  static QString profileDir();
  /** Default Chrome/Chromium user-data-dir if present. */
  static QString defaultUserDataDir(const QString& browser_path);

 signals:
  void started();
  void stopped(int exit_code);
  void failed(const QString& error);

 private:
  QString browser_path_;
  QString url_ = QStringLiteral("https://www.youtube.com/");
  int window_w_ = 1280;
  int window_h_ = 720;
  int window_x_ = 0;
  int window_y_ = 0;
  bool have_pos_ = false;
  bool start_fullscreen_ = false;
  bool start_minimized_ = false;
  float device_scale_ = 1.5f;
  bool use_default_profile_ = true;
  bool shared_session_ = false;
  qint64 detached_pid_ = 0;
  QProcess proc_;
};

}  // namespace vrp
