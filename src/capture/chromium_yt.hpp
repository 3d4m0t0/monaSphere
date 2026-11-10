#pragma once

#include <QObject>
#include <QProcess>
#include <QString>

namespace vrp {

/** Live browser backend (conf `live_browser`). */
enum class LiveBrowser {
  Chrome,   // Google Chrome / Chromium
  Firefox,
};

/** Chromium/Chrome/Firefox launcher for live ScreenCast capture. */
class ChromiumYtLauncher : public QObject {
  Q_OBJECT
 public:
  explicit ChromiumYtLauncher(QObject* parent = nullptr);
  ~ChromiumYtLauncher() override;

  void set_browser(LiveBrowser b) { browser_ = b; }
  LiveBrowser browser() const { return browser_; }

  /** Override binary path (empty = auto-detect for `browser_`). */
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

  /** Chromium --force-device-scale-factor (UI chrome size; ignored for Firefox). */
  void set_device_scale_factor(float s) { device_scale_ = (s >= 1.f && s <= 3.f) ? s : 1.f; }
  float device_scale_factor() const { return device_scale_; }

  /**
   * Wayland: omit --force-device-scale-factor so Chromium uses the output scale
   * (live_virtual_scale / Plasma 200% == true 2×). When false, always pass
   * --force-device-scale-factor (needed for X11 ozone). Firefox ignores this.
   */
  void set_follow_output_scale(bool on) { follow_output_scale_ = on; }
  bool follow_output_scale() const { return follow_output_scale_; }

  /**
   * Force Chromium --ozone-platform=x11 (GNOME place via X11 tree / window-position).
   * Also honoured when MONASPHERE_YT_X11=1. Ignored for Firefox (already XWayland).
   */
  void set_force_x11(bool on) { force_x11_ = on; }
  bool force_x11() const { return force_x11_; }

  /**
   * true (default): system browser profile (subscription logins).
   * false: isolated ~/.config/monasphere/chromium-yt or firefox-yt.
   */
  void set_use_default_profile(bool on) { use_default_profile_ = on; }
  bool use_default_profile() const { return use_default_profile_; }

  /**
   * When true, set PULSE_SINK/PIPEWIRE_NODE=monaSphereYt for Flat live capture.
   * Keep false while Flat OpenAL live audio is deferred — otherwise Chrome
   * targets a missing/orphan null sink and the host mixer shows a bogus mic.
   */
  void set_route_audio_to_yt_sink(bool on) { route_audio_to_yt_sink_ = on; }
  bool route_audio_to_yt_sink() const { return route_audio_to_yt_sink_; }

  bool running() const;
  qint64 process_id() const;
  /** Start or attach; returns false if no browser found. */
  bool start();
  /**
   * Stop our live window. Shared (default) profile: do not kill system browser.
   * Isolated: terminate the monasphere browser process (+ children).
   */
  void stop();

  static QString detectBrowser(LiveBrowser kind = LiveBrowser::Chrome);
  /** Isolated profile path (live_chrome_profile=isolated). */
  static QString profileDir(LiveBrowser kind = LiveBrowser::Chrome);
  /** Default Chrome/Chromium user-data-dir if present. */
  static QString defaultUserDataDir(const QString& browser_path);
  /**
   * Kill every process using the monasphere isolated Live profile.
   * GNOME Meta teardown must call this even if our QProcess already exited —
   * zygote children left on Meta-* assert-crash gnome-shell → logout.
   */
  static void killIsolatedLiveBrowsers(LiveBrowser kind = LiveBrowser::Chrome);

 signals:
  void started();
  void stopped(int exit_code);
  void failed(const QString& error);

 private:
  LiveBrowser browser_ = LiveBrowser::Chrome;
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
  bool follow_output_scale_ = false;
  bool force_x11_ = false;
  bool use_default_profile_ = true;
  bool route_audio_to_yt_sink_ = false;
  bool shared_session_ = false;
  qint64 detached_pid_ = 0;
  QProcess proc_;
};

}  // namespace vrp
