#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace vrp {

/**
 * PulseAudio / PipeWire monitor capture → interleaved S16 stereo @ 48 kHz.
 * Used to feed browser (live) audio into the Flat stereo→4.0 OpenAL path.
 *
 * open/read/close are mutex-serialized. Prefer interrupt() then join the reader
 * thread before close() — never free pa_simple while another thread is in read.
 */
class PulseMonitor {
 public:
  PulseMonitor() = default;
  ~PulseMonitor() { close(); }

  PulseMonitor(const PulseMonitor&) = delete;
  PulseMonitor& operator=(const PulseMonitor&) = delete;

  /** Open a record stream on `source` (e.g. "monaSphereYt.monitor"). */
  bool open(const std::string& source);
  /**
   * Unblock a stuck pa_simple_read (suspend monitor source). Call before joining
   * the reader thread; then close().
   */
  void interrupt();
  void close();
  bool ok() const;

  /**
   * Blocking read of up to `max_frames` stereo frames.
   * @return frames read (>0), 0 on timeout/empty, <0 on error.
   */
  int read_s16_stereo(int16_t* dst, int max_frames);

  std::string source() const;
  std::string last_error() const;

  static constexpr int kRate = 48000;
  static constexpr int kChannels = 2;

  /**
   * Ensure a hidden null sink + return its .monitor name (empty on failure).
   * Created with device.class=filter so many mixers hide it; still unload on stop.
   */
  static std::string ensure_yt_null_sink();
  /** Unload monaSphereYt null sink (Pulse module + PipeWire node). */
  static void destroy_yt_null_sink();
  /** Move Chrome onto the live null sink; mute other HMD competitors. */
  static int route_browser_to_yt_sink();
  /** Restore mutes/moves, then destroy the null sink. */
  static int restore_browser_routing();
  static constexpr const char* kYtSinkName = "monaSphereYt";

  /** Module index from last successful load-module (0 = unknown). */
  static uint32_t last_module_index();

 private:
  void close_unlocked();

  mutable std::mutex mu_;
  void* stream_ = nullptr;  // pa_simple*
  bool ok_ = false;
  std::string source_;
  std::string last_error_;
};

}  // namespace vrp
