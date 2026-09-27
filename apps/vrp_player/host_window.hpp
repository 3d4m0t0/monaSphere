#pragma once

#include "options.hpp"
#include "video/video_decoder.hpp"

#include <QMainWindow>
#include <QProcess>
#include <QMap>
#include <QString>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

class QCloseEvent;
class QLabel;
class QPlainTextEdit;
class QMenu;
class QAction;
class QActionGroup;
class QRadioButton;

namespace vrp {
class XrVulkanApp;
class VrMenu;
}

struct UsbHmdInfo {
  QString sysfs_name;
  quint16 vid = 0;
  quint16 pid = 0;
  QString label;
};

class HostWindow : public QMainWindow {
  Q_OBJECT
 public:
  explicit HostWindow(vrp::PlayerOptions opt, QString shader_dir, QWidget* parent = nullptr);
  ~HostWindow() override;

 public slots:
  void appendLog(const QString& line);
  /** Unminimize and focus this window. On Wayland, activation_token is required. */
  void presentToFront(const QString& activation_token = {});
  void applyStatus(const QString& hmd, const QString& session, const QString& health,
                   const QString& detail, bool tracking, int width, int height, double display_hz,
                   double frame_ms, const QString& sync_note, bool openxr_hz_switchable = false,
                   const QString& available_hz = {});

 private slots:
  void startMonado();
  void stopMonado();
  void connectHeadset();
  void disconnectHeadset();
  void onMonadoOutput();
  void onMonadoFinished(int code, QProcess::ExitStatus status);
  void refreshAudioDevices();
  /** Re-scan and optionally force HMD headphone route (PS VR2 / other GPU-HDMI HMDs). */
  void refreshAudioDevices(bool prefer_hmd);
  void applyHmdAudioRoute(const QString& xr_system_name);
  void rebuildConnectDeviceMenu();
  void rebuildAudioMenu();
  void rebuildFsrMenu();
  void rebuildHzMenu();
  void onControllerProfileAction(QAction* action);
  void onHandAction(QAction* action);
  void onAudioDeviceAction(QAction* action);
  void onFsrAction(QAction* action);
  void onHzAction(QAction* action);
  void showAboutDialog();
  void onRuntimeChosen(bool wivrn);

 private:
  /** Start owned monado-service. If allow_adopt is false, never attach to a pre-existing IPC
   *  (needed so XRT_COMPOSITOR_DESIRED_MODE is actually applied). */
  void startOwnedMonado(bool allow_external_adopt);
  void buildUi();
  void buildMenus();
  void restoreWindowGeometry();
  void saveWindowGeometry() const;
  void loadAppConf();
  void saveAppConf() const;
  void setSessionChrome(bool session_active);
  void setHealth(const QString& health, const QString& detail);
  void reportVideoOpen(const vrp::VideoInfo& info);
  void reportThumbnails(const vrp::ThumbnailStrip& strip);
  void beginThumbnailBuild(const std::string& path);
  void xrThreadMain();
  void signalXrStop();
  void stopMonadoProcess();
  /** Stop systemd user monado.socket/service and wait for IPC to die (DESIRED_MODE ownership). */
  void stopSystemdMonadoForOwnedStart();
  /** Kill owned + systemd Monado, then startOwnedMonado(false) with DESIRED_MODE. */
  void restartOwnedMonadoForHz();
  /** Join XR thread. stop_monado: kill owned Monado (needed to unblock xrWaitFrame / app exit). */
  bool shutdownXrSession(int timeout_ms, bool stop_monado);
  /** Start Monado if IPC is down; wait until live. */
  bool ensureMonadoReady();
  /** USB 上の既知 HMD 一覧 / 台数。 */
  std::vector<UsbHmdInfo> listConnectedHmds() const;
  int countConnectedHmds() const;
  /** USB をポーリングし、実機 HMD 1 台＋Monado 準備で自動接続。 */
  void maybeAutoConnectHeadset();
  /** 自動接続後の OpenXR system 名を検証（Simulated なら切断して再待機）。 */
  void onAutoConnectXrReady(const QString& system_name);
  static bool isPlaceholderHmdName(const QString& system_name);
  /** XR 切断後の UI 同期。resume_auto: 手動抑制中でなければ USB ポール再開。 */
  void syncDisconnectedUi(const QString& detail);
  void disconnectHeadsetSession(bool resume_auto_poll);
  void closeEvent(QCloseEvent* event) override;
  void requestHudPaint();
  void paintHudNow();
  QString findMonadoBinary() const;
  QString findWivrnBinary() const;
  QString findRuntimeJson() const;
  bool usingWivrn() const;
  void setRuntimeKind(bool wivrn);
  /** Disconnect an active HMD session, then stop only the runtime process this app started. */
  void releaseRuntimeForSwitch();
  void syncRuntimeRadios();
  void refreshRuntimeActions();
  bool isWivrnServerLive() const;
  bool adoptExternalWivrn();
  void startOwnedWivrn(bool allow_external_adopt);
  QString monadoIpcPath() const;
  bool isMonadoIpcLive() const;
  bool clearStaleMonadoSocket();
  bool adoptExternalMonado();
  QString selectedAudioDevice() const;
  void applyMonadoPacingEnv(QProcessEnvironment& env) const;
  void maybeRestartMonadoForHz();
  int findModeIndexForHz(int hz) const;
  /** Fill mode table via vulkaninfo (HMD display) and/or monasphere.conf — before Monado start. */
  bool ensureMonadoModeTable();
  void saveMonadoModeTable() const;
  void loadMonadoModeTable();
  /** Try OpenXR dynamic Hz; on failure (or force) restart Monado with DESIRED_MODE.
   *  Must run on the UI thread — never call from the XR thread (would join itself). */
  void applyPreferredRefreshRate(bool from_ui, bool force_monado_restart = false);

