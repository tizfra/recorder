#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace httplib { class Server; }

namespace recorder {

enum class RemoteCommandType {
  RecordStart,
  RecordStop,
  RecordPause,
  RecordResume,
  PlaybackPlay,      // usa file_arg, e opzionalmente int_arg = channel_offset
  PlaybackPlayFolder, // usa opzionalmente file_arg = file da cui partire (altrimenti dal primo)
                       // e int_arg = channel_offset; avvia la playlist con auto-avanzamento
  PlaybackNavigate,   // usa file_arg: ".." per risalire, nome sottocartella per entrarci,
                       // stringa vuota per tornare alla radice (USB o cartella locale)
  PlaybackStop,
  PlaybackPause,
  PlaybackResume,
  PlaybackSeek,       // usa float_arg = secondi
  PlaybackSetVolume,  // usa float_arg = dB (-24..+6)
  PlaybackSetGrouping,  // usa int_arg = 0/1 (disattiva/attiva 'Group split files')
  PlaybackPrev,       // salta alla voce precedente nel browser corrente
  PlaybackNext,       // salta alla voce successiva nel browser corrente
  SwitchToRecordMode,
  SwitchToPlaybackMode,
  Quit,
  Shutdown,
};

struct RemoteCommand {
  RemoteCommandType type;
  std::string file_arg;
  double float_arg = 0.0;
  int int_arg = 0;
};

// Stato pubblicato dal thread GUI ad ogni frame, letto dagli handler HTTP
// per rispondere a GET /api/status. Tutti i campi sono semplici POD/string
// copiati sotto mutex: nessun puntatore a oggetti di proprieta' del thread
// GUI (Recorder/AudioPlayer) attraversa mai il confine tra i due thread.
struct RemoteStatus {
  std::string mode;              // "record" | "playback"
  std::string record_state;      // "idle" | "recording" | "paused" | "stopped"
  double record_elapsed = 0.0;
  std::string record_file;
  uint64_t record_overruns = 0;
  bool has_input_device = false;
  std::string input_device_name;

  std::vector<std::string> playback_files;   // nomi da mostrare (per i gruppi: "base [N files]")
  std::vector<std::string> playback_file_ids;  // stesso ordine di playback_files: nome del primo
                                                // file di ciascuna voce, usato per selezionarla/
                                                // riprodurla (identifica la voce anche se e' un
                                                // gruppo multi-file)
  std::vector<std::string> playback_subdirs; // nomi sottocartelle (senza path) nella cartella corrente
  std::string playback_current_dir;          // percorso corrente relativo alla radice, es. "/" o "/Live/2024"
  bool playback_can_go_up = false;           // true se non si e' gia' alla radice
  std::string playback_current_file;
  std::string playback_state;     // "idle" | "playing" | "paused" | "stopped"
  double playback_position = 0.0;
  float playback_volume_db = -5.0f;
  bool playback_group_split_files = false;
  float cpu_temp_c = -1.0f;  // -1 = non disponibile (es. non e' un Raspberry Pi)
  long ram_used_mb = 0;
  long ram_total_mb = 0;
  double playback_duration = 0.0;
  int playback_channels = 0;
  int playback_channel_offset = 0;
  int output_max_channels = 2;    // canali nativi del device di output, per popolare il selettore
  bool playlist_active = false;   // true se in riproduzione sequenziale con auto-avanzamento

  std::string error_message;
  std::string disk_space;
};

// Restituisce gli indirizzi IPv4 locali del dispositivo (esclude
// loopback e interfacce non attive), utile per mostrare all'utente a
// quale indirizzo connettersi da telefono/tablet. Puo' restituire un
// vettore vuoto se nessuna interfaccia di rete e' disponibile.
std::vector<std::string> get_local_ip_addresses();

class RemoteControl {
 public:
  RemoteControl();
  ~RemoteControl();

  RemoteControl(const RemoteControl&) = delete;
  RemoteControl& operator=(const RemoteControl&) = delete;

  // Avvia il server HTTP su un thread dedicato. Ritorna false se la bind
  // fallisce (es. porta occupata) — in tal caso il resto della GUI
  // continua a funzionare normalmente, semplicemente senza controllo remoto.
  bool start(int port = 8080);
  void stop();

  // Chiamato dal thread GUI ad ogni frame per prelevare (ed eseguire) i
  // comandi arrivati via HTTP dall'ultimo poll. Ritorna un comando alla
  // volta; chiamare in loop finche' non ritorna std::nullopt.
  std::optional<RemoteCommand> poll_command();

  // Chiamato dal thread GUI ad ogni frame per pubblicare lo stato corrente,
  // cosi' gli handler HTTP (su un altro thread) possono rispondere a
  // GET /api/status senza mai toccare Recorder/AudioPlayer direttamente.
  void publish_status(const RemoteStatus& status);

 private:
  void push_command(RemoteCommand cmd);
  RemoteStatus get_status_copy();

  std::unique_ptr<httplib::Server> _server;
  std::thread _server_thread;

  std::mutex _cmd_mutex;
  std::deque<RemoteCommand> _cmd_queue;

  std::mutex _status_mutex;
  RemoteStatus _status;
};

}  // namespace recorder
