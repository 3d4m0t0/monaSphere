#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace vrp {

class VideoDecoder;

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
   * Prefer attach_file_demux() for playback — shared video demux avoids a second seek.
   */
  bool switch_file(const std::string& path);
  /**
   * File playback via VideoDecoder's shared demux (no second avformat_open / av_seek).
   * Sets up codec/swr from the video demuxer; decode thread pulls AVPackets from it.
   */
  bool attach_file_demux(class VideoDecoder* vd);
  void detach_file_demux();
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
  /** Gain 0, then drop queued PCM. Volume returns on the next prefill. */
  void mute_for_switch();
  /** Seek to sec and stay silent (non-blocking demux post). Video holds the still. */
  void park_at(double sec);
  /**
   * Blocking arm (tests / rare paths). Prefer begin_arm_at + poll_arm_at on the UI thread —
   * slow storage demux can take seconds and must not freeze the host loop.
   */
  bool arm_at(double sec);
  /** Mute, post demux seek, return immediately. Drive with poll_arm_at each tick. */
  void begin_arm_at(double sec);
  /**
   * Pending = seeking/prefilling; Ready = land done (Soft may still be empty);
   * Failed = retry begin_arm_at. Call commit_arm() then start_presentation.
   */
  enum class ArmPoll { Pending, Ready, Failed };
  ArmPoll poll_arm_at();
  /** Unmute after Ready — pair with start_presentation so A/V start together. */
  bool commit_arm();
  /** Drop an in-flight arm (new seek / scrub). Leaves output muted. */
  void cancel_arm();
  /** True while Seeking/Prefilling/Buffered (not yet committed). */
  bool arm_pending() const { return arm_phase_ != ArmPhase::Idle || arm_instant_ready_; }
  /** Seek to 0 + mute; device stays up (silence) until the next play(). */
  void stop();
  void toggle();
  bool is_playing() const { return playing_; }

  void seek_to(double sec);
  /** Drop buffered PCM (only while output is muted). */
  void flush_output();

  /** Linear file gain 0..1. 1 is the safe maximum. */
  void set_volume(float v);
  float volume() const { return volume_; }

  /**
   * `monasphere.conf` hrtf_trim_db. Decibels cut before upmix and HRTF.
   * 0..24, default 9. 0 leaves the limiter output unchanged.
   */
  void set_hrtf_trim_db(float db);
  float hrtf_trim_db() const { return hrtf_trim_db_; }

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

  /**
   * Stereo→4.0 rear upmix gain (0..2, default 0.8). Shared by file / Live / [アプリ]
   * 2ch Soft path (`monasphere.conf` surround_gain). Ignored for discrete multichannel.
   */
  void set_surround_gain(float g);
  float surround_gain() const { return surround_gain_; }

  /**
   * Short-window loudness match (`monasphere.conf` audio_normalize).
   * Default on. File and Live. Before upmix and OpenAL HRTF so head rotation
   * does not drive the limiter. Peak ceiling -6 dBFS. Listener gain unchanged.
   */
  void set_loudness_normalize(bool on);
  bool loudness_normalize() const { return loudness_normalize_; }

  /**
   * Live stereo from a Pulse monitor (browser/[アプリ]) → same Soft 4.0 path as file
   * 2ch (side-diff upmix + surround_gain) on the HMD sink. Guest streams go to
   * monaSphereYt so Soft does not double-play system audio. Prefer request_* from
   * XR — never run Soft/Pulse on the XR thread.
   */
  bool start_live_monitor();
  void stop_live_monitor();
  /** Non-blocking: runs start_live_monitor on a worker. */
  void request_start_live_monitor();
  /** Non-blocking: runs stop_live_monitor on a worker. */
  void request_stop_live_monitor();
  /**
   * Live close: mute the listener and the output sink before any buffer drains.
   * Paired with end_live_close_mute() once the Live session is gone.
   */
  void begin_live_close_mute();
  /** Restore the file listener after Live has been deleted. Idempotent. */
  void end_live_close_mute();
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
  /** True from begin_live_close_mute until end_live_close_mute or the next Live start. */
  std::atomic<bool> live_close_muted_{false};
  std::atomic<bool> live_op_busy_{false};
  /** Bumped on each live start/stop request so a wedged start cannot stick busy. */
  std::atomic<uint64_t> live_op_gen_{0};
  /** Serializes start_live_monitor / stop_live_monitor bodies (OpenAL must not overlap). */
  std::mutex live_ops_mu_;
  bool start_live_monitor_with_gen(uint64_t gen);
  float volume_ = 0.2f;
  /** Decibels cut before upmix / HRTF. Default −10 dB. */
  static constexpr float kDefaultHrtfTrimDb = 10.f;
  static constexpr float kMaxHrtfTrimDb = 24.f;
  float hrtf_trim_db_ = kDefaultHrtfTrimDb;
  /** Linear form of hrtf_trim_db_. 10^(-10/20) until set_hrtf_trim_db. */
  std::atomic<float> headroom_gain_{0.316227766f};
  /** Live / [アプリ] listener gain. Unity; the level cut is headroom_gain_. */
  float live_base_gain_ = 1.f;
  /** File switch: listener stays at 0 until the new stream is prefilled. */
  bool switch_muted_ = false;
  float rate_ = 1.f;
  float surround_gain_ = 0.8f;
  bool loudness_normalize_ = true;
  std::string device_name_;
  std::string last_error_;
  std::mutex status_mu_;
  std::vector<std::string> status_lines_;

  struct State;
  std::unique_ptr<State> st_;

  void fail(const std::string& msg);
  void start_thread();
  void stop_thread();
  void apply_listener_gain();
  void start_live_thread(const std::string& monitor_source);
  void stop_live_thread();
  /** Free FFmpeg demux/codec/swr (device/ring untouched). */
  void free_demux();
  /** Open path into existing State demux slots (legacy dual-open; prefer attach_file_demux). */
  bool setup_demux(const std::string& path);
  /** Codec+swr from shared video demux codecpar (no AVFormatContext). */
  bool setup_codec_from_params(const void* codecpar, int tb_num, int tb_den,
                               double pts_origin_sec = 0.0);
  bool init_device(const std::string& sink_label);
  void shutdown_device();
  std::string resolve_sink_label(const std::string& device_name) const;
  /** Stop output+decode, wait writer/callback idle, reset ring (SPSC-safe). */
  void quiesce_and_flush();
  /** Post demux seek and wait until done (or timeout). Ring must already be muted. */
  bool seek_demux_wait(double sec, int timeout_ms = 5000);
  /** Post demux seek without waiting (ring must already be muted). */
  uint64_t post_demux_seek(double sec);
  /**
   * Start decode, wait for prefill when requested, then enable output.
   * Returns false if wait_for_prefill and PCM never arrived — output stays muted.
   */
  bool prefill_and_enable_output(bool wait_for_prefill);
  /** Unmute after prefill depth is met (no wait). */
  bool enable_output_after_prefill();
  bool prefill_depth_ready() const;
  /** Shared demux: flush codec/ring, set discard_until, start muted decode fill. */
  void enter_shared_prefill(double sec);

  enum class ArmPhase { Idle, Seeking, Prefilling, Buffered };
  ArmPhase arm_phase_ = ArmPhase::Idle;
  uint64_t arm_epoch_ = 0;
  double arm_sec_ = 0.0;
  std::chrono::steady_clock::time_point arm_phase_from_{};
  /** begin_arm_at with no demux (live / no audio) — poll returns Ready until commit. */
  bool arm_instant_ready_ = false;
  /** Shared file demux (owned by VideoDecoder). nullptr = legacy own-fmt mode. */
  VideoDecoder* file_demux_ = nullptr;
};

}  // namespace vrp
