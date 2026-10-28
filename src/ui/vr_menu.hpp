#pragma once

#include "net/dlna_client.hpp"
#include "options.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vrp {

/** Playback format list. Flat keeps the list open and inserts full/half directly under itself. */
enum class FormatChoice : uint8_t {
  Flat = 0,
  Deg180,
  Deg360,
  Mono,
  Sbs,
  Ou,
  Full,
  Half,
};

inline int format_menu_count(ProjectionMode projection) {
  return projection == ProjectionMode::Flat ? 8 : 6;
}

inline FormatChoice format_menu_at(ProjectionMode projection, int index) {
  static constexpr FormatChoice kFlat[] = {
      FormatChoice::Flat, FormatChoice::Full, FormatChoice::Half, FormatChoice::Deg180,
      FormatChoice::Deg360, FormatChoice::Mono, FormatChoice::Sbs, FormatChoice::Ou};
  static constexpr FormatChoice kOther[] = {FormatChoice::Flat, FormatChoice::Deg180, FormatChoice::Deg360,
                                            FormatChoice::Mono, FormatChoice::Sbs, FormatChoice::Ou};
  const int n = format_menu_count(projection);
  if (index < 0 || index >= n) return FormatChoice::Flat;
  return (projection == ProjectionMode::Flat ? kFlat : kOther)[index];
}

/**
 * In-HMD UI:
 *  - Menu (Start/Guide): file browser
 *  - Playback controls (B): transport + volume / FSR / format + scrubber + info
 */
class VrMenu {
 public:
  VrMenu() = default;
  ~VrMenu();
  VrMenu(const VrMenu&) = delete;
  VrMenu& operator=(const VrMenu&) = delete;

  enum class Screen : uint8_t { Browser = 0, Format = 1, Player = 2 };

  /** Sub-UI while playback controls are open. */
  enum class ControlsEdit : uint8_t {
    None = 0,
    Volume = 1,      // up/down adjusts; A keeps it, B restores and closes
    FsrPick = 2,     // A on FSR → list; cursor applies live; A close / B revert
    FormatPick = 3,
    HzPick = 4,      // Auto / 90 / 120
    HzConfirm = 5,   // restart notice → A restart / B back to HzPick
  };

  struct Entry {
    std::string name;
    std::filesystem::path path;
    /** Non-empty for DLNA / HTTP stream items (takes precedence over path for open). */
    std::string url;
    /** Non-empty for [アプリ] items (conf live_desktop_app). */
    std::string exec;
    std::vector<std::string> exec_args;
    /** Per-title Virtual span x1–x3 (fixed width, shorter height). */
    int monitor_span = 1;
    /** App index into desktop_apps_ when drilling into titles (-1 = none). */
    int desktop_app_index = -1;
    /** DLNA ContentDirectory object id (containers / items). */
    std::string dlna_id;
    bool is_dir = false;
    bool is_video = false;
    bool is_special = false;
  };

