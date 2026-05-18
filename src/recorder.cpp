#include "recorder.h"

#include <portaudio.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "flac_writer.h"
#include "ring_buffer.h"

namespace recorder {

std::string Recorder::make_split_filename(const std::string& base, int index) {
  std::string stem = base;
  std::string ext = ".flac";
  auto dot = base.rfind('.');
  if (dot != std::string::npos) {
    stem = base.substr(0, dot);
    ext = base.substr(dot);
  }
  char buf[16];
  std::snprintf(buf, sizeof(buf), "_%03d", index);
  return stem + buf + ext;
}

Recorder::Recorder(const Config& config) : _config(config) {}

Recorder::~Recorder() {
  if (_state.load() == State::Recording || _state.load() == State::Paused) {
    stop();
  }
  if (_stream) {
    Pa_CloseStream(_stream);
  }
  if (_pa_initialized) {
    Pa_Terminate();
  }
}

bool Recorder::open() {
  PaError err = Pa_Initialize();
  if (err != paNoError) {
    std::cerr << "Error: PortAudio init failed: " << Pa_GetErrorText(err) << "\n";
    return false;
  }
  _pa_initialized = true;

  _device_index = _config.device_index;
  if (_device_index < 0) {
    _device_index = Pa_GetDefaultInputDevice();
    if (_device_index == paNoDevice) {
      std::cerr << "Error: no default input device\n";
      return false;
    }
  }

  const PaDeviceInfo* dev_info = Pa_GetDeviceInfo(_device_index);
  if (!dev_info) {
    std::cerr << "Error: invalid device index " << _device_index << "\n";
    return false;
  }
  _device_name = dev_info->name;

  if (_config.channels > dev_info->maxInputChannels) {
    std::cerr << "Error: device \"" << _device_name << "\" supports max "
              << dev_info->maxInputChannels << " input channels, requested " << _config.channels
              << "\n";
    return false;
  }

  if (_config.channels > 8) {
    std::cerr << "Warning: FLAC spec defines channel assignments up to 8. Files with "
              << _config.channels << " channels may not play in all software.\n";
  }

  int shift = 32 - _config.bit_depth;
  size_t ring_capacity = _config.sample_rate * _config.channels * 2;
  _ring = std::make_unique<SpscRingBuffer<int32_t>>(ring_capacity);

  _cb_ctx = {_ring.get(), &_overruns, _config.channels, shift};

  PaStreamParameters params{};
  params.device = _device_index;
  params.channelCount = _config.channels;
  params.sampleFormat = paInt32;
  params.suggestedLatency = dev_info->defaultHighInputLatency;
  params.hostApiSpecificStreamInfo = nullptr;

  err = Pa_OpenStream(&_stream, &params, nullptr, _config.sample_rate,
                      paFramesPerBufferUnspecified, paNoFlag,
                      reinterpret_cast<PaStreamCallback*>(pa_callback), &_cb_ctx);
  if (err != paNoError) {
    std::cerr << "Error: failed to open stream: " << Pa_GetErrorText(err) << "\n";
    return false;
  }

  return true;
}

bool Recorder::start() {
  State expected = State::Idle;
  if (!_state.compare_exchange_strong(expected, State::Recording)) return false;

  _writer_running.store(true);
  _writer_thread = std::thread(&Recorder::writer_thread_func, this);

  PaError err = Pa_StartStream(_stream);
  if (err != paNoError) {
    std::cerr << "Error: failed to start stream: " << Pa_GetErrorText(err) << "\n";
    _writer_running.store(false);
    _writer_thread.join();
    _state.store(State::Idle);
    return false;
  }

  _accumulated_seconds = 0.0;
  _segment_start = std::chrono::steady_clock::now();

  std::cerr << "Recording: device=\"" << _device_name << "\", " << _config.channels << "ch, "
            << _config.sample_rate << "Hz, " << _config.bit_depth << "bit → "
            << _config.output_file << "\n";
  if (_config.split_seconds > 0) {
    std::cerr << "Splitting every " << _config.split_seconds / 60.0 << " minutes\n";
  }

  return true;
}

void Recorder::pause() {
  State expected = State::Recording;
  if (!_state.compare_exchange_strong(expected, State::Paused)) return;

  Pa_StopStream(_stream);

  auto now = std::chrono::steady_clock::now();
  _accumulated_seconds += std::chrono::duration<double>(now - _segment_start).count();
}

void Recorder::resume() {
  State expected = State::Paused;
  if (!_state.compare_exchange_strong(expected, State::Recording)) return;

  _segment_start = std::chrono::steady_clock::now();
  Pa_StartStream(_stream);
}

void Recorder::stop() {
  State s = _state.load();
  if (s != State::Recording && s != State::Paused) return;

  if (s == State::Recording) {
    Pa_StopStream(_stream);
    auto now = std::chrono::steady_clock::now();
    _accumulated_seconds += std::chrono::duration<double>(now - _segment_start).count();
  }

  _state.store(State::Stopped);
  _writer_running.store(false);
  if (_writer_thread.joinable()) {
    _writer_thread.join();
  }
}

double Recorder::elapsed_seconds() const {
  State s = _state.load(std::memory_order_relaxed);
  if (s == State::Recording) {
    auto now = std::chrono::steady_clock::now();
    return _accumulated_seconds + std::chrono::duration<double>(now - _segment_start).count();
  }
  return _accumulated_seconds;
}

std::string Recorder::current_file() const {
  std::lock_guard<std::mutex> lock(_file_mutex);
  return _current_file;
}

int Recorder::pa_callback(const void* input, void* /*output*/, unsigned long frame_count,
                           const void* /*time_info*/, unsigned long /*flags*/, void* user_data) {
  auto* ctx = static_cast<CallbackContext*>(user_data);
  const auto* in = static_cast<const int32_t*>(input);
  const size_t total_samples = frame_count * ctx->channels;

  if (ctx->shift == 0) {
    size_t written = ctx->ring->write(in, total_samples);
    if (written < total_samples) {
      ctx->overruns->fetch_add(frame_count - written / ctx->channels, std::memory_order_relaxed);
    }
  } else {
    int32_t buf[4096];
    size_t remaining = total_samples;
    const int32_t* src = in;
    while (remaining > 0) {
      size_t chunk = std::min(remaining, size_t{4096});
      for (size_t i = 0; i < chunk; ++i) {
        buf[i] = src[i] >> ctx->shift;
      }
      size_t written = ctx->ring->write(buf, chunk);
      if (written < chunk) {
        size_t dropped_samples = chunk - written + (remaining - chunk);
        ctx->overruns->fetch_add(dropped_samples / ctx->channels, std::memory_order_relaxed);
        break;
      }
      src += chunk;
      remaining -= chunk;
    }
  }

  return paContinue;
}

void Recorder::writer_thread_func() {
  const int channels = _config.channels;
  const size_t batch_samples = 4096 * channels;
  std::vector<int32_t> buf(batch_samples);

  uint64_t frames_per_split = 0;
  if (_config.split_seconds > 0) {
    frames_per_split = static_cast<uint64_t>(_config.split_seconds * _config.sample_rate);
  }
  bool splitting = frames_per_split > 0;

  int file_index = 1;
  std::string filename =
      splitting ? make_split_filename(_config.output_file, file_index) : _config.output_file;

  {
    std::lock_guard<std::mutex> lock(_file_mutex);
    _current_file = filename;
  }

  auto writer = std::make_unique<FlacWriter>(filename, channels, _config.sample_rate,
                                             _config.bit_depth);
  if (!writer->init()) {
    _writer_error.store(true, std::memory_order_relaxed);
    return;
  }
  if (splitting) {
    std::cerr << "  → " << filename << "\n";
  }

  uint64_t current_file_frames = 0;

  while (_writer_running.load(std::memory_order_relaxed) || _ring->read_available() > 0) {
    size_t avail = _ring->read_available();
    if (avail == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }

    size_t to_read = std::min(avail, batch_samples);
    to_read -= to_read % channels;
    if (to_read == 0) continue;

    size_t got = _ring->read(buf.data(), to_read);
    size_t frames = got / channels;

    if (splitting && current_file_frames + frames >= frames_per_split) {
      size_t frames_remaining = frames_per_split - current_file_frames;
      if (frames_remaining > 0) {
        if (!writer->write_samples(buf.data(), frames_remaining)) {
          _writer_error.store(true, std::memory_order_relaxed);
          return;
        }
      }

      _total_frames.fetch_add(frames_remaining, std::memory_order_relaxed);
      writer->finalize();
      _file_count.store(file_index, std::memory_order_relaxed);

      ++file_index;
      filename = make_split_filename(_config.output_file, file_index);
      {
        std::lock_guard<std::mutex> lock(_file_mutex);
        _current_file = filename;
      }

      writer =
          std::make_unique<FlacWriter>(filename, channels, _config.sample_rate, _config.bit_depth);
      if (!writer->init()) {
        _writer_error.store(true, std::memory_order_relaxed);
        return;
      }
      std::cerr << "  → " << filename << "\n";
      current_file_frames = 0;

      size_t leftover = frames - frames_remaining;
      if (leftover > 0) {
        const int32_t* leftover_ptr = buf.data() + frames_remaining * channels;
        if (!writer->write_samples(leftover_ptr, leftover)) {
          _writer_error.store(true, std::memory_order_relaxed);
          return;
        }
        current_file_frames += leftover;
        _total_frames.fetch_add(leftover, std::memory_order_relaxed);
      }
      continue;
    }

    if (!writer->write_samples(buf.data(), frames)) {
      _writer_error.store(true, std::memory_order_relaxed);
      return;
    }
    current_file_frames += frames;
    _total_frames.fetch_add(frames, std::memory_order_relaxed);
  }

  writer->finalize();
  _file_count.store(file_index, std::memory_order_relaxed);
}

RecordingStats run_recording(const Config& config, std::atomic<bool>& running) {
  Recorder rec(config);
  if (!rec.open() || !rec.start()) return {};

  if (config.duration_seconds > 0) {
    std::cerr << "Duration: " << config.duration_seconds << "s\n";
  } else {
    std::cerr << "Press Ctrl+C to stop.\n";
  }

  while (running.load(std::memory_order_relaxed)) {
    if (rec.has_error()) {
      std::cerr << "Error: FLAC writer failed, stopping.\n";
      break;
    }
    if (config.duration_seconds > 0 && rec.elapsed_seconds() >= config.duration_seconds) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  rec.stop();
  return {rec.total_frames(), rec.overruns(), rec.files_written()};
}

}  // namespace recorder
