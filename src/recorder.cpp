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

struct CallbackContext {
  SpscRingBuffer<int32_t>* ring;
  std::atomic<uint64_t>* overruns;
  int channels;
  int shift;
};

static int pa_callback(const void* input, void* /*output*/, unsigned long frame_count,
                       const PaStreamCallbackTimeInfo* /*time_info*/,
                       PaStreamCallbackFlags /*flags*/, void* user_data) {
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

static std::string make_split_filename(const std::string& base, int index) {
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

struct WriterParams {
  SpscRingBuffer<int32_t>& ring;
  int channels;
  int sample_rate;
  int bit_depth;
  std::string output_file;
  uint64_t frames_per_split;
  std::atomic<bool>& writer_running;
  std::atomic<bool>& writer_error;
  std::atomic<uint64_t>& total_frames;
  std::atomic<int>& file_count;
};

static void writer_thread_func(WriterParams p) {
  const size_t batch_samples = 4096 * p.channels;
  std::vector<int32_t> buf(batch_samples);

  int file_index = 1;
  bool splitting = p.frames_per_split > 0;
  std::string filename = splitting ? make_split_filename(p.output_file, file_index) : p.output_file;

  auto writer = std::make_unique<FlacWriter>(filename, p.channels, p.sample_rate, p.bit_depth);
  if (!writer->init()) {
    p.writer_error.store(true, std::memory_order_relaxed);
    return;
  }
  if (splitting) {
    std::cerr << "  → " << filename << "\n";
  }

  uint64_t current_file_frames = 0;

  while (p.writer_running.load(std::memory_order_relaxed) || p.ring.read_available() > 0) {
    size_t avail = p.ring.read_available();
    if (avail == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }

    size_t to_read = std::min(avail, batch_samples);
    to_read -= to_read % p.channels;
    if (to_read == 0) continue;

    size_t got = p.ring.read(buf.data(), to_read);
    size_t frames = got / p.channels;

    if (splitting && current_file_frames + frames >= p.frames_per_split) {
      // Write the frames that fit in the current file, then rotate.
      size_t frames_remaining = p.frames_per_split - current_file_frames;
      if (frames_remaining > 0) {
        if (!writer->write_samples(buf.data(), frames_remaining)) {
          p.writer_error.store(true, std::memory_order_relaxed);
          return;
        }
      }

      p.total_frames.fetch_add(frames_remaining, std::memory_order_relaxed);
      writer->finalize();
      p.file_count.store(file_index, std::memory_order_relaxed);

      ++file_index;
      filename = make_split_filename(p.output_file, file_index);
      writer = std::make_unique<FlacWriter>(filename, p.channels, p.sample_rate, p.bit_depth);
      if (!writer->init()) {
        p.writer_error.store(true, std::memory_order_relaxed);
        return;
      }
      std::cerr << "  → " << filename << "\n";
      current_file_frames = 0;

      // Write leftover frames to the new file.
      size_t leftover = frames - frames_remaining;
      if (leftover > 0) {
        const int32_t* leftover_ptr = buf.data() + frames_remaining * p.channels;
        if (!writer->write_samples(leftover_ptr, leftover)) {
          p.writer_error.store(true, std::memory_order_relaxed);
          return;
        }
        current_file_frames += leftover;
        p.total_frames.fetch_add(leftover, std::memory_order_relaxed);
      }
      continue;
    }

    if (!writer->write_samples(buf.data(), frames)) {
      p.writer_error.store(true, std::memory_order_relaxed);
      return;
    }
    current_file_frames += frames;
    p.total_frames.fetch_add(frames, std::memory_order_relaxed);
  }

  writer->finalize();
  p.file_count.store(file_index, std::memory_order_relaxed);
}

RecordingStats run_recording(const Config& config, std::atomic<bool>& running) {
  RecordingStats stats{};

  PaError err = Pa_Initialize();
  if (err != paNoError) {
    std::cerr << "Error: PortAudio init failed: " << Pa_GetErrorText(err) << "\n";
    return stats;
  }

  int device = config.device_index;
  if (device < 0) {
    device = Pa_GetDefaultInputDevice();
    if (device == paNoDevice) {
      std::cerr << "Error: no default input device\n";
      Pa_Terminate();
      return stats;
    }
  }

  const PaDeviceInfo* dev_info = Pa_GetDeviceInfo(device);
  if (!dev_info) {
    std::cerr << "Error: invalid device index " << device << "\n";
    Pa_Terminate();
    return stats;
  }

  if (config.channels > dev_info->maxInputChannels) {
    std::cerr << "Error: device \"" << dev_info->name << "\" supports max "
              << dev_info->maxInputChannels << " input channels, requested " << config.channels
              << "\n";
    Pa_Terminate();
    return stats;
  }

  if (config.channels > 8) {
    std::cerr << "Warning: FLAC spec defines channel assignments up to 8. Files with "
              << config.channels << " channels may not play in all software.\n";
  }

  int shift = 32 - config.bit_depth;
  size_t ring_capacity = config.sample_rate * config.channels * 2;
  auto ring = std::make_unique<SpscRingBuffer<int32_t>>(ring_capacity);

  std::atomic<uint64_t> overruns{0};
  CallbackContext cb_ctx{ring.get(), &overruns, config.channels, shift};

  PaStreamParameters params{};
  params.device = device;
  params.channelCount = config.channels;
  params.sampleFormat = paInt32;
  params.suggestedLatency = dev_info->defaultHighInputLatency;
  params.hostApiSpecificStreamInfo = nullptr;

  PaStream* stream = nullptr;
  err = Pa_OpenStream(&stream, &params, nullptr, config.sample_rate, paFramesPerBufferUnspecified,
                      paNoFlag, pa_callback, &cb_ctx);
  if (err != paNoError) {
    std::cerr << "Error: failed to open stream: " << Pa_GetErrorText(err) << "\n";
    Pa_Terminate();
    return stats;
  }

  uint64_t frames_per_split = 0;
  if (config.split_seconds > 0) {
    frames_per_split = static_cast<uint64_t>(config.split_seconds * config.sample_rate);
  }

  std::atomic<bool> writer_running{true};
  std::atomic<bool> writer_error{false};
  std::atomic<uint64_t> total_frames{0};
  std::atomic<int> file_count{0};

  WriterParams wp{*ring,          config.channels,   config.sample_rate, config.bit_depth,
                  config.output_file, frames_per_split, writer_running,     writer_error,
                  total_frames,   file_count};

  std::thread writer_thread(writer_thread_func, std::move(wp));

  err = Pa_StartStream(stream);
  if (err != paNoError) {
    std::cerr << "Error: failed to start stream: " << Pa_GetErrorText(err) << "\n";
    writer_running.store(false);
    writer_thread.join();
    Pa_CloseStream(stream);
    Pa_Terminate();
    return stats;
  }

  std::cerr << "Recording: device=\"" << dev_info->name << "\", " << config.channels << "ch, "
            << config.sample_rate << "Hz, " << config.bit_depth << "bit → " << config.output_file
            << "\n";
  if (config.split_seconds > 0) {
    std::cerr << "Splitting every " << config.split_seconds / 60.0 << " minutes\n";
  }
  if (config.duration_seconds > 0) {
    std::cerr << "Duration: " << config.duration_seconds << "s\n";
  } else {
    std::cerr << "Press Ctrl+C to stop.\n";
  }

  auto start = std::chrono::steady_clock::now();

  while (running.load(std::memory_order_relaxed)) {
    if (writer_error.load(std::memory_order_relaxed)) {
      std::cerr << "Error: FLAC writer failed, stopping.\n";
      break;
    }

    if (config.duration_seconds > 0) {
      auto elapsed = std::chrono::steady_clock::now() - start;
      double secs = std::chrono::duration<double>(elapsed).count();
      if (secs >= config.duration_seconds) break;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  Pa_StopStream(stream);
  writer_running.store(false);
  writer_thread.join();

  Pa_CloseStream(stream);
  Pa_Terminate();

  stats.frames_recorded = total_frames.load();
  stats.overruns = overruns.load();
  stats.files_written = file_count.load();
  return stats;
}

}  // namespace recorder