  struct Snapshot {
    uint64_t gen = 0;
    bool visible = false;
    bool controls_visible = false;
    /** Seek bar alone, while playback controls are hidden. */
    bool seek_bar_only = false;
    /** How far from 0 the thumbnail strip already covers. The rest of the bar cannot be seeked. */
    double thumb_cover_sec = 0;
    Screen screen = Screen::Browser;
    ControlsEdit controls_edit = ControlsEdit::None;
    std::string title;
    std::string hint;
    std::vector<Entry> entries;
    int cursor = 0;
    int scroll = 0;
    /** Continuous selection and list origin, in rows. Digital moves snap these to integers. */
    float focus = 0.f;
    float scroll_f = 0.f;
    ProjectionMode projection = ProjectionMode::Flat;
    StereoLayout stereo = StereoLayout::Mono;
    bool stereo_full = true;
    int format_cursor = 0;
    int transport_cursor = 0;
    int picker_cursor = 0;  // within FSR / format popup
    bool playing = false;
    double position_sec = 0;
    double duration_sec = 0;
    std::string media_name;
    std::string info_video_codec;
    std::string info_res;
    std::string info_fps;
    std::string info_bitrate;
    std::string info_audio_codec;
    std::string info_audio_rate;
    std::string info_audio_channels;
    std::string info_hwaccel;
    std::string info_output_note;  // zero-copy / CPU path (ASCII)
    std::string info_display_hz;   // e.g. "120" / "-"
    float volume = 1.f;
    float rate = 1.f;
    FsrMode fsr = FsrMode::Off;
    int preferred_hz = 0;   // 0 = auto
    float display_hz = 0.f; // measured / reported
    int preview_w = 0;
    int preview_h = 0;
    /** Shared with the live menu buffer so HUD paints do not deep-copy megabytes. */
    std::shared_ptr<const std::vector<uint8_t>> preview_rgba;
    /** DLNA discover/browse running on a worker thread. */
    bool network_loading = false;
    /** Radians; HUD draws three chasing dots from this. */
    float network_spin_phase = 0.f;
  };

  struct Output {
    bool open_video = false;
    std::string video_path;
    bool apply_format = false;
    ProjectionMode projection = ProjectionMode::Flat;
    StereoLayout stereo = StereoLayout::Mono;
    /** Flat SBS/OU packing. Ignored by 180° and 360°. */
    bool stereo_full = true;
    bool play = false;
    bool pause = false;
    bool stop = false;
    bool play_toggle = false;
    bool seek_start = false;  // [|<] cue to 0
    bool seek_scrubbing = false;  // left stick held in a seek
    /** Absolute bar time, once, when the stick or LB/RB is released. */
    bool seek = false;
    double seek_sec = 0;
    float volume_delta = 0.f;
    bool rate_changed = false;
    float rate = 1.f;
    bool fsr_changed = false;
    FsrMode fsr = FsrMode::Off;
    /** User confirmed HMD refresh-rate change (may restart Monado / XR). */
    bool refresh_hz_restart = false;
    int preferred_hz = 0;  // 0 = auto
    /** Start Chromium --app on Virtual MONITOR (ScreenCast live). */
    bool open_live = false;
    /** Target URL when open_live (from conf live_app[N]=Name|URL). */
    std::string live_url;
    /** Launch registered desktop GUI on Virtual MONITOR (conf live_desktop_app). */
    bool open_desktop_app = false;
    std::string desktop_app_exec;
    std::vector<std::string> desktop_app_args;
    /** 1–3 = Virtual span (fixed live_virtual width; height from aspect). */
    int desktop_monitor_span = 1;
    /** Busy-status label (app or title name). */
    std::string desktop_app_label;
  };

  struct PadInput {
    float stick_x = 0;
    float stick_y = 0;
    bool confirm = false;
    bool confirm_held = false;  // A / confirm level
    bool back = false;
    /** Continuous B/back for long-press (not edge). */
    bool back_held = false;
    bool menu_toggle = false;
    bool nav_up = false;
    bool nav_down = false;
    bool nav_left = false;
    bool nav_right = false;
    /** LB/RB held. Same seek path as the stick, fixed 30s per host second. */
    bool seek_back_held = false;
    bool seek_forward_held = false;
    /** Left / right trigger held. L2 slows, R2 speeds up, both reset to ×1. */
    bool l2_held = false;
    bool r2_held = false;
  };

  struct LiveApp {
    std::string name;
    std::string url;
  };

  struct DesktopTitle {
    std::string name;
    int monitor_span = 1;  // x1–x3
    std::vector<std::string> args;
  };

  struct DesktopApp {
    std::string name;
    std::string exec;
    std::vector<std::string> args;
    std::vector<DesktopTitle> titles;
  };

