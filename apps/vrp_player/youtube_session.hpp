#pragma once

#include "capture/chromium_yt.hpp"
#include "capture/chromium_yt_x11.hpp"
#include "capture/desktop_env.hpp"
#include "capture/eis_pointer.hpp"
#include "capture/kde_eis_pointer.hpp"
#include "capture/pipewire_capture.hpp"
#include "capture/screencast_portal.hpp"
#include "capture/virtual_display_manager.hpp"

#include <QObject>
#include <QProcess>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
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
    /**
     * Non-empty: launch this executable on Virtual instead of Chromium
     * (HMD [アプリ] / live_desktop_app). Process exit ends capture.
     */
    QString exec;
    QStringList exec_args;
    /** Display name for busy status (app or title). */
    QString desktop_label;
    /**
     * Legacy PadFilter uinput node. Prefer pad_hook_sock + LD_PRELOAD (no uinput).
     */
    QString sdl_joystick_device;
    /** Absolute path of Unix socket for libmonasphere_padhook.so ↔ host. */
    QString pad_hook_sock;
    /** Absolute path of libmonasphere_padhook.so for LD_PRELOAD. */
    QString pad_hook_lib;
    QString restore_token;
    QString output_hint;  // e.g. Virtual-monaSphere-x1
    /** krfb --name (monaSphere-xN). Empty → derive from output_hint / x1. */
    QString virtual_name;
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

  /** Shared Virtual lifecycle (owned by Host). Required before start(). */
  void set_virtual_manager(VirtualDisplayManager* mgr) { vdisp_mgr_ = mgr; }
  VirtualDisplayManager* virtual_manager() const { return vdisp_mgr_; }

  void start(const Config& cfg);
  void stop();
  /**
   * XR thread: drop active/loading and stop PipeWire only (after DMA texture reset).
   * Portal / Chrome / KWin cleanup must follow on the Qt thread via stop().
   */
  void stop_capture_from_xr();
  /** App quit: stop session. Virtual destroy is Host/manager policy. */
  void shutdown();

  /** True while manager owns a running Virtual. */
  bool virtual_display_alive() const;

  State state() const { return state_; }
  bool loading() const { return loading_.load(std::memory_order_acquire); }
  bool active() const { return active_.load(std::memory_order_acquire); }
  /** True when this session launched conf live_desktop_app (not Chromium). */
  bool desktop_app_mode() const { return !cfg_.exec.isEmpty(); }
  bool audio_wanted() const { return audio_wanted_.load(std::memory_order_acquire); }
  void clear_audio_wanted() { audio_wanted_.store(false, std::memory_order_release); }

  /** Short status under Flat spinner (「Name 起動中」など). Empty = no busy overlay. */
  void set_busy_status(const QString& status);
  void clear_busy_status();
  QString busy_status() const;
  bool show_busy_overlay() const {
    return loading_.load(std::memory_order_acquire) || !busy_status().isEmpty();
  }
  QString desktop_label() const { return desktop_label_; }

  QString output_name() const { return output_name_; }
  QString restore_token() const { return restore_token_; }
  QString kwin_window_id() const { return kwin_window_id_; }
  int place_x() const { return place_x_; }
  int place_y() const { return place_y_; }
  int place_w() const { return place_w_; }
  int place_h() const { return place_h_; }
  int virtual_w() const { return cfg_.virtual_w; }
  int virtual_h() const { return cfg_.virtual_h; }
  /** False when portal ScreenCast has no Embedded cursor (Flat draws software). */
  bool embedded_cursor() const { return portal_ && portal_->embedded_cursor(); }

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
  /** Desktop [アプリ] process exited — Host should idle Flat and open AppsList. */
  void desktopAppExited();

 private:
  void setState(State s);
  void fail(const QString& err);
  void cleanupStreaming();
  void stopEis();
  void rememberSeatCursor();
  void restoreSeatCursor();
  EisPointer* activeEis();

  bool resolveOutputRect(QRect* geo, QString* matched);
  bool desktopAppMode() const { return desktop_app_mode(); }
  void beginPlace();
  void beginPlaceGnome();
  void beginPlaceDesktopApp();
  void pollPlace();
  void pollPlaceGnome();
  void pollPlaceDesktopApp();
  void onPlaced();
  void beginPortal(/*bool interactive_retry=*/bool after_reject = false);
  void onPortalStarted(const ScreencastPortal::Result& r);
  void onPortalFailed(const QString& e);
  void armPortalWatchdog(bool had_token);
  void stopDesktopAppProcess();
  void onDesktopAppFinished(int exit_code, QProcess::ExitStatus status);
  /** While Streaming: re-place PID windows (RetroArch content recreates the surface). */
  void maintainDesktopAppWindows();
  void startDesktopAppMaintain();
  void stopDesktopAppMaintain();
  void installDesktopAppSessionRule();

  Config cfg_;
  State state_ = State::Idle;
  DesktopEnv desk_ = DesktopEnv::Other;
  std::atomic<bool> loading_{false};
  std::atomic<bool> active_{false};
  std::atomic<bool> audio_wanted_{false};
  /** True while we are intentionally stopping the desktop app (avoid re-entrant stop). */
  bool desktop_app_stopping_ = false;
  bool desktop_rule_installed_ = false;
  QString desktop_wmclass_;
  /** resourceClass/Name substrings for KWin place (e.g. retroarch). */
  QStringList desktop_want_names_;
  QString desktop_label_;
  mutable std::mutex busy_mu_;
  QString busy_status_;

  ChromiumYtLauncher* chrome_ = nullptr;
  QProcess* desktop_app_ = nullptr;
  ScreencastPortal* portal_ = nullptr;
  KdeEisPointer* kde_eis_ = nullptr;
  EisPointer* portal_eis_ = nullptr;
  /** Not owned — HostWindow VirtualDisplayManager. */
  VirtualDisplayManager* vdisp_mgr_ = nullptr;
  ChromiumYtX11 x11_;
#if defined(VRP_HAS_PIPEWIRE)
  std::unique_ptr<PipewireCapture> capture_;
  uint64_t frame_seq_ = 0;
  uint64_t dma_seq_ = 0;
  bool dma_logged_ = false;
  bool dma_disabled_ = false;
#endif

  QString restore_token_;
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
  /** Desktop [アプリ]: keep new windows on Virtual after content start. */
  QTimer desk_maintain_timer_;
};

}  // namespace vrp

Q_DECLARE_METATYPE(vrp::YouTubeSession::State)
