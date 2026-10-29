#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
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

// FFmpeg decode + OpenAL Soft HRTF (≥2 ch) or miniaudio (mono / OpenAL missing). Pulse for sink/HMD.
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
  /** Seek to sec and stay silent. Video is held on the matching still. */
  void park_at(double sec);
  /** Seek to sec, prefill, then unmute. Call start_presentation() immediately after. */
  void arm_at(double sec);
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

  /**
   * Head orientation for OpenAL Soft HRTF (radians).
   * Speakers stay world-locked; turning toward a side speaker brings it forward.
   */
  void set_listener_orientation(float yaw_rad, float pitch_rad);
  /** True when OpenAL Soft HRTF is active (file or live Flat path). */
  bool spatial_active() const { return spatial_active_.load(std::memory_order_acquire); }

  /** Stereo→4.0 rear upmix gain (0..2, default 0.55). Ignored for discrete multichannel. */
  void set_surround_gain(float g);
  float surround_gain() const { return surround_gain_; }

  /**
   * Live stereo from a Pulse monitor (browser/[アプリ]) → stereo OpenAL Soft HRTF
   * on the HMD sink. Guest streams go to monaSphereYt so Soft does not double-play
   * system audio. Prefer request_* from XR — never run Soft/Pulse on the XR thread.
   */
  bool start_live_monitor();
  void stop_live_monitor();
  /** Non-blocking: runs start_live_monitor on a worker. */
  void request_start_live_monitor();
  /** Non-blocking: runs stop_live_monitor on a worker. */
  void request_stop_live_monitor();
  /**
   * Block until live start/stop workers finish (and monitor is down).
   * Call from XR before tearing down capture — avoids OpenAL Soft HRTF JIT UAF.
   */
  void wait_live_monitor_idle(int timeout_ms = 8000);
  bool live_monitor_active() const { return live_monitor_.load(std::memory_order_acquire); }
  bool live_monitor_busy() const { return live_op_busy_.load(std::memory_order_acquire); }

  /** Drain live/Flat audio debug lines for the host log (XR or UI thread). */
  std::vector<std::string> drain_audio_status();

 private:
  void push_audio_status(std::string line);
  bool open_ = false;
  bool has_audio_ = false;
  bool playing_ = false;
  std::atomic<bool> spatial_active_{false};
  std::atomic<bool> live_monitor_{false};
  std::atomic<bool> live_op_busy_{false};
  /** Bumped on each live start/stop request so a wedged start cannot stick busy. */
  std::atomic<uint64_t> live_op_gen_{0};
  /** Serializes start_live_monitor / stop_live_monitor bodies (OpenAL must not overlap). */
  std::mutex live_ops_mu_;
  bool start_live_monitor_with_gen(uint64_t gen);
  float volume_ = 0.2f;
  float rate_ = 1.f;
  float surround_gain_ = 0.55f;
  std::string device_name_;
  std::string last_error_;
  std::mutex status_mu_;
  std::vector<std::string> status_lines_;

  struct State;
  std::unique_ptr<State> st_;

  void fail(const std::string& msg);
  void start_thread();
  void stop_thread();
  void start_live_thread(const std::string& monitor_source);
  void stop_live_thread();
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
