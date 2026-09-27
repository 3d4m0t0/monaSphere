#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vrp {

enum class AudioRouteKind {
  Unknown = 0,
  Generic,
  TvHdmi,   // living-room display on GPU HDMI
  HmdHdmi,  // HMD headphones via GPU DP/HDMI (PS VR2, Index, …)
};

struct AudioDeviceInfo {
  int index = -1;           // -1 = default
  std::string name;         // Pulse sink / "default" / vrp:profile:…
  std::string description;  // UI label
  AudioRouteKind kind = AudioRouteKind::Unknown;
  /** Higher = better HMD headphone match. 0 = not an HMD route. */
  int hmd_score = 0;
};

// FFmpeg decode + miniaudio playback (Pulse used only for sink/HMD routing when available).
class AudioPlayer {
 public:
  AudioPlayer();
  ~AudioPlayer();

  AudioPlayer(const AudioPlayer&) = delete;
  AudioPlayer& operator=(const AudioPlayer&) = delete;

  static std::vector<AudioDeviceInfo> list_devices();

  /** Classify HMD vs TV and set kind / hmd_score (optional OpenXR system name hint). */
  static void classify_device(AudioDeviceInfo& d, const std::string& xr_system_hint = {});

  /**
   * Best HMD headphone device name, or empty if none.
   * xr_system_hint boosts ports whose product name overlaps the OpenXR system.
   */
  static std::string prefer_hmd_device(const std::vector<AudioDeviceInfo>& devices,
                                       const std::string& xr_system_hint = {});

  /** Pulse sink WiVRn creates after the headset app connects, or empty. */
  static std::string find_wivrn_device(const std::vector<AudioDeviceInfo>& devices);
  static bool is_wivrn_device(const AudioDeviceInfo& device);

  /**
   * Keep miniaudio running for the whole HMD session (silence when idle).
   * Creates the device on first call; no-ops if already up on the same sink.
   * Recreates the device only when the sink name changes.
   */
  bool ensure_output(const std::string& device_name = {});
  bool is_output_ready() const;

  // device_name: Pulse sink name, empty/"default" → system default.
  /** Attach/replace demux file on the live output (calls ensure_output if needed). */
  bool open(const std::string& path, const std::string& device_name = {});
  /**
   * Switch the demuxed file while keeping the miniaudio device running (silence).
   * Calls ensure_output() if the device is not up yet.
   */
  bool switch_file(const std::string& path);
  /** Drop demux only; keep silent output device (e.g. video-only file). */
  void clear_file();
  /** Tear down device + demux (HMD disconnect / app exit). */
  void close();

  bool is_open() const { return is_output_ready(); }
  bool has_audio() const { return has_audio_; }
  const std::string& device_name() const { return device_name_; }
  const std::string& last_error() const { return last_error_; }

  void play();
  void pause();
  /** Soft-mute + flush ring; device keeps running and outputs silence to the HMD. */
  void mute_output();
  /**
   * After video seek: seek demux + start filling the ring while still muted.
   * Call enable_after_seek(video_pts) when the first post-seek video frame is ready.
   */
  void begin_seek_prefill(double sec);
  /**
   * Unmute after begin_seek_prefill. Pass the first decoded video PTS — audio is
   * realigned when video landed on an earlier keyframe (otherwise sound leads).
   */
  void enable_after_seek(double video_pts_sec);
  /** Seek to 0 + mute; device stays up (silence) until the next play(). */
  void stop();
  void toggle();
  bool is_playing() const { return playing_; }

  void seek_to(double sec);
  /** Drop buffered PCM (only while output is muted). */
  void flush_output();

  /** Linear gain 0..1 (default 1). */
  void set_volume(float v);
  float volume() const { return volume_; }

  /** Playback rate 0.5 .. 20 (1 = normal). */
  void set_rate(float rate);
  float rate() const { return rate_; }

 private:
  bool open_ = false;
  bool has_audio_ = false;
  bool playing_ = false;
  float volume_ = 0.2f;
  float rate_ = 1.f;
  /** Target used by begin_seek_prefill; compared in enable_after_seek. */
  double pending_prefill_sec_ = -1.0;
  std::string device_name_;
  std::string last_error_;

  struct State;
  std::unique_ptr<State> st_;

  void fail(const std::string& msg);
  void start_thread();
  void stop_thread();
  /** Free FFmpeg demux/codec/swr (device/ring untouched). */
  void free_demux();
  /** Open path into existing State demux slots. */
  bool setup_demux(const std::string& path);
  bool init_device(const std::string& sink_label);
  void shutdown_device();
  std::string resolve_sink_label(const std::string& device_name) const;
  /** Stop output+decode, wait writer/callback idle, reset ring (SPSC-safe). */
  void quiesce_and_flush();
  /** Post demux seek and wait; ring must already be muted. */
  void seek_demux_wait(double sec);
  /** Start decode, optionally wait for a small prefill, then enable output. */
  bool prefill_and_enable_output(bool wait_for_prefill);
};

}  // namespace vrp