  vrp::PlayerOptions opt_;
  QString shader_dir_;
  QString runtime_json_;
  bool monado_external_ = false;
  std::atomic<bool> shutting_down_{false};

  QProcess* monado_ = nullptr;
  std::thread xr_thread_;
  std::mutex xr_app_mu_;
  vrp::XrVulkanApp* xr_app_ = nullptr;  // non-owning; set only while XR thread runs
  std::mutex menu_mu_;
  vrp::VrMenu* vr_menu_ = nullptr;
  std::atomic<uint64_t> hud_paint_req_{0};
  std::atomic<uint64_t> hud_paint_done_{0};
  std::mutex hud_pixels_mu_;
  std::vector<uint8_t> hud_pixels_;
  int hud_w_ = 1280;
  int hud_h_ = 720;
  uint64_t hud_pixels_gen_ = 0;
  std::atomic<bool> xr_stop_{false};
  std::atomic<bool> xr_running_{false};
  std::atomic<int> cmd_{0};  // 1 play, 2 seek-, 3 seek+, 4 recenter
  std::atomic<int> proj_req_{-1};
  std::atomic<int> stereo_req_{-1};
  std::mutex video_mu_;
  std::string pending_video_;
  std::atomic<bool> video_pending_{false};
  std::mutex audio_mu_;
  std::string audio_device_;
  std::atomic<bool> audio_reopen_{false};
  std::atomic<bool> audio_user_override_{false};
  QString last_xr_system_;
  std::thread thumb_thread_;
  std::atomic<bool> thumb_cancel_{false};

  QLabel* runtime_label_ = nullptr;  // not shown; kept for status text updates
  QLabel* monado_label_ = nullptr;   // not shown; kept for status text updates
  QLabel* hmd_label_ = nullptr;
  QLabel* health_label_ = nullptr;
  QLabel* sync_status_label_ = nullptr;
  QPlainTextEdit* log_ = nullptr;

  QRadioButton* runtime_monado_rb_ = nullptr;
  QRadioButton* runtime_wivrn_rb_ = nullptr;
  QAction* act_start_runtime_ = nullptr;
  QAction* act_stop_runtime_ = nullptr;
  bool runtime_ui_guard_ = false;
  std::atomic<int> runtime_kind_{0};  // 0 Monado, 1 WiVRn

  QMenu* connect_menu_ = nullptr;
  QMenu* audio_menu_ = nullptr;
  QMenu* fsr_menu_ = nullptr;
  QMenu* hz_menu_ = nullptr;
  QMenu* controller_menu_ = nullptr;
  QMenu* hand_menu_ = nullptr;
  QAction* act_gamepad_ = nullptr;
  QAction* act_disconnect_ = nullptr;
  /** SDL gamepad route. Independent of the OpenXR pad profile. */
  std::atomic<bool> gamepad_enabled_{true};
  QActionGroup* controller_group_ = nullptr;
  QActionGroup* hand_group_ = nullptr;
  QActionGroup* audio_group_ = nullptr;
  QActionGroup* fsr_group_ = nullptr;
  QActionGroup* hz_group_ = nullptr;
  bool connect_ui_enabled_ = true;
  QString selected_audio_device_ = QStringLiteral("default");
  /** Parallel to menu items: data name → display label. */
  QStringList audio_device_ids_;
  QStringList audio_device_labels_;

  int preferred_hz_ = 0;               // 0 = auto, else 90 / 120
  vrp::FsrMode preferred_fsr_ = vrp::FsrMode::Quality;
  std::atomic<int> fsr_req_{-1};       // -1 none; else FsrMode
  int monado_desired_mode_ = -1;       // cached XRT_COMPOSITOR_DESIRED_MODE
  bool monado_hz_restarted_ = false;   // one restart after parsing modes
  QMap<int, double> monado_mode_hz_;   // mode index → refresh Hz
  QMap<int, qint64> monado_mode_pixels_;  // mode index → width*height (prefer max at target Hz)
  bool monado_hz_require_owned_ = false;  // block adopt until DESIRED_MODE start succeeds
  bool monado_hz_mismatch_retried_ = false;  // one auto-correct if measured ≠ preferred
  bool user_suppressed_auto_connect_ = false;  // 手動切断後は自動再接続しない
  bool monado_auto_connect_in_progress_ = false;
  bool monado_multi_hmd_logged_ = false;
  bool monado_simulated_reject_logged_ = false;
  bool monado_intentional_stop_ = false;  // terminate/kill 時の finished を異常扱いしない
  int last_usb_hmd_count_ = -1;
  qint64 monado_auto_connect_cooldown_ms_ = 0;
  bool logged_session_refresh_hz_ = false;
  qint64 sync_lag_hold_until_ms_ = 0;  // keep 「フレーム遅れ」 visible briefly
};