  void init(const std::filesystem::path& start_dir);
  /** HMD Roots entries [Name] → ScreenCast live browser (conf live_app[N]=). */
  void set_live_apps(std::vector<LiveApp> apps);
  /** HMD [アプリ] list (conf live_desktop_app[N]=). */
  void set_desktop_apps(std::vector<DesktopApp> apps);
  void set_media(const std::string& path, double duration_sec);
  void set_media_info(const std::string& video_codec, const std::string& res, const std::string& fps,
                       const std::string& bitrate, const std::string& audio_codec,
                       const std::string& audio_rate, const std::string& audio_channels,
                       const std::string& hwaccel, const std::string& output_note);
  void set_output_note(const std::string& output_note);
  void set_playback(bool playing, double position_sec);
  void set_volume(float v);
  float volume() const { return volume_; }
  void set_rate(float r);
  float rate() const { return rate_; }
  void set_fsr(FsrMode m);
  FsrMode fsr() const { return fsr_; }
  void save_last_dir() const;
  std::filesystem::path load_last_dir() const;
  void set_preferred_hz(int hz);
  int preferred_hz() const { return preferred_hz_; }
  void set_display_hz(float hz);
  float display_hz() const { return display_hz_; }
  static int hz_option_value(int index);
  static int hz_option_index(int hz);
  static const char* hz_option_label(int index);
  void set_format(ProjectionMode proj, StereoLayout stereo, bool stereo_full);
  void set_preview(std::vector<uint8_t> rgba, int w, int h);
  void clear_preview();
  void notify_changed() { bump(); }

  Output update(const PadInput& in, float dt);

  bool visible() const { return visible_; }
  bool controls_visible() const { return controls_visible_; }
  /** Show the file browser at Roots (e.g. after closing live capture). */
  void show_file_browser();
  /** Show [アプリ] list (after desktop-app process exit). */
  void show_apps_list();
  bool seek_bar_only() const { return seek_bar_only_; }
  /** While thumbs are still building, keep a seek bar up (unless playback controls are open). */
  void set_thumb_bar_force(bool on);
  void set_thumb_cover(double sec);
  /** Seeking keeps the bar up. It hides about two seconds after seeking stops. */
  void note_seek_bar(bool active);
  /** One seek step on the bar. On success, `target_sec` is the absolute bar target. */
  bool arm_seek_delta(double delta_sec, double& target_sec);
  /**
   * Publish the bar time for this HUD tick. Returns the menu generation of that
   * paint, or 0 when the shown time did not change.
   */
  uint64_t publish_seek_bar();
  /** Host seconds since the previous UI tick. The seek clock uses this, not the paint. */
  void set_seek_frame_dt(float dt);
  double seek_bar_sec() const { return seek_bar_sec_; }
  /** Cinema thumb time. Updates only after the matching bar mesh is shown. */
  double seek_drawn_sec() const { return seek_drawn_sec_; }
  bool seek_bar_drawn() const { return seek_bar_drawn_; }
  void note_seek_bar_drawn(double sec);
  void clear_seek_bar_drawn();
  double place_seek_bar(double sec);
  Screen screen() const { return screen_; }
  /**
   * XR thread: copy live menu state into the HUD-facing snapshot.
   * Call after menu mutations for the frame so Qt HUD paints never race live fields.
   */
  void publish_hud_snapshot();
  /** Qt HUD thread: locked copy of the last publish_hud_snapshot(). */
  Snapshot snapshot() const;
  /** Browser row under the cursor, including directories. Empty outside the file list. */
  std::string highlighted_row_id() const;
  std::string highlighted_video_path() const;
  /** The file-list highlight is sliding between rows. */
  bool list_gliding() const { return list_glide_paint_; }
  /** The seek bar is sliding between stick-seek targets. */
  bool seek_gliding() const { return seek_glide_paint_; }
  /** DLNA discover/browse worker is running (HUD spins at higher fps). */
  bool network_busy() const { return dlna_loading_.load(std::memory_order_acquire); }

  /** Rows shown in the file list. Page left/right moves by this many. */
  static constexpr int kVisibleRows = 17;

