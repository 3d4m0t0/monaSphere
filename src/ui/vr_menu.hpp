#pragma once

#include "options.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
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
    bool is_dir = false;
    bool is_video = false;
    bool is_special = false;
  };

  struct Snapshot {
    uint64_t gen = 0;
    bool visible = false;
    bool controls_visible = false;
    Screen screen = Screen::Browser;
    ControlsEdit controls_edit = ControlsEdit::None;
    std::string title;
    std::string hint;
    std::vector<Entry> entries;
    int cursor = 0;
    int scroll = 0;
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
    std::vector<uint8_t> preview_rgba;
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
    bool seek_back = false;
    bool seek_forward = false;
    bool seek_start = false;  // [|<] cue to 0
    bool seek_scrubbing = false;  // A hold-repeat on [<]/[>]
    float volume_delta = 0.f;
    bool rate_changed = false;
    float rate = 1.f;
    bool fsr_changed = false;
    FsrMode fsr = FsrMode::Off;
    /** User confirmed HMD refresh-rate change (may restart Monado / XR). */
    bool refresh_hz_restart = false;
    int preferred_hz = 0;  // 0 = auto
  };

  struct PadInput {
    float stick_x = 0;
    float stick_y = 0;
    bool confirm = false;
    bool confirm_held = false;  // A / confirm level (seek hold-repeat)
    bool back = false;
    /** Continuous B/back for long-press (not edge). */
    bool back_held = false;
    bool menu_toggle = false;
    bool nav_up = false;
    bool nav_down = false;
    bool nav_left = false;
    bool nav_right = false;
  };

  void init(const std::filesystem::path& start_dir);
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
  Screen screen() const { return screen_; }
  Snapshot snapshot() const;
  std::string highlighted_video_path() const;

  /** Rows shown in the file list. Page left/right moves by this many. */
  static constexpr int kVisibleRows = 17;

 private:
  enum class Place : uint8_t { Roots = 0, HomeTree = 1, RemovableList = 2, RemovableTree = 3 };

  /** [|<][<][>/||][□][>][>|] [Vol] [Fmt] [FSR] [Hz] */
  enum Transport : int {
    kCuePrev = 0,     // [|<] seek start / prev video if near start
    kSeekBack = 1,    // [<] same as LB
    kPlayPause = 2,
    kStop = 3,
    kSeekForward = 4, // [>] same as RB
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
  void activate();
  void go_back();
  void bump();
  void select_entry_named(const std::string& name);
  bool path_under(const std::filesystem::path& path, const std::filesystem::path& root) const;
  void list_directory(const std::filesystem::path& dir, bool add_up, const std::filesystem::path& up_path,
                      bool up_is_special);
  void list_removable_mounts();
  std::string sibling_video(int delta) const;
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
  ControlsEdit controls_edit_ = ControlsEdit::None;
  Screen screen_ = Screen::Browser;
  Place place_ = Place::Roots;
  std::filesystem::path home_;
  std::filesystem::path cwd_;
  std::filesystem::path removable_root_;
  std::vector<Entry> entries_;
  int cursor_ = 0;
  int scroll_ = 0;
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

  std::vector<uint8_t> preview_rgba_;
  int preview_w_ = 0;
  int preview_h_ = 0;

  float nav_cooldown_ = 0;
  uint64_t gen_ = 1;
  mutable std::mutex mu_;

  bool prev_confirm_ = false;
  bool prev_back_ = false;
  bool prev_menu_toggle_ = false;
  bool back_held_ = false;
  bool back_long_fired_ = false;
  float back_held_sec_ = 0.f;
  static constexpr float kBackLongPressSec = 0.45f;
  static constexpr double kNearStartSec = 3.0;

  // A held on [<]/[>] — same key-repeat timing as LB/RB
  bool confirm_seek_held_ = false;
  bool confirm_seek_repeating_ = false;
  std::chrono::steady_clock::time_point confirm_seek_next_{};
};

}  // namespace vrp
