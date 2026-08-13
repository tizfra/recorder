#include "audio_player.h"

#include <portaudio.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace recorder {

namespace {
constexpr size_t kRingSeconds = 2;      // buffer di pre-carico
constexpr size_t kChunkFrames = 1024;   // dimensione letture dal file

// Su molte interfacce USB multicanale, ALSA/PortAudio espone cattura e
// riproduzione come DUE device index separati anche se e' lo stesso
// hardware fisico. Se il device richiesto (quello usato per la
// registrazione) non ha canali di output, cerchiamo un'altra entry con
// lo stesso nome che li abbia, prima di ripiegare sul device di default.
PaDeviceIndex find_output_device_for(int requested_index) {
  const PaDeviceInfo* requested = Pa_GetDeviceInfo(requested_index);
  if (requested && requested->maxOutputChannels > 0) {
    return requested_index;
  }

  if (requested) {
    std::string name = requested->name;
    int count = Pa_GetDeviceCount();
    for (int i = 0; i < count; ++i) {
      const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
      if (info && info->maxOutputChannels > 0 && name == info->name) {
        std::fprintf(stderr, "AudioPlayer: uso device output '%s' (index %d) per '%s' (index %d)\n",
                     info->name, i, requested->name, requested_index);
        return i;
      }
    }
  }

  std::fprintf(stderr, "AudioPlayer: nessuna entry di output trovata per il device richiesto, "
               "uso il device di default\n");
  return Pa_GetDefaultOutputDevice();
}

}  // namespace

int query_output_channel_count(int device_index) {
  if (Pa_Initialize() != paNoError) return 2;
  PaDeviceIndex dev = (device_index >= 0) ? find_output_device_for(device_index)
                                            : Pa_GetDefaultOutputDevice();
  const PaDeviceInfo* info = Pa_GetDeviceInfo(dev);
  int result = (info && info->maxOutputChannels > 0) ? info->maxOutputChannels : 2;
  Pa_Terminate();
  return result;
}

AudioPlayer::AudioPlayer(std::vector<std::string> file_paths, int device_index,
                         int channel_offset, int output_sample_rate)
    : _paths(std::move(file_paths)), _device_index(device_index), _channel_offset(channel_offset),
      _stream_sample_rate(output_sample_rate) {}

AudioPlayer::AudioPlayer(std::string file_path, int device_index, int channel_offset,
                         int output_sample_rate)
    : AudioPlayer(std::vector<std::string>{std::move(file_path)}, device_index, channel_offset,
                 output_sample_rate) {}

AudioPlayer::~AudioPlayer() {
  stop();
  if (_pa_initialized) Pa_Terminate();
}