 private:
  enum class Place : uint8_t {
    Roots = 0,
    HomeTree = 1,
    RemovableList = 2,
    RemovableTree = 3,
    NetworkHub = 4,   // DLNA / smb (todo)
    NetworkList = 5,  // DLNA MediaServer discovery
    NetworkTree = 6,  // ContentDirectory browse
    AppsList = 7,     // live_desktop_app entries
    AppTitles = 8,    // titles under one live_desktop_app
  };

  /** [|<][<][>/||][□][>][>|] [Vol] [Fmt] [FSR] [Hz] */
  enum Transport : int {
    kCuePrev = 0,     // [|<] seek start / prev video if near start
    kSeekBack = 1,    // [<]
    kPlayPause = 2,
    kStop = 3,
    kSeekForward = 4, // [>]
    kNextVideo = 5,   // [>|]
    kVolume = 6,
    kFormat = 7,
    kFsr = 8,
    kHz = 9,
    kTransportCount = 10,
  };

  static constexpr int kHzOptionCount = 3;  // Auto, 90, 120

  void refresh_dir(const std::string& select_name = {});
  void move_cursor(int delta);
  void step_list_row(int dir);
  void advance_list_glide(float dt);
  void sync_list_pose();
  void advance_seek_glide(float dt);
  void clamp_list_window();
  void activate();
  void go_back();
  void bump();
  void select_entry_named(const std::string& name);
  bool path_under(const std::filesystem::path& path, const std::filesystem::path& root) const;
  void list_directory(const std::filesystem::path& dir, bool add_up, const std::filesystem::path& up_path,
                      bool up_is_special);
  void list_removable_mounts();
  void list_dlna_servers();
  void list_dlna_browse();
  void cancel_dlna_job();
  void poll_dlna_job();
  void apply_dlna_server_list(const std::string& select_name);
  void apply_dlna_browse_list(const std::vector<dlna::Item>& items, const std::string& select_name);
  std::string sibling_video(int delta) const;
  std::string entry_open_path(const Entry& e) const;
  float step_rate(float cur, int dir) const;
  /** Playback-controls rate: 0.5 … 2.0 */
  float step_playback_rate(float cur, int dir) const;
  void load_volume();
  void save_volume() const;
  std::filesystem::path user_conf_path() const;
  void close_controls_edit();
  static bool is_video_ext(const std::filesystem::path& p);
  int format_option_count() const;
  void apply_format_index(int index);
  void page_cursor(int dir);

  bool visible_ = true;
  bool controls_visible_ = false;
  bool seek_bar_only_ = false;
  std::chrono::steady_clock::time_point seek_bar_until_{};
  bool thumb_bar_force_ = false;
  double thumb_cover_sec_ = 0;
  ControlsEdit controls_edit_ = ControlsEdit::None;
  Screen screen_ = Screen::Browser;
  Place place_ = Place::Roots;
  std::filesystem::path home_;
  std::filesystem::path cwd_;
  std::filesystem::path removable_root_;
  /** Cached DLNA servers from the last discovery. */
  std::vector<dlna::Server> dlna_servers_;
  int dlna_server_index_ = -1;
  std::string dlna_object_id_ = "0";
  std::vector<std::string> dlna_id_stack_;  // parent chain for go_back
  /** Playable siblings in the current DLNA folder (urls), for next/prev. */
  std::vector<std::string> dlna_folder_urls_;
  std::atomic<bool> dlna_loading_{false};
  std::thread dlna_thread_;
  struct DlnaResult {
    uint64_t job_id = 0;
    bool ready = false;
    bool is_discover = false;
    std::vector<dlna::Server> servers;
    std::vector<dlna::Item> items;
    std::string select_name;
  };
  /** Shared with worker so cancel can detach without touching destroyed `this`. */
  struct DlnaMailbox {
    std::atomic<uint64_t> job_id{0};
    std::mutex mu;
    DlnaResult result;
  };
  std::shared_ptr<DlnaMailbox> dlna_mail_ = std::make_shared<DlnaMailbox>();
  float dlna_spin_phase_ = 0.f;
  std::string dlna_pending_select_;
  std::vector<LiveApp> live_apps_;
  std::vector<DesktopApp> desktop_apps_;
  /** Index into desktop_apps_ while place_ == AppTitles. */
  int desktop_app_focus_ = -1;
  std::vector<Entry> entries_;
  int cursor_ = 0;
  int scroll_ = 0;
  float focus_f_ = 0.f;
  float scroll_f_ = 0.f;
  int list_target_ = 0;
  /** Seconds to slide one row. Digital repeat is 0.055; analog uses 1×, 2×, or 3× that. */
  float list_row_secs_ = 0.055f;
  bool list_gliding_ = false;
  /** True on a glide frame and the frame it arrives, so the last position is painted. */
  bool list_glide_paint_ = false;
  struct AxisRepeat {
    int dir = 0;
    float held = 0.f;
    bool repeating = false;
  };
  AxisRepeat list_row_repeat_{};
  AxisRepeat list_page_repeat_{};

