#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "audio_reader.h"
#include "recorder.h"     // LevelData, MAX_CHANNELS
#include "ring_buffer.h"

typedef void PaStream;

namespace recorder {

// Restituisce il numero di canali di output nativi del device associato a
// device_index (o del device di output di default se device_index < 0 o
// non ha canali di output). Utile per popolare un selettore di canale in
// GUI prima di creare un AudioPlayer. Inizializza/termina PortAudio
// internamente, quindi puo' essere chiamata senza che uno stream sia
// gia' aperto altrove.
int query_output_channel_count(int device_index);

class AudioPlayer {
 public:
  enum class State { Idle, Playing, Paused, Stopped };

  // device_index: indice PortAudio da provare per l'output (tipicamente
  // lo stesso device usato per la registrazione). -1 = usa il device di
  // output di default del sistema. Se il device indicato non ha canali
  // di output (comune con interfacce USB dove input/output sono entry
  // separate), open() cerca automaticamente un'altra entry con lo stesso
  // nome che li abbia, prima di ripiegare sul default.
  //
  // channel_offset: canale dello stream (0-based) da cui iniziare a
  // scrivere i campioni del file. Es. per un file stereo instradato sui
  // canali USB 5-6 di un device a 32 canali, offset = 4. Se offset +
  // channels() supera i canali nativi del device, open() lo azzera e
  // stampa un avviso.
  //
  // output_sample_rate: frequenza a cui aprire lo stream PortAudio. 0
  // (default) = usa la frequenza nativa del file. Molti device USB
  // (incluse interfacce multicanale fisse) accettano solo una frequenza
  // specifica (es. 48000 Hz): se il file ha una frequenza diversa (tipico
  // per MP3, spesso a 44100 Hz), passare qui la frequenza del device fa
  // si' che open() attivi un resampler lineare in tempo reale.
  explicit AudioPlayer(std::string file_path, int device_index = -1, int channel_offset = 0,
                       int output_sample_rate = 0);
  ~AudioPlayer();

  AudioPlayer(const AudioPlayer&) = delete;
  AudioPlayer& operator=(const AudioPlayer&) = delete;

  bool open();
  bool start();
  void pause();
  void resume();
  void stop();
  void seek(double seconds);

  State state() const { return _state.load(std::memory_order_relaxed); }
  // True se lo stato e' diventato Stopped perche' il file e' arrivato
  // alla fine da solo (EOF), invece che per una chiamata esplicita a
  // stop(). Utile per l'auto-avanzamento in una playlist: si vuole
  // passare al file successivo solo in questo caso, non quando l'utente
  // ha premuto Stop volontariamente.
  bool finished_naturally() const { return _finished_naturally.load(std::memory_order_relaxed); }
  double position_seconds() const;
  double duration_seconds() const { return _duration_seconds; }
  int channels() const { return _channels; }
  int channel_offset() const { return _channel_offset; }
  LevelData& levels() { return _levels; }

 private:
  struct CallbackContext {
    SpscRingBuffer<int32_t>* ring;
    std::atomic<uint64_t>* frames_played;
    LevelData* levels;
    int file_channels;    // canali del file audio (quelli scritti nel ring dal reader thread)
    int stream_channels;  // canali richiesti dal device PortAudio (puo' essere > file_channels
                           // su device che rifiutano stream con un conteggio canali diverso
                           // da quello nativo, es. interfacce USB multicanale fisse)
    int channel_offset;   // canale dello stream da cui iniziare a scrivere i campioni del file
  };

  std::string _path;
  int _device_index;
  int _channel_offset;
  std::unique_ptr<AudioReader> _reader;
  std::atomic<State> _state{State::Idle};

  int _channels = 0;
  int _file_sample_rate = 0;      // frequenza nativa del file
  int _stream_sample_rate = 0;    // frequenza a cui e' aperto lo stream PortAudio
                                   // (== _file_sample_rate se nessun resampling richiesto)
  uint64_t _total_frames = 0;
  double _duration_seconds = 0.0;

  // Posizione frazionaria (in frame del FILE) del prossimo campione da
  // interpolare, persistente tra una chiamata al reader thread e
  // l'altra: sempre in [0, ratio) dove ratio = file_rate/stream_rate.
  // Azzerata su seek().
  double _resample_pos = 0.0;

  bool _pa_initialized = false;
  PaStream* _stream = nullptr;
  std::unique_ptr<SpscRingBuffer<int32_t>> _ring;
  CallbackContext _cb_ctx{};

  std::thread _reader_thread;
  std::atomic<bool> _reader_running{false};
  std::atomic<uint64_t> _frames_played{0};
  std::atomic<bool> _finished_naturally{false};

  LevelData _levels;

  static int pa_callback(const void* input, void* output, unsigned long frame_count,
                         const void* time_info, unsigned long flags, void* user_data);
  void reader_thread_func();
};

}  // namespace recorder