bool AudioPlayer::open() {
  if (_paths.empty()) {
    std::fprintf(stderr, "AudioPlayer: nessun file da aprire\n");
    return false;
  }

  _readers.clear();
  _group_channels.clear();
  _group_offsets.clear();

  for (auto& path : _paths) {
    auto reader = create_reader(path);
    if (!reader) {
      std::fprintf(stderr, "AudioPlayer: impossibile aprire il file '%s'\n", path.c_str());
      return false;
    }
    _readers.push_back(std::move(reader));
  }

  // Verifica coerenza tra i file del gruppo: stessa frequenza per tutti
  // (sono attesi provenire dalla stessa registrazione originale), e la
  // durata usata e' il minimo tra tutti (difensivo, nel caso uno dei
  // file risultasse per qualche motivo piu' corto degli altri).
  _file_sample_rate = _readers[0]->sample_rate();
  _total_frames = _readers[0]->total_frames();
  for (size_t i = 1; i < _readers.size(); ++i) {
    if (_readers[i]->sample_rate() != _file_sample_rate) {
      std::fprintf(stderr, "AudioPlayer: i file del gruppo hanno frequenze diverse (%d vs %d Hz) "
                   "— riproduzione sincronizzata non possibile\n",
                   _readers[i]->sample_rate(), _file_sample_rate);
      return false;
    }
    _total_frames = std::min(_total_frames, _readers[i]->total_frames());
  }

  _stream_sample_rate = (_stream_sample_rate > 0) ? _stream_sample_rate : _file_sample_rate;
  if (_readers.size() > 1 && _stream_sample_rate != _file_sample_rate) {
    // Il resampling multi-reader in lockstep e' molto piu' complesso da
    // fare correttamente (ogni file avrebbe una propria posizione
    // frazionaria da tenere sincronizzata) — dato che i gruppi sono
    // sempre split della stessa registrazione originale, in pratica
    // hanno gia' la frequenza del device: se non e' cosi', meglio
    // fallire con un messaggio chiaro che riprodurre qualcosa di rotto.
    std::fprintf(stderr, "AudioPlayer: resampling non supportato per la riproduzione di piu' file "
                 "insieme (file a %d Hz, device a %d Hz)\n", _file_sample_rate, _stream_sample_rate);
    return false;
  }

  _channels = 0;
  for (auto& reader : _readers) {
    int ch = reader->channels();
    _group_offsets.push_back(_channels);
    _group_channels.push_back(ch);
    _channels += ch;
  }
  if (_channels > MAX_CHANNELS) {
    std::fprintf(stderr, "AudioPlayer: %d canali combinati superano il massimo supportato (%d)\n",
                 _channels, MAX_CHANNELS);
    return false;
  }

  _duration_seconds = _file_sample_rate > 0
      ? static_cast<double>(_total_frames) / _file_sample_rate : 0.0;
  _levels.channels.store(_channels, std::memory_order_relaxed);

  if (_stream_sample_rate != _file_sample_rate) {
    std::fprintf(stderr, "AudioPlayer: file a %d Hz, device a %d Hz — resampling attivo\n",
                 _file_sample_rate, _stream_sample_rate);
  }

  if (Pa_Initialize() != paNoError) return false;
  _pa_initialized = true;

  _ring = std::make_unique<SpscRingBuffer<int32_t>>(
      static_cast<size_t>(_stream_sample_rate) * _channels * kRingSeconds);

  PaDeviceIndex out_device = (_device_index >= 0) ? find_output_device_for(_device_index)
                                                    : Pa_GetDefaultOutputDevice();
  const PaDeviceInfo* info = Pa_GetDeviceInfo(out_device);
  if (!info || info->maxOutputChannels <= 0) {
    out_device = Pa_GetDefaultOutputDevice();
    info = Pa_GetDeviceInfo(out_device);
  }
  if (!info || info->maxOutputChannels <= 0) {
    std::fprintf(stderr, "AudioPlayer: nessun device di output disponibile\n");
    return false;
  }

  // Alcuni device USB multicanale (es. stage box digitali tipo DL32S)
  // rifiutano l'apertura dello stream con un numero di canali diverso
  // da quello nativo. Apriamo sempre con TUTTI i canali del device, e
  // nel pa_callback scriviamo i campioni reali solo sui canali
  // [channel_offset, channel_offset + channels), silenzio altrove.
  int stream_channels = info->maxOutputChannels;

  if (_channel_offset < 0 || _channel_offset + _channels > stream_channels) {
    if (_channel_offset != 0) {
      std::fprintf(stderr, "AudioPlayer: channel_offset %d non valido per %d canali combinati / "
                   "%d canali stream, uso 0\n", _channel_offset, _channels, stream_channels);
    }
    _channel_offset = 0;
  }

  if (stream_channels < _channels + _channel_offset) {
    std::fprintf(stderr, "AudioPlayer: device ha solo %d canali di output, servirebbero %d "
                 "(offset %d + %d canali combinati) — alcuni canali non verranno riprodotti\n",
                 stream_channels, _channel_offset + _channels, _channel_offset, _channels);
  }

  PaStreamParameters out_params{};
  out_params.device = out_device;
  out_params.channelCount = stream_channels;
  out_params.sampleFormat = paInt32;
  out_params.suggestedLatency = info->defaultLowOutputLatency;
  out_params.hostApiSpecificStreamInfo = nullptr;

  _cb_ctx.ring = _ring.get();
  _cb_ctx.frames_played = &_frames_played;
  _cb_ctx.levels = &_levels;
  _cb_ctx.file_channels = _channels;
  _cb_ctx.stream_channels = stream_channels;
  _cb_ctx.channel_offset = _channel_offset;
  _cb_ctx.volume_db = &_volume_db;
  _cb_ctx.state = &_state;

  PaError err = Pa_OpenStream(
      reinterpret_cast<PaStream**>(&_stream), nullptr, &out_params, _stream_sample_rate,
      256, paClipOff, reinterpret_cast<PaStreamCallback*>(&AudioPlayer::pa_callback),
      &_cb_ctx);
  if (err != paNoError) {
    std::fprintf(stderr, "AudioPlayer: Pa_OpenStream failed: %s\n", Pa_GetErrorText(err));
    return false;
  }
  return true;
}