  ProjectionMode projection_ = ProjectionMode::Flat;
  StereoLayout stereo_ = StereoLayout::Mono;
  bool stereo_full_ = true;
  int format_cursor_ = 0;
  int transport_cursor_ = kPlayPause;
  int picker_cursor_ = 0;
  ProjectionMode pick_proj_ = ProjectionMode::Flat;
  StereoLayout pick_stereo_ = StereoLayout::Mono;
  FsrMode pick_fsr_ = FsrMode::Off;
  float pick_volume_ = 0.2f;

  bool playing_ = false;
  double position_sec_ = 0;
  double duration_sec_ = 0;
  /** Shown seek-bar time while a stick seek glides. Decoder time stays in position_sec_. */
  double seek_bar_sec_ = 0;
  double seek_target_sec_ = 0;
  /** Video seconds advanced per host second while the stick or shoulder is held. */
  double seek_step_sec_ = 5;
  /** Last bar time handed to a HUD paint. */
  double seek_published_sec_ = 0;
  /** Host seconds to apply on the next seek step. Consumed by advance_seek_glide. */
  double seek_frame_dt_ = 0;
  bool seek_gliding_ = false;
  double seek_drawn_sec_ = 0;
  bool seek_bar_drawn_ = false;
  bool seek_glide_paint_ = false;
  /** Keep the bar on the commanded time. A decoded keyframe is earlier and must not pull it back. */
  bool seek_bar_hold_ = false;
  AxisRepeat stick_seek_repeat_{};
  /** After L2+R2, ignore both triggers until each has been released. */
  bool rate_trigger_lock_ = false;
  AxisRepeat rate_trigger_repeat_{};
  float volume_ = 0.2f;
  float rate_ = 1.f;
  FsrMode fsr_ = FsrMode::Quality;
  int preferred_hz_ = 0;   // 0 = auto
  float display_hz_ = 0.f;
  int pick_hz_ = 0;        // pending selection while in HzPick / HzConfirm
  std::filesystem::path media_path_;
  std::string media_name_;
  std::string info_video_codec_;
  std::string info_res_;
  std::string info_fps_;
  std::string info_bitrate_;
  std::string info_audio_codec_;
  std::string info_audio_rate_;
  std::string info_audio_channels_;
  std::string info_hwaccel_;
  std::string info_output_note_;
  std::string info_display_hz_;

  std::shared_ptr<const std::vector<uint8_t>> preview_rgba_;
  int preview_w_ = 0;
  int preview_h_ = 0;
  Snapshot published_;

  float nav_cooldown_ = 0;
  uint64_t gen_ = 1;
  mutable std::mutex mu_;

  bool prev_confirm_ = false;
  bool prev_back_ = false;
  bool prev_menu_toggle_ = false;
  bool back_held_ = false;
  static constexpr double kNearStartSec = 3.0;
};

}  // namespace vrp
