#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "cli.h"
#include "ring_buffer.h"

struct PaStreamParameters;
typedef void PaStream;

namespace recorder {

static constexpr int MAX_CHANNELS = 32;

struct LevelData {
  std::atomic<float> peak[MAX_CHANNELS] = {};
  std::atomic<int> channels{0};
};

class AudioMonitor {
 public:
  AudioMonitor();
  ~AudioMonitor();

  AudioMonitor(const AudioMonitor&) = delete;
  AudioMonitor& operator=(const AudioMonitor&) = delete;

  bool start(int device_index, int num_channels, int sample_rate);
  void stop();
  bool running() const { return _stream != nullptr; }

  LevelData levels;

 private:
  PaStream* _stream = nullptr;
  bool _pa_initialized = false;
  int _channels = 0;

  static int monitor_callback(const void* input, void* output, unsigned long frame_count,
                               const void* time_info, unsigned long flags, void* user_data);
};

struct RecordingStats {
  uint64_t frames_recorded = 0;
  uint64_t overruns = 0;
  int files_written = 0;
};

class Recorder {
 public:
  enum class State { Idle, Recording, Paused, Stopped };

  explicit Recorder(const Config& config);
  ~Recorder();

  Recorder(const Recorder&) = delete;
  Recorder& operator=(const Recorder&) = delete;

  bool open();
  bool start();
  void pause();
  void resume();
  void stop();

  State state() const { return _state.load(std::memory_order_relaxed); }
  uint64_t total_frames() const { return _total_frames.load(std::memory_order_relaxed); }
  uint64_t overruns() const { return _overruns.load(std::memory_order_relaxed); }
  int files_written() const { return _file_count.load(std::memory_order_relaxed); }
  bool has_error() const { return _writer_error.load(std::memory_order_relaxed); }
  double elapsed_seconds() const;
  std::string current_file() const;
  const std::string& device_name() const { return _device_name; }
  LevelData& levels() { return _levels; }

 private:
  struct CallbackContext {
    SpscRingBuffer<int32_t>* ring;
    std::atomic<uint64_t>* overruns;
    LevelData* levels;
    int channels;
    int shift;
  };

  Config _config;
  std::atomic<State> _state{State::Idle};

  std::string _device_name;
  int _device_index = -1;
  bool _pa_initialized = false;

  PaStream* _stream = nullptr;
  std::unique_ptr<SpscRingBuffer<int32_t>> _ring;
  CallbackContext _cb_ctx{};

  std::thread _writer_thread;
  std::atomic<bool> _writer_running{false};
  std::atomic<bool> _writer_error{false};

  std::atomic<uint64_t> _total_frames{0};
  std::atomic<uint64_t> _overruns{0};
  std::atomic<int> _file_count{0};

  mutable std::mutex _file_mutex;
  std::string _current_file;

  LevelData _levels;

  std::chrono::steady_clock::time_point _segment_start;
  double _accumulated_seconds = 0.0;

  static int pa_callback(const void* input, void* output, unsigned long frame_count,
                         const void* time_info, unsigned long flags, void* user_data);
  void writer_thread_func();
  static std::string make_split_filename(const std::string& base, int index);
};

RecordingStats run_recording(const Config& config, std::atomic<bool>& running);

}  // namespace recorder