bool AudioPlayer::start() {
  if (!_stream) return false;
  _frames_played.store(0, std::memory_order_relaxed);
  _finished_naturally.store(false, std::memory_order_relaxed);
  _reader_running.store(true, std::memory_order_relaxed);
  _state.store(State::Playing, std::memory_order_relaxed);
  _reader_thread = std::thread(&AudioPlayer::reader_thread_func, this);

  // pre-riempi meta' buffer prima di avviare lo stream per evitare
  // un underrun subito dopo l'avvio
  while (_ring->read_available() < _ring->capacity() / 2 &&
         _reader_running.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  PaError err = Pa_StartStream(reinterpret_cast<PaStream*>(_stream));
  if (err != paNoError) {
    std::fprintf(stderr, "AudioPlayer: Pa_StartStream failed: %s\n", Pa_GetErrorText(err));
    _reader_running.store(false, std::memory_order_relaxed);
    if (_reader_thread.joinable()) _reader_thread.join();
    _state.store(State::Idle, std::memory_order_relaxed);
    return false;
  }
  return true;
}

void AudioPlayer::pause() { _state.store(State::Paused, std::memory_order_relaxed); }
void AudioPlayer::resume() { _state.store(State::Playing, std::memory_order_relaxed); }

void AudioPlayer::stop() {
  if (_state.load(std::memory_order_relaxed) == State::Idle) return;
  _reader_running.store(false, std::memory_order_relaxed);
  if (_reader_thread.joinable()) _reader_thread.join();
  if (_stream) {
    // Pa_AbortStream invece di Pa_StopStream: quest'ultimo, per
    // specifica PortAudio, ASPETTA che tutto l'audio gia' in coda finisca
    // di suonare prima di ritornare — con lo stop "a scatto" che ci
    // serve qui, vogliamo interrompere subito senza aspettare il drain.
    Pa_AbortStream(reinterpret_cast<PaStream*>(_stream));
    Pa_CloseStream(reinterpret_cast<PaStream*>(_stream));
    _stream = nullptr;
  }
  _state.store(State::Stopped, std::memory_order_relaxed);
}

void AudioPlayer::seek(double seconds) {
  if (_readers.empty() || _file_sample_rate <= 0) return;

  uint64_t frame = static_cast<uint64_t>(std::max(0.0, seconds) * _file_sample_rate);
  frame = std::min(frame, _total_frames);

  // Mettiamo in pausa: il reader thread smette di scrivere e il
  // pa_callback, quando il ring si svuota, riproduce silenzio senza
  // leggere oltre. Cosi' possiamo ricreare il ring senza race.
  State prev = _state.load(std::memory_order_relaxed);
  _state.store(State::Paused, std::memory_order_relaxed);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  // Sincronizza il seek su TUTTI i file del gruppo insieme.
  for (auto& reader : _readers) {
    reader->seek(frame);
  }

  uint64_t stream_frame = _file_sample_rate > 0
      ? static_cast<uint64_t>(frame * (static_cast<double>(_stream_sample_rate) / _file_sample_rate))
      : frame;
  _frames_played.store(stream_frame, std::memory_order_relaxed);
  _resample_pos = 0.0;
  _ring = std::make_unique<SpscRingBuffer<int32_t>>(_ring->capacity());
  _cb_ctx.ring = _ring.get();

  if (prev == State::Playing) _state.store(State::Playing, std::memory_order_relaxed);
}

double AudioPlayer::position_seconds() const {
  return _stream_sample_rate > 0
      ? static_cast<double>(_frames_played.load(std::memory_order_relaxed)) / _stream_sample_rate
      : 0.0;
}

void AudioPlayer::set_volume_db(float db) {
  float clamped = std::clamp(db, -60.0f, 12.0f);
  _volume_db.store(clamped, std::memory_order_relaxed);
}

void AudioPlayer::reader_thread_func() {
  size_t n = _readers.size();
  bool single_file = (n == 1);
  // Il resampling e' supportato solo nel caso a singolo file (vedi
  // controllo in open() per il caso multi-file).
  bool need_resample = single_file && (_stream_sample_rate != _file_sample_rate) &&
                       _file_sample_rate > 0;
  double ratio = need_resample
      ? static_cast<double>(_file_sample_rate) / static_cast<double>(_stream_sample_rate)
      : 1.0;

  // Un buffer di lettura grezza per ciascun file del gruppo.
  std::vector<std::vector<int32_t>> raw_chunks(n);
  for (size_t g = 0; g < n; ++g) {
    raw_chunks[g].resize(kChunkFrames * static_cast<size_t>(_group_channels[g]));
  }
  std::vector<int32_t> combined(kChunkFrames * _channels);
  std::vector<int32_t> resampled;  // riusato solo nel caso singolo file con resampling attivo

  while (_reader_running.load(std::memory_order_relaxed)) {
    if (_state.load(std::memory_order_relaxed) == State::Paused) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }

    size_t est_output_frames = need_resample
        ? static_cast<size_t>(kChunkFrames / ratio) + 2
        : kChunkFrames;
    if (_ring->write_available() < est_output_frames * static_cast<size_t>(_channels)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }

    // Legge un chunk da OGNI file del gruppo, in lockstep. `got` e' il
    // minimo tra tutti i reader: se uno dei file finisse prima degli
    // altri (non dovrebbe capitare — sono attesi della stessa durata,
    // essendo split della stessa registrazione — ma per sicurezza), la
    // riproduzione del gruppo si ferma li' invece di continuare
    // spaiata su canali diversi.
    size_t got = kChunkFrames;
    for (size_t g = 0; g < n; ++g) {
      size_t got_g = _readers[g]->read_samples(raw_chunks[g].data(), kChunkFrames);
      got = std::min(got, got_g);
    }
    if (got == 0) {
      _reader_running.store(false, std::memory_order_relaxed);
      _finished_naturally.store(true, std::memory_order_relaxed);
      _state.store(State::Stopped, std::memory_order_relaxed);
      break;
    }

    // Interleaving: ogni gruppo scrive i propri canali nell'offset che
    // gli compete all'interno del frame combinato.
    for (size_t f = 0; f < got; ++f) {
      for (size_t g = 0; g < n; ++g) {
        int gc = _group_channels[g];
        int off = _group_offsets[g];
        const int32_t* src = &raw_chunks[g][f * static_cast<size_t>(gc)];
        int32_t* dst = &combined[f * static_cast<size_t>(_channels) + static_cast<size_t>(off)];
        std::memcpy(dst, src, static_cast<size_t>(gc) * sizeof(int32_t));
      }
    }

    if (!need_resample) {
      _ring->write(combined.data(), got * static_cast<size_t>(_channels));
      continue;
    }

    // Resampling lineare (solo caso singolo file, n==1, quindi
    // _channels == _group_channels[0] — stessa logica di sempre).
    resampled.clear();
    double pos = _resample_pos;
    while (pos < static_cast<double>(got)) {
      size_t i0 = static_cast<size_t>(pos);
      size_t i1 = std::min(i0 + 1, got - 1);
      double frac = pos - static_cast<double>(i0);
      for (int c = 0; c < _channels; ++c) {
        int32_t s0 = combined[i0 * static_cast<size_t>(_channels) + static_cast<size_t>(c)];
        int32_t s1 = combined[i1 * static_cast<size_t>(_channels) + static_cast<size_t>(c)];
        double v = s0 + (s1 - s0) * frac;
        resampled.push_back(static_cast<int32_t>(v));
      }
      pos += ratio;
    }
    _resample_pos = pos - static_cast<double>(got);

    if (!resampled.empty()) {
      _ring->write(resampled.data(), resampled.size());
    }
  }
}

