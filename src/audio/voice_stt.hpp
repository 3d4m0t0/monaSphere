#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace vrp {

/**
 * Offline mic → text via bundled/dlopen Vosk (models user-installed).
 * Safe to start/stop from the XR thread; capture+recognize run on a worker.
 */
class VoiceStt {
 public:
  VoiceStt() = default;
  ~VoiceStt();

  VoiceStt(const VoiceStt&) = delete;
  VoiceStt& operator=(const VoiceStt&) = delete;

  /**
   * Begin listening. `lang` is a UI / conf code: ja, en, de, fr, es, ko, zh_CN, zh_TW
   * (see third_party/vosk/README.md). Returns false if setup fails.
   */
  bool start(const std::string& lang);
  void stop();

  bool running() const { return running_.load(std::memory_order_acquire); }
  std::string last_error() const;

  /**
   * Search-field string (UTF-8): AcceptWaveform==0 → base + PartialResult;
   * AcceptWaveform==1 → base updated from Result, then continue (next partial after cursor).
   */
  std::string text() const;
  void clear_text();

  /** Mic input level 0..1 (peak) for HUD pulse (pre-gate). */
  float level() const { return level_.load(std::memory_order_acquire); }

  /** Soft-reset recognizer (keep mic + model). Rarely needed — □ is browser DEL only. */
  void reset_utterance();

 private:
  void worker_main(std::string lang);
  bool load_vosk();
  void unload_vosk();
  std::string resolve_model_dir(const std::string& lang) const;

  mutable std::mutex mu_;
  std::string last_error_;
  std::string text_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_req_{false};
  std::atomic<bool> reset_req_{false};
  std::atomic<float> level_{0.f};
  std::thread thr_;

  void* dl_ = nullptr;
  void* model_ = nullptr;
  using ModelNewFn = void* (*)(const char*);
  using ModelFreeFn = void (*)(void*);
  using RecNewFn = void* (*)(void*, float);
  using RecFreeFn = void (*)(void*);
  using RecAcceptFn = int (*)(void*, const char*, int);
  using RecPartialFn = const char* (*)(void*);
  using RecResultFn = const char* (*)(void*);
  using RecFinalFn = const char* (*)(void*);
  ModelNewFn model_new_ = nullptr;
  ModelFreeFn model_free_ = nullptr;
  RecNewFn rec_new_ = nullptr;
  RecFreeFn rec_free_ = nullptr;
  RecAcceptFn rec_accept_ = nullptr;
  RecPartialFn rec_partial_ = nullptr;
  RecResultFn rec_result_ = nullptr;
  RecFinalFn rec_final_ = nullptr;
};

}  // namespace vrp
