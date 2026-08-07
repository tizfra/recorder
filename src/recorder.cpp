#include "recorder.h"

#include <portaudio.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <filesystem>

#include "audio_writer.h"
#include "ring_buffer.h"

namespace recorder {

static void compute_peaks(const int32_t* input, unsigned long frames, int channels,
                           LevelData& levels) {
  int32_t peak[MAX_CHANNELS] = {};
  for (unsigned long i = 0; i < frames; ++i) {
    for (int c = 0; c < channels; ++c) {
      int32_t v = std::abs(input[i * channels + c]);
      if (v > peak[c]) peak[c] = v;
    }
  }
  constexpr float scale = 1.0f / 2147483648.0f;
  levels.channels.store(channels, std::memory_order_relaxed);
  for (int c = 0; c < channels; ++c) {
    levels.peak[c].store(peak[c] * scale, std::memory_order_relaxed);
  }
}

// --- AudioMonitor ---

AudioMonitor::AudioMonitor() = default;

AudioMonitor::~AudioMonitor() { stop(); }

bool AudioMonitor::start(int device_index, int num_channels, int sample_rate) {
  if (_stream) return true;

  PaError err = Pa_Initialize();
  if (err != paNoError) return false;
  _pa_initialized = true;

  const PaDeviceInfo* info = Pa_GetDeviceInfo(device_index);
  if (!info) {
    Pa_Terminate();
    _pa_initialized = false;
    return false;
  }

  _channels = num_channels;
  levels.channels.store(num_channels, std::memory_order_relaxed);

  PaStreamParameters params{};
  params.device = device_index;
  params.channelCount = num_channels;
  params.sampleFormat = paInt32;
  params.suggestedLatency = info->defaultHighInputLatency;
  params.hostApiSpecificStreamInfo = nullptr;

  err = Pa_OpenStream(&_stream, &params, nullptr, sample_rate, 4096,
                      paNoFlag, reinterpret_cast<PaStreamCallback*>(monitor_callback), this);
  if (err != paNoError) {
    Pa_Terminate();
    _pa_initialized = false;
    _stream = nullptr;
    return false;
  }

  Pa_StartStream(_stream);
  return true;
}

void AudioMonitor::stop() {
  if (_stream) {
    Pa_StopStream(_stream);
    Pa_CloseStream(_stream);
    _stream = nullptr;
  }
  if (_pa_initialized) {
    Pa_Terminate();
    _pa_initialized = false;
  }
  for (int c = 0; c < MAX_CHANNELS; ++c) {
    levels.peak[c].store(0.0f, std::memory_order_relaxed);
  }
}

int AudioMonitor::monitor_callback(const void* input, void* /*output*/, unsigned long frame_count,
                                    const void* /*time_info*/, unsigned long /*flags*/,
                                    void* user_data) {
  auto* self = static_cast<AudioMonitor*>(user_data);
  compute_peaks(static_cast<const int32_t*>(input), frame_count, self->_channels, self->levels);
  return paContinue;
}

// --- Recorder ---

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

// Comprime un elenco di canali (indici 0-based) in una stringa leggibile
// con numerazione 1-based, raggruppando i run consecutivi. Es. i canali
// 0-based {0,1,8,9} (cioe' i canali 1,2,9,10 del device) diventano
// "01-02_09-10"; un singolo canale isolato appare come "05" senza range.
static std::string format_channel_suffix(const std::vector<int>& channel_indices_0based) {
  std::vector<int> sorted_ch(channel_indices_0based.begin(), channel_indices_0based.end());
  std::sort(sorted_ch.begin(), sorted_ch.end());

  std::string out;
  size_t i = 0;
  while (i < sorted_ch.size()) {
    size_t j = i;
    while (j + 1 < sorted_ch.size() && sorted_ch[j + 1] == sorted_ch[j] + 1) ++j;
    char buf[16];
    if (j == i) {
      std::snprintf(buf, sizeof(buf), "%02d", sorted_ch[i] + 1);
    } else {
      std::snprintf(buf, sizeof(buf), "%02d-%02d", sorted_ch[i] + 1, sorted_ch[j] + 1);
    }
    if (!out.empty()) out += "_";
    out += buf;
    i = j + 1;
  }
  return out;
}

static std::string make_channel_suffixed_filename(const std::string& base,
                                                   const std::string& channel_suffix) {
  std::string stem = base;
  std::string ext = ".flac";
  auto dot = base.rfind('.');
  if (dot != std::string::npos) {
    stem = base.substr(0, dot);
    ext = base.substr(dot);
  }
  return stem + "_ch" + channel_suffix + ext;
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

  // Il campo record_channels seleziona quali canali finiscono nel file
  // (gestito interamente in writer_thread_func): qui validiamo solo che
  // gli indici forniti siano dentro il range aperto sullo stream, cosi'
  // un valore fuori range viene segnalato subito invece che silenziato
  // dopo l'avvio della registrazione.
  for (int idx : _config.record_channels) {
    if (idx < 0 || idx >= _config.channels) {
      std::cerr << "Error: record_channels contains index " << idx << " out of range [0, "
                << _config.channels << ")\n";
      return false;
    }
  }

  int max_ch = max_channels_for_format(_config.output_file);
  int write_channels = _config.record_channels.empty()
      ? _config.channels : static_cast<int>(_config.record_channels.size());
  if (write_channels > max_ch) {
    std::cerr << "Recording " << write_channels << " channels in groups of " << max_ch << "\n";
  }

  int shift = format_needs_bit_shift(_config.output_file) ? 32 - _config.bit_depth : 0;
  size_t ring_capacity = _config.sample_rate * _config.channels * 2;
  _ring = std::make_unique<SpscRingBuffer<int32_t>>(ring_capacity);

  _cb_ctx = {_ring.get(), &_overruns, &_levels, _config.channels, shift};

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

  int write_channels = _config.record_channels.empty()
      ? _config.channels : static_cast<int>(_config.record_channels.size());
  std::cerr << "Recording: device=\"" << _device_name << "\", " << _config.channels
            << "ch captured, " << write_channels << "ch written, " << _config.sample_rate
            << "Hz, " << _config.bit_depth << "bit → " << _config.output_file << "\n";
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

  // Nota: qui catturiamo SEMPRE tutti i ctx->channels canali del device
  // nel ring buffer, indipendentemente da quali verranno poi scritti su
  // file (quella selezione avviene in writer_thread_func). Questo tiene
  // i VU meter attivi su tutti i canali anche quando se ne registra solo
  // un sottoinsieme, utile per scegliere quali canali includere.
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

  compute_peaks(in, frame_count, ctx->channels, *ctx->levels);

  return paContinue;
}

static constexpr uint64_t max_file_bytes = 3900000000ULL;

struct ChannelGroup {
  // Indici (0-based, dentro il frame interleaved a _config.channels
  // canali letto dal ring buffer) dei canali che questo gruppo scrive
  // sul proprio file. Non necessariamente contigui: riflettono l'ordine
  // di _config.record_channels quando e' stata fatta una selezione.
  std::vector<int> channel_indices;
  std::unique_ptr<AudioWriter> writer;
  std::vector<int32_t> deinterleave_buf;
  std::string filename;
};

void Recorder::writer_thread_func() {
  const int channels = _config.channels;  // larghezza del frame interleaved nel ring buffer

  // Canali effettivamente da scrivere su file, nell'ordine desiderato.
  // Vuoto in _config.record_channels = registra tutti i canali dello
  // stream, nello stesso ordine nativo: e' il default e ricalca
  // esattamente il comportamento precedente all'introduzione di questo
  // campo.
  std::vector<int> active_channels = _config.record_channels;
  if (active_channels.empty()) {
    active_channels.resize(channels);
    for (int i = 0; i < channels; ++i) active_channels[i] = i;
  }
  const bool full_native_order = _config.record_channels.empty();
  const int write_channels = static_cast<int>(active_channels.size());

  const int max_ch = max_channels_for_format(_config.output_file);
  const int num_groups = (write_channels > max_ch) ? (write_channels + max_ch - 1) / max_ch : 1;
  const bool multi_group = num_groups > 1;
  const size_t batch_samples = 4096 * channels;
  std::vector<int32_t> buf(batch_samples);

  uint64_t frames_per_split = 0;
  if (_config.split_seconds > 0) {
    frames_per_split = static_cast<uint64_t>(_config.split_seconds * _config.sample_rate);
  }

  int file_index = 1;
  bool use_splits = frames_per_split > 0;
  std::vector<ChannelGroup> groups;

  const int channels_per_group = multi_group ? max_ch : write_channels;

  auto make_group_writers = [&](const std::string& base) -> bool {
    groups.clear();
    for (int g = 0; g < num_groups; ++g) {
      ChannelGroup cg;
      int start = g * channels_per_group;
      int count = std::min(channels_per_group, write_channels - start);
      cg.channel_indices.assign(active_channels.begin() + start,
                                active_channels.begin() + start + count);

      // Include i canali nel nome file ogni volta che la registrazione
      // NON copre tutti i canali dello stream (full_native_order=false),
      // non solo quando serve dividere in piu' file: cosi' anche una
      // selezione che sta comoda in un unico file (es. canali 1,2,9,10 di
      // un device a 32ch, sotto il limite del formato) resta identificabile
      // dal nome senza doverlo aprire — utile perche' WAV/FLAC non hanno
      // un modo standard di etichettare i singoli canali che un editor
      // come Audacity possa mostrare.
      if (multi_group || !full_native_order) {
        cg.filename = make_channel_suffixed_filename(base, format_channel_suffix(cg.channel_indices));
      } else {
        cg.filename = base;
      }

      cg.writer = create_writer(cg.filename, static_cast<int>(cg.channel_indices.size()),
                               _config.sample_rate, _config.bit_depth);
      if (!cg.writer->init()) {
        _writer_error.store(true, std::memory_order_relaxed);
        return false;
      }
      cg.deinterleave_buf.resize(4096 * cg.channel_indices.size());
      std::cerr << "  → " << cg.filename << "\n";
      groups.push_back(std::move(cg));
    }
    return true;
  };

  auto write_to_groups = [&](const int32_t* data, size_t frames) -> bool {
    // Percorso rapido: nessuna selezione (registra tutti i canali nel
    // loro ordine nativo) e nessuno split in gruppi — stesso identico
    // comportamento/prestazioni di prima dell'introduzione di
    // record_channels, nessuna copia extra per de-interleave.
    if (!multi_group && full_native_order) {
      return groups[0].writer->write_samples(data, frames);
    }
    for (auto& cg : groups) {
      int gc = static_cast<int>(cg.channel_indices.size());
      for (size_t f = 0; f < frames; ++f) {
        for (int c = 0; c < gc; ++c) {
          cg.deinterleave_buf[f * gc + c] = data[f * channels + cg.channel_indices[c]];
        }
      }
      if (!cg.writer->write_samples(cg.deinterleave_buf.data(), frames)) return false;
    }
    return true;
  };

  auto finalize_groups = [&]() {
    for (auto& cg : groups) cg.writer->finalize();
  };

  std::string base_filename =
      use_splits ? make_split_filename(_config.output_file, file_index) : _config.output_file;

  {
    std::lock_guard<std::mutex> lock(_file_mutex);
    _current_file = base_filename;
  }

  if (!make_group_writers(base_filename)) return;

  uint64_t current_file_frames = 0;

  auto rotate = [&]() -> bool {
    finalize_groups();
    _file_count.store(file_index, std::memory_order_relaxed);

    ++file_index;
    use_splits = true;
    base_filename = make_split_filename(_config.output_file, file_index);
    {
      std::lock_guard<std::mutex> lock(_file_mutex);
      _current_file = base_filename;
    }

    if (!make_group_writers(base_filename)) return false;
    current_file_frames = 0;
    return true;
  };

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

    // Time-based rotation
    if (frames_per_split > 0 && current_file_frames + frames >= frames_per_split) {
      size_t fit = frames_per_split - current_file_frames;
      if (fit > 0) {
        if (!write_to_groups(buf.data(), fit)) {
          _writer_error.store(true, std::memory_order_relaxed);
          return;
        }
        _total_frames.fetch_add(fit, std::memory_order_relaxed);
      }

      if (!rotate()) return;

      size_t leftover = frames - fit;
      if (leftover > 0) {
        if (!write_to_groups(buf.data() + fit * channels, leftover)) {
          _writer_error.store(true, std::memory_order_relaxed);
          return;
        }
        current_file_frames += leftover;
        _total_frames.fetch_add(leftover, std::memory_order_relaxed);
      }
      continue;
    }

    // Size-based rotation: check largest file in group
    bool need_rotate = false;
    for (auto& cg : groups) {
      std::error_code ec;
      auto sz = std::filesystem::file_size(cg.filename, ec);
      if (!ec && sz >= max_file_bytes) {
        need_rotate = true;
        break;
      }
    }
    if (need_rotate) {
      if (!rotate()) return;
    }

    if (!write_to_groups(buf.data(), frames)) {
      _writer_error.store(true, std::memory_order_relaxed);
      return;
    }
    current_file_frames += frames;
    _total_frames.fetch_add(frames, std::memory_order_relaxed);
  }

  finalize_groups();
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