int AudioPlayer::pa_callback(const void*, void* output, unsigned long frame_count,
                             const void*, unsigned long, void* user_data) {
  auto* ctx = static_cast<CallbackContext*>(user_data);
  auto* out = static_cast<int32_t*>(output);
  int file_ch = ctx->file_channels;  // larghezza TOTALE combinata (uno o piu' file)
  int stream_ch = ctx->stream_channels;
  int offset = ctx->channel_offset;

  // Silenzio su tutto il buffer di output prima di riempirlo: copre sia
  // gli under-run (dati mancanti dal ring) sia i canali "extra" oltre
  // [offset, offset+file_ch) che il device richiede ma il gruppo non usa.
  std::memset(out, 0, static_cast<size_t>(frame_count) * stream_ch * sizeof(int32_t));

  // In pausa: silenzio immediato (gia' scritto sopra) e usciamo SENZA
  // leggere dal ring buffer. Il reader thread nel frattempo smette di
  // riempirlo, quindi il contenuto gia' pre-caricato resta congelato —
  // alla ripresa si riparte esattamente da li', senza salti ne' click.
  AudioPlayer::State st = ctx->state ? ctx->state->load(std::memory_order_relaxed)
                                     : AudioPlayer::State::Playing;
  if (st == AudioPlayer::State::Paused) {
    return paContinue;
  }

  size_t needed_file_samples = static_cast<size_t>(frame_count) * file_ch;
  std::vector<int32_t> file_buf(needed_file_samples);
  size_t got = ctx->ring->read(file_buf.data(), needed_file_samples);
  size_t frames_here = got / static_cast<size_t>(file_ch);

  // Guadagno letto una volta per buffer (non per campione): sufficiente
  // per una risposta percepita come istantanea allo slider del volume
  // (il buffer copre pochi millisecondi), evita un'operazione atomica
  // per ogni singolo campione.
  float volume_db = ctx->volume_db ? ctx->volume_db->load(std::memory_order_relaxed) : 0.0f;
  float gain = std::pow(10.0f, volume_db / 20.0f);
  bool apply_gain = std::fabs(volume_db) > 0.001f;

  float peaks[MAX_CHANNELS] = {};
  int meter_channels = std::min(file_ch, static_cast<int>(MAX_CHANNELS));
  for (size_t i = 0; i < frames_here; ++i) {
    for (int c = 0; c < file_ch; ++c) {
      int32_t sample = file_buf[i * file_ch + c];
      if (apply_gain) {
        double scaled = static_cast<double>(sample) * gain;
        scaled = std::clamp(scaled, -2147483648.0, 2147483647.0);
        sample = static_cast<int32_t>(scaled);
      }
      int dest = c + offset;
      if (dest >= 0 && dest < stream_ch) {
        out[i * stream_ch + dest] = sample;
      }
      // Il VU meter riflette il segnale DOPO il guadagno: e' quello che
      // arriva davvero al mixer, coerente con lo scopo del controllo
      // volume (evitare di mandare un livello troppo alto).
      if (c < meter_channels) {
        float v = std::fabs(static_cast<float>(sample) / 2147483648.0f);
        if (v > peaks[c]) peaks[c] = v;
      }
    }
  }
  for (int c = 0; c < meter_channels; ++c) {
    ctx->levels->peak[c].store(peaks[c], std::memory_order_relaxed);
  }

  ctx->frames_played->fetch_add(frames_here, std::memory_order_relaxed);
  return paContinue;
}

}  // namespace recorder
