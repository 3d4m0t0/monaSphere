#pragma once

#include "capture/chromium_yt.hpp"
#include "capture/chromium_yt_x11.hpp"
#include "capture/desktop_env.hpp"
#include "capture/eis_pointer.hpp"
#include "capture/gnome_virtual_display.hpp"
#include "capture/kde_eis_pointer.hpp"
#include "capture/pipewire_capture.hpp"
#include "capture/mutter_virtual_screencast.hpp"
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
 * KDE: krfb Virtual → place → ScreenCast MONITOR + KWin EIS.
 * GNOME managed Virtual: RD+ScreenCast VIRTUAL → Meta-* (after PW) → evacuate
 *   portal dialogs to primary → (later) place browser. Soft HMD cursor while
 *   ScreenCast cursor_mode=Hidden.
 * GNOME spare MONITOR: live_virtual=0 + live_output= → place → portal MONITOR.
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
    /** Display name under the spinner (app or title). */
    QString desktop_label;
    /** Shown under the spinner (e.g. YouTube). No percent. */
    QString launch_title;
    /**
     * Legacy PadFilter uinput node. Prefer pad_hook_sock + LD_PRELOAD (no uinput).
     */
    QString sdl_joystick_device;
    /** Absolute path of Unix socket for libmonasphere_padhook.so ↔ host. */
    QString pad_hook_sock;
    /** Absolute path of libmonasphere_padhook.so for LD_PRELOAD. */
    QString pad_hook_lib;
    QString restore_token;
    QString output_hint;  // Virtual-monaSphere
    /** krfb --name (monaSphere). Empty → derive from output_hint. */
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
    /** Conf `live_browser` — Chrome/Chromium (default) or Firefox. */
    LiveBrowser browser = LiveBrowser::Chrome;
    /** When true, HostWindow may start Flat live audio after streaming. */
    bool enable_audio = false;
    /** Vulkan-sampleable DRM modifiers for PipeWire DMA-BUF EnumFormat (LINEAR min). */
    std::vector<uint64_t> drm_modifiers;
  };

  explicit YouTubeSession(QObject* parent = nullptr);
  ~YouTubeSession() override;

  /** Shared Virtual lifecycle (owned by Host). Required before start(). */
  void set_virtual_manager(VirtualDisplayManager* mgr);
  VirtualDisplayManager* virtual_manager() const { return vdisp_mgr_; }

  void start(const Config& cfg);
  void stop();
  /**
   * Live↔Live (browser): keep Virtual/ScreenCast/PW, hide HMD content, close and
   * relaunch Chrome with a new URL on the same Virtual, then reveal on first frame.
   * Returns false if a full stop/restart is required (desktop app, not streaming, …).
   */
  bool switch_live_url(const QString& url, const QString& label = {});
  /**
   * XR thread: drop active/loading and stop PipeWire only (after DMA texture reset).
   * Portal / Chrome / KWin cleanup must follow on the Qt thread via stop().
   */
  void stop_capture_from_xr();
  /**
   * App quit: stop session without Mutter Session.Stop.
   * Host destroys Meta after XR/Monado release the HMD DRM lease (avoids
   * gnome-shell SEGV / session freeze on exit).
   */
  void shutdown();

  /**
   * Profile policy for Live and Chrome warm (KDE).
   * Conf isolated always wins. GNOME + virtual Chrome: shared/default when no
   * user Wayland Chrome; otherwise force isolated (Host shows dialog).
   * forced_isolated_out is set for that GNOME Wayland fallback.
   */
  static bool resolveUseDefaultChromeProfile(bool conf_default, LiveBrowser browser,
                                             bool virtual_display,
                                             bool* forced_isolated_out = nullptr);

  /**
   * XR/filter thread: Meta capture stuck in pool-recycle hold — queue stage
   * damage on the Qt thread (safe to call from any thread).
   */
  void kick_meta_paint();

  /** True while manager owns a running Virtual. */
  bool virtual_display_alive() const;

  State state() const { return state_; }
  bool loading() const { return loading_.load(std::memory_order_acquire); }
  bool active() const { return active_.load(std::memory_order_acquire); }
  /** True when this session launched conf live_desktop_app (not Chromium). */
  bool desktop_app_mode() const { return !cfg_.exec.isEmpty(); }
  /** GNOME RecordVirtual / Meta-* browser Live (DMA or SHM). */
  bool meta_live_path() const {
    return desk_ == DesktopEnv::Gnome && cfg_.virtual_display && !desktop_app_mode();
  }
  bool audio_wanted() const { return audio_wanted_.load(std::memory_order_acquire); }
  void clear_audio_wanted() { audio_wanted_.store(false, std::memory_order_release); }

  /** Short status under Flat spinner (e.g. 「YouTube」). Empty = no overlay. */
  void set_busy_status(const QString& status);
  void clear_busy_status();
  QString busy_status() const;
  /** Progress % is unused (spinner + title only). Kept for call-site compatibility. */
  void set_busy_progress(int percent);
  int busy_progress() const;
  bool show_busy_overlay() const {
    return loading_.load(std::memory_order_acquire) || !busy_status().isEmpty();
  }
  /**
   * HMD may show the PipeWire texture. GNOME Meta: false until Chrome is on Meta
   * (wallpaper must not replace the spinner — KDE's first frame is already the browser).
   */
  bool hmd_content_ready() const {
    if (switch_hold_content_) return false;
    if (using_mutter_virtual_) return gnome_live_ready_;
    return true;
  }
  /** Live↔Live Chrome relaunch — hold last HMD frame until reveal. */
  bool switching_live() const { return switch_hold_content_; }
  /**
   * True while the old browser is gone and the new one is not yet safe to show.
   * Includes post-place paint settle (still draining wallpaper — do not import).
   */
  bool switch_freeze_frames() const {
    return switch_hold_content_ && !pending_switch_reveal_;
  }
  /** PW import resumed after paint settle; hold clears on the next presented frames. */
  bool switch_reveal_armed() const {
    return switch_hold_content_ && pending_switch_reveal_;
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
  /** True when DE cursor is baked into PW frames (skip Flat software stick). */
  bool embedded_cursor() const {
    if (using_mutter_virtual_) return true;  // Mutter RecordVirtual cursor=embedded
    return portal_ && portal_->embedded_cursor();
  }

  ChromiumYtX11& x11() { return x11_; }
  const ChromiumYtX11& x11() const { return x11_; }

#if defined(VRP_HAS_PIPEWIRE)
  PipewireCapture* capture() { return capture_.get(); }
  uint64_t& frame_seq() { return frame_seq_; }
  uint64_t& dma_seq() { return dma_seq_; }
  bool& dma_logged() { return dma_logged_; }
  bool& dma_disabled() { return dma_disabled_; }
#endif

  /**
   * XR: first *usable* frame → clear loading spinner.
   * GNOME Meta: ignored until gnome_live_ready_ (Chrome on Meta + EIS rebound);
   * wallpaper frames must not clear the spinner (KDE parity).
   */
  /** Clear busy / switch hold when content is safe to show. content_key unused (API). */
  void mark_frame_presented(uint64_t content_key = 0);
  /** Chrome on Meta (after place) — reveal PW stream + clear spinner (KDE parity). */
  void mark_gnome_live_ready();

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
  /** @param destroy_meta GNOME: Session.Stop Meta-*. False on app quit (Host after XR). */
  void cleanupStreaming(bool destroy_meta = true);
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
  /** Wayland/X11 parent handle string for xdg-desktop-portal. */
  QString portalParentWindow() const;
  /** KDE restore: CreateSession+SelectSources while Chrome is cold-starting. */
  void prefetchPortalRestore();
  /**
   * Mute+move guest audio onto monaSphereYt before Soft starts (shared Chrome
   * ignores PULSE_SINK and can blast the HMD for ~1s otherwise).
   */
  void kickEarlyYtAudioRoute();
  /** Apply cfg_ → chrome_ window/scale/profile (shared by KDE/GNOME launch). */
  void configureLiveBrowserLaunch();
  /** Close Live browser window / launcher state without tearing down capture. */
  void closeLiveBrowserWindow();
  /**
   * After PipeWire negotiates portal VIRTUAL (Mutter Meta-* exists): launch + place.
   * Call only once capture has started — RecordVirtual creates the MONITOR then.
   */
  void launchBrowserAfterGnomeVirtual(int prefer_w, int prefer_h);
  /** Pick Mutter Meta-* via DisplayConfig / non-primary QScreen. */
  bool resolveGnomeVirtualRect(int prefer_w, int prefer_h, QRect* geo, QString* matched);
  /** Move portal/remote-control dialogs off Meta-* onto primary (GNOME). */
  void armGnomePortalEvacuate();
  int evacuateGnomePortalDialogsOnce();
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
  bool desktop_rule_installing_ = false;
  /** One-shot latch: desktop window found → rule/portal (no re-entrant poll). */
  bool desktop_place_finishing_ = false;
  QString desktop_wmclass_;
  /** resourceClass/Name substrings for KWin place (e.g. retroarch). */
  QStringList desktop_want_names_;
  QString desktop_label_;
  mutable std::mutex busy_mu_;
  QString busy_status_;
  std::atomic<int> busy_percent_{-1};

  ChromiumYtLauncher* chrome_ = nullptr;
  QProcess* desktop_app_ = nullptr;
  ScreencastPortal* portal_ = nullptr;
  /** GNOME: Mutter RecordVirtual owned by VirtualDisplayManager (not this session). */
  bool using_mutter_virtual_ = false;
  /** GNOME: spinner stays until EIS rebound to Meta + Chrome launch. */
  bool gnome_live_ready_ = false;
  /** Pending ensureGnomeMeta products until onPortalStarted consumes eis_fd. */
  uint32_t gnome_node_id_ = 0;
  int gnome_eis_fd_ = -1;
  QString gnome_mapping_id_;
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
  /** GNOME: logical window size on Meta-* (EIS region); place_w_/h_ stay stream pixels. */
  int place_win_w_ = 0;
  int place_win_h_ = 0;
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
  /**
   * GNOME managed Virtual: portal Start creates the MONITOR first; browser launch
   * waits until onPortalStarted (see launchBrowserAfterGnomeVirtual).
   */
  bool gnome_defer_browser_ = false;
  /**
   * Legacy Plan B: Meta wallpaper + soft cursor only (no Chrome). Kept false —
   * Chrome on Meta supplies stage damage for Mutter RecordVirtual streams.
   */
  bool gnome_capture_only_ = false;
  /** Bumped on start/stop so evacuate timers from a prior session die. */
  int gnome_evacuate_gen_ = 0;
  /**
   * Live↔Live Chrome relaunch: hide HMD content (wallpaper/black stream) until
   * the new browser is placed and a frame arrives.
   */
  bool switch_hold_content_ = false;
  /**
   * False until post-place paint settle elapses — XR keeps switch_freeze (no wallpaper
   * import). Then true: import resumes; mark_frame_presented clears hold shortly after.
   */
  bool pending_switch_reveal_ = false;
  /** Post-settle PW frames seen while still holding. */
  int switch_reveal_seen_ = 0;
  /** Bumped on switch / cleanup so paint-settle timers die. */
  int switch_reveal_gen_ = 0;
  QTimer place_timer_;
  /** Desktop [アプリ]: keep new windows on Virtual after content start. */
  QTimer desk_maintain_timer_;
  /**
   * GNOME Meta-*: periodic stage damage so RecordVirtual does not idle into
   * multi-second pool-recycle holds (unique burst → freeze → jump).
   */
  QTimer meta_paint_timer_;
  /** Set from XR when drop_pp hold stretches — consumed by meta_paint_timer_. */
  std::atomic<bool> meta_paint_kick_{false};
  /** Bumped on stop/start so early yt-route workers exit. */
  std::atomic<int> audio_route_gen_{0};
};

}  // namespace vrp

Q_DECLARE_METATYPE(vrp::YouTubeSession::State)
