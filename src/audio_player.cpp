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

AudioPlayer::AudioPlayer(std::string file_path, int device_index, int channel_offset,
                         int output_sample_rate)
    : _path(std::move(file_path)), _device_index(device_index), _channel_offset(channel_offset),
      _stream_sample_rate(output_sample_rate) {}

AudioPlayer::~AudioPlayer() {
  stop();
  if (_pa_initialized) Pa_Terminate();
}

bool AudioPlayer::open() {
  _reader = create_reader(_path);
  if (!_reader) {
    std::fprintf(stderr, "AudioPlayer: impossibile aprire il file '%s'\n", _path.c_str());
    return false;
  }

  _channels = _reader->channels();
  _file_sample_rate = _reader->sample_rate();
  _stream_sample_rate = (_stream_sample_rate > 0) ? _stream_sample_rate : _file_sample_rate;
  _total_frames = _reader->total_frames();
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
  // [channel_offset, channel_offset + file_channels), silenzio altrove.
  int stream_channels = info->maxOutputChannels;

  if (_channel_offset < 0 || _channel_offset + _channels > stream_channels) {
    if (_channel_offset != 0) {
      std::fprintf(stderr, "AudioPlayer: channel_offset %d non valido per %d canali file / "
                   "%d canali stream, uso 0\n", _channel_offset, _channels, stream_channels);
    }
    _channel_offset = 0;
  }

  if (stream_channels < _channels + _channel_offset) {
    std::fprintf(stderr, "AudioPlayer: device ha solo %d canali di output, servirebbero %d "
                 "(offset %d + %d canali file) — alcuni canali non verranno riprodotti\n",
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
  if (!_reader || _file_sample_rate <= 0) return;

  uint64_t frame = static_cast<uint64_t>(std::max(0.0, seconds) * _file_sample_rate);
  frame = std::min(frame, _total_frames);

  // Mettiamo in pausa: il reader thread smette di scrivere e il
  // pa_callback, quando il ring si svuota, riproduce silenzio senza
  // leggere oltre. Cosi' possiamo ricreare il ring senza race.
  State prev = _state.load(std::memory_order_relaxed);
  _state.store(State::Paused, std::memory_order_relaxed);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  _reader->seek(frame);
  // Il conteggio "frames played" avanza in unita' di frame di STREAM
  // (post-resampling): riproiettiamo il frame di seek dal dominio file
  // al dominio stream, cosi' la seek bar riparte dal punto corretto.
  uint64_t stream_frame = _file_sample_rate > 0
      ? static_cast<uint64_t>(frame * (static_cast<double>(_stream_sample_rate) / _file_sample_rate))
      : frame;
  _frames_played.store(stream_frame, std::memory_order_relaxed);
  _resample_pos = 0.0;
  _ring = std::make_unique<SpscRingBuffer<int32_t>>(_ring->capacity());
  _cb_ctx.ring = _ring.get();

  if (prev == State::Playing) _state.store(State::Playing, std::memory_order_relaxed);
}

void AudioPlayer::set_volume_db(float db) {
  float clamped = std::clamp(db, -60.0f, 12.0f);
  _volume_db.store(clamped, std::memory_order_relaxed);
}

double AudioPlayer::position_seconds() const {
  return _stream_sample_rate > 0
      ? static_cast<double>(_frames_played.load(std::memory_order_relaxed)) / _stream_sample_rate
      : 0.0;
}

void AudioPlayer::reader_thread_func() {
  std::vector<int32_t> chunk(kChunkFrames * _channels);
  std::vector<int32_t> resampled;  // riusato ogni iterazione, ridimensionato al bisogno

  bool need_resample = (_stream_sample_rate != _file_sample_rate) && _file_sample_rate > 0;
  double ratio = need_resample
      ? static_cast<double>(_file_sample_rate) / static_cast<double>(_stream_sample_rate)
      : 1.0;

  while (_reader_running.load(std::memory_order_relaxed)) {
    if (_state.load(std::memory_order_relaxed) == State::Paused) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }

    // Stima quanti frame di OUTPUT produrra' un chunk di kChunkFrames frame
    // di input, per non riempire il ring oltre lo spazio disponibile.
    size_t est_output_frames = need_resample
        ? static_cast<size_t>(kChunkFrames / ratio) + 2
        : kChunkFrames;
    if (_ring->write_available() < est_output_frames * static_cast<size_t>(_channels)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }

    size_t got = _reader->read_samples(chunk.data(), kChunkFrames);
    if (got == 0) {
      _reader_running.store(false, std::memory_order_relaxed);
      _finished_naturally.store(true, std::memory_order_relaxed);
      _state.store(State::Stopped, std::memory_order_relaxed);
      break;
    }

    if (!need_resample) {
      _ring->write(chunk.data(), got * _channels);
      continue;
    }

    // Resampling lineare: `pos` e' la posizione frazionaria (in frame del
    // FILE) del prossimo campione da interpolare, in [0, ratio) all'inizio
    // di ogni chunk (il resto viene riportato dal chunk precedente in
    // _resample_pos). Genera campioni finche' pos rimane dentro al chunk
    // corrente; l'avanzo frazionario viene salvato per il prossimo giro.
    resampled.clear();
    double pos = _resample_pos;
    while (pos < static_cast<double>(got)) {
      size_t i0 = static_cast<size_t>(pos);
      size_t i1 = std::min(i0 + 1, got - 1);
      double frac = pos - static_cast<double>(i0);
      for (int c = 0; c < _channels; ++c) {
        int32_t s0 = chunk[i0 * _channels + c];
        int32_t s1 = chunk[i1 * _channels + c];
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
  int file_ch = ctx->file_channels;
  int stream_ch = ctx->stream_channels;
  int offset = ctx->channel_offset;

  // Silenzio su tutto il buffer di output prima di riempirlo: copre sia
  // gli under-run (dati mancanti dal ring) sia i canali "extra" oltre
  // [offset, offset+file_ch) che il device richiede ma il file non usa.
  std::memset(out, 0, static_cast<size_t>(frame_count) * stream_ch * sizeof(int32_t));

  // In pausa: silenzio immediato (gia' scritto sopra) e usciamo SENZA
  // leggere dal ring buffer. Il reader thread nel frattempo smette di
  // riempirlo (vedi reader_thread_func), quindi il contenuto gia'
  // pre-caricato resta congelato — alla ripresa si riparte esattamente
  // da li', senza salti in avanti ne' click. Se invece continuassimo a
  // drenare il ring anche in pausa, si sentirebbe ancora l'audio gia'
  // bufferizzato (fino a ~1s) prima del silenzio effettivo.
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
