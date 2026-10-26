#pragma once

#include "capture/chromium_yt.hpp"
#include "capture/chromium_yt_x11.hpp"
#include "capture/desktop_env.hpp"
#include "capture/eis_pointer.hpp"
#include "capture/kde_eis_pointer.hpp"
#include "capture/kde_virtual_display.hpp"
#include "capture/pipewire_capture.hpp"
#include "capture/screencast_portal.hpp"

#include <QObject>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace vrp {

/**
 * Single state machine for live browser → HMD Flat capture.
 *
 * Idle → VDispReady → Placing → Placed → Portal → Streaming → Stopping → Idle
 *
 * KDE: ScreenCast-only + KWin EIS. GNOME: RemoteDesktop+ScreenCast + portal EIS.
 */
class YouTubeSession : public QObject {
  Q_OBJECT
 public:
  enum class State {
    Idle,
    VDispReady,
    Placing,
    Placed,
    Portal,
    Streaming,
    Stopping,
    Failed,
  };

  struct Config {
    QString url = QStringLiteral("https://www.youtube.com/");
    QString restore_token;
    QString output_hint;  // e.g. Virtual-monaSphere
    bool virtual_display = true;
    int virtual_w = 1960;
    int virtual_h = 1080;
    /** krfb-virtualmonitor --scale (1.0–3.0). 2.0 = 200%. */
    float virtual_scale = 2.f;
    /**
     * Chromium --force-device-scale-factor. On a scaled Virtual MONITOR keep near 1.0
     * so the compositor scale is not doubled.
     */
    float chrome_scale = 1.f;
    bool use_default_chrome_profile = true;
    /** When true, HostWindow may start Flat live audio after streaming. */
    bool enable_audio = false;
    /** Vulkan-sampleable DRM modifiers for PipeWire DMA-BUF EnumFormat (LINEAR min). */
    std::vector<uint64_t> drm_modifiers;
  };

  explicit YouTubeSession(QObject* parent = nullptr);
  ~YouTubeSession() override;

  void start(const Config& cfg);
  void stop();
  /**
   * XR thread: drop active/loading and stop PipeWire only (after DMA texture reset).
   * Portal / Chrome / KWin cleanup must follow on the Qt thread via stop().
   */
  void stop_capture_from_xr();
  /** App quit: stop session (destroys managed virtual display). */
  void shutdown();

  State state() const { return state_; }
  bool loading() const { return loading_.load(std::memory_order_acquire); }
  bool active() const { return active_.load(std::memory_order_acquire); }
  bool audio_wanted() const { return audio_wanted_.load(std::memory_order_acquire); }
  void clear_audio_wanted() { audio_wanted_.store(false, std::memory_order_release); }

  QString output_name() const { return output_name_; }
  QString restore_token() const { return restore_token_; }
  QString kwin_window_id() const { return kwin_window_id_; }
  int place_x() const { return place_x_; }
  int place_y() const { return place_y_; }
  int place_w() const { return place_w_; }
  int place_h() const { return place_h_; }
  int virtual_w() const { return cfg_.virtual_w; }
  int virtual_h() const { return cfg_.virtual_h; }

  ChromiumYtX11& x11() { return x11_; }
  const ChromiumYtX11& x11() const { return x11_; }

#if defined(VRP_HAS_PIPEWIRE)
  PipewireCapture* capture() { return capture_.get(); }
  uint64_t& frame_seq() { return frame_seq_; }
  uint64_t& dma_seq() { return dma_seq_; }
  bool& dma_logged() { return dma_logged_; }
  bool& dma_disabled() { return dma_disabled_; }
#endif

  /** XR: first displayable frame → clear loading spinner. */
  void mark_frame_presented();

 signals:
  void logMessage(const QString& msg);
  void toast(const QString& msg);
  void restoreTokenChanged(const QString& token);
  void stateChanged(vrp::YouTubeSession::State state);
  void streamingStarted();
  void stopped();
  void failed(const QString& error);
  /** ScreenCast ended externally (user Stop in tray) — HostWindow should request XR teardown. */
  void streamLost();

 private:
  void setState(State s);
  void fail(const QString& err);
  void cleanupStreaming();
  void destroyVirtualDisplay();
  void stopEis();
  void rememberSeatCursor();
  void restoreSeatCursor();
  EisPointer* activeEis();

  bool ensureVirtualDisplay();
  bool resolveOutputRect(QRect* geo, QString* matched);
  void beginPlace();
  void beginPlaceGnome();
  void pollPlace();
  void pollPlaceGnome();
  void onPlaced();
  void beginPortal(/*bool interactive_retry=*/bool after_reject = false);
  void onPortalStarted(const ScreencastPortal::Result& r);
  void onPortalFailed(const QString& e);
  void armPortalWatchdog(bool had_token);

  Config cfg_;
  State state_ = State::Idle;
  DesktopEnv desk_ = DesktopEnv::Other;
  std::atomic<bool> loading_{false};
  std::atomic<bool> active_{false};
  std::atomic<bool> audio_wanted_{false};

  ChromiumYtLauncher* chrome_ = nullptr;
  ScreencastPortal* portal_ = nullptr;
  KdeEisPointer* kde_eis_ = nullptr;
  EisPointer* portal_eis_ = nullptr;
  KdeVirtualDisplay* vdisp_ = nullptr;
  ChromiumYtX11 x11_;
#if defined(VRP_HAS_PIPEWIRE)
  std::unique_ptr<PipewireCapture> capture_;
  uint64_t frame_seq_ = 0;
  uint64_t dma_seq_ = 0;
  bool dma_logged_ = false;
  bool dma_disabled_ = false;
#endif

  QString restore_token_;
  /** Resolution associated with restore_token_ (clear token only if cfg differs). */
  int token_grant_w_ = 0;
  int token_grant_h_ = 0;
  QString output_name_;
  QStringList chrome_ids_before_;
  QString kwin_window_id_;
  int place_x_ = 0;
  int place_y_ = 0;
  int place_w_ = 1280;
  int place_h_ = 720;
  /** Desktop-global logical seat cursor before live capture stole it onto Virtual. */
  bool have_saved_cursor_ = false;
  double saved_cursor_x_ = 0;
  double saved_cursor_y_ = 0;

  int place_tries_ = 0;
  int portal_gen_ = 0;
  /** Silent restore retries after reject/hang (before interactive picker). */
  int portal_restore_retries_ = 0;
  /** True after falling back to interactive picker once this start(). */
  bool portal_retry_used_ = false;
  QTimer place_timer_;
};

}  // namespace vrp

Q_DECLARE_METATYPE(vrp::YouTubeSession::State)
