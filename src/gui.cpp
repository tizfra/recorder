#include "gui.h"

#define GL_SILENCE_DEPRECATION
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <csignal>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "audio_player.h"
#include "audio_writer.h"
#include "device_list.h"
#include "recorder.h"
#include "remote_control.h"
#include "version.h"

namespace recorder {

static std::atomic<bool> g_gui_running{true};
static void gui_signal_handler(int) { g_gui_running.store(false, std::memory_order_relaxed); }

static void format_time(double seconds, char* buf, size_t len) {
  int total = static_cast<int>(seconds);
  int h = total / 3600;
  int m = (total % 3600) / 60;
  int s = total % 60;
  if (h > 0) {
    std::snprintf(buf, len, "%d:%02d:%02d", h, m, s);
  } else {
    std::snprintf(buf, len, "%02d:%02d", m, s);
  }
}

// Disegna i VU meter verticali per i canali disponibili nell'area del
// contenuto corrente. `lvl` puo' essere nullptr (nessun segnale, es.
// nessun device/player attivo): in quel caso vengono disegnate solo le
// barre vuote. `display_peak` deve essere un array persistente (fornito
// dal chiamante) di almeno MAX_CHANNELS elementi, usato per lo smoothing
// del decadimento tra un frame e l'altro.
static void draw_vu_meters(LevelData* lvl, int fallback_channels, float* display_peak) {
  int meter_channels = lvl ? lvl->channels.load(std::memory_order_relaxed) : fallback_channels;
  if (meter_channels > MAX_CHANNELS) meter_channels = MAX_CHANNELS;

  float meter_avail_w = ImGui::GetContentRegionAvail().x;
  float meter_height = ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeightWithSpacing() - 4;
  if (meter_height < 20) meter_height = 20;

  float spacing = meter_channels > 16 ? 2.0f : 4.0f;
  int layout_channels = std::max(meter_channels, 16);
  float bar_width = layout_channels > 0
      ? (meter_avail_w - spacing * (layout_channels - 1)) / layout_channels
      : 0;
  if (bar_width < 4) bar_width = 4;

  ImVec2 origin = ImGui::GetCursorScreenPos();
  ImDrawList* draw = ImGui::GetWindowDrawList();

  for (int c = 0; c < meter_channels; ++c) {
    float raw = lvl ? lvl->peak[c].load(std::memory_order_relaxed) : 0.0f;
    if (raw >= display_peak[c]) {
      display_peak[c] = raw;
    } else {
      display_peak[c] *= 0.92f;
    }
    float level = display_peak[c];

    float x = origin.x + c * (bar_width + spacing);
    float y_top = origin.y;
    float y_bottom = origin.y + meter_height;

    draw->AddRectFilled(ImVec2(x, y_top), ImVec2(x + bar_width, y_bottom),
                        IM_COL32(40, 40, 40, 255));

    if (level > 0.001f) {
      float bar_h = meter_height * level;
      float bar_y = y_bottom - bar_h;

      ImU32 color;
      if (level < 0.5f) {
        color = IM_COL32(30, 180, 30, 255);
      } else if (level < 0.85f) {
        color = IM_COL32(220, 180, 20, 255);
      } else {
        color = IM_COL32(220, 40, 40, 255);
      }
      draw->AddRectFilled(ImVec2(x, bar_y), ImVec2(x + bar_width, y_bottom), color);
    }
  }

  float label_y = origin.y + meter_height + 2;
  for (int c = 0; c < meter_channels; ++c) {
    char label[8];
    std::snprintf(label, sizeof(label), "%d", c + 1);
    float label_w = ImGui::CalcTextSize(label).x;
    float x = origin.x + c * (bar_width + spacing) + (bar_width - label_w) * 0.5f;
    draw->AddText(ImVec2(x, label_y), IM_COL32(200, 200, 200, 255), label);
  }
}

enum class GuiMode { Record, Playback };

// Disegna un bottone che occupa una frazione della larghezza disponibile,
// pensato per righe di N bottoni affiancati su schermi piccoli (es. il
// touchscreen del Pi). `count` e' il numero di bottoni nella riga.
static bool row_button(const char* label, int index_in_row, int count, float height,
                       float spacing = 6.0f) {
  float avail = ImGui::GetContentRegionAvail().x;
  float w = (avail - spacing * (count - 1)) / count;
  if (index_in_row > 0) ImGui::SameLine(0.0f, spacing);
  return ImGui::Button(label, ImVec2(w, height));
}

int run_gui(const Config& config) {
  std::signal(SIGINT, gui_signal_handler);
  std::signal(SIGTERM, gui_signal_handler);

  if (!glfwInit()) {
    std::fprintf(stderr, "Error: GLFW init failed\n");
    return 1;
  }

#ifdef __APPLE__
  const char* glsl_version = "#version 150";
  auto set_gl_hints = []() {
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);  // niente barra del titolo/bordi
  };
  auto create_window = [&]() {
    set_gl_hints();
    return glfwCreateWindow(640, 360, "Audio Recorder " RECORDER_VERSION, nullptr, nullptr);
  };
#else
  const char* glsl_version = "#version 130";
  auto create_window = [&]() {
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);  // niente barra del titolo/bordi
    GLFWmonitor* primary = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode = primary ? glfwGetVideoMode(primary) : nullptr;
    if (mode) {
      // Finestra grande quanto lo schermo ma NON fullscreen esclusivo
      // (nullptr al posto di `primary`): resta gestita dal window
      // manager, quindi compare sempre e puo' essere minimizzata anche
      // senza la barra del titolo (glfwIconifyWindow non dipende dai
      // controlli di decorazione, funziona comunque via bottone Desktop).
      return glfwCreateWindow(mode->width, mode->height, "Audio Recorder " RECORDER_VERSION, nullptr, nullptr);
    }
    return glfwCreateWindow(640, 360, "Audio Recorder " RECORDER_VERSION, nullptr, nullptr);
  };
#endif

  // On some systems (Pi), the first window after boot doesn't render.
  // Create and destroy a throwaway window to prime the GPU/compositor.
  GLFWwindow* warmup = create_window();
  if (warmup) {
    glfwMakeContextCurrent(warmup);
    glfwSwapBuffers(warmup);
    glfwDestroyWindow(warmup);
  }

  GLFWwindow* window = create_window();
  if (!window) {
    std::fprintf(stderr, "Error: failed to create window\n");
    glfwTerminate();
    return 1;
  }
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();
  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init(glsl_version);

  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;

  {
    const char* font_paths[] = {
#ifdef __APPLE__
      "/System/Library/Fonts/SFNS.ttf",
      "/System/Library/Fonts/Helvetica.ttc",
#else
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
#endif
    };
    bool loaded = false;
    for (auto* path : font_paths) {
      if (io.Fonts->AddFontFromFileTTF(path, 16.0f)) {
        loaded = true;
        break;
      }
    }
    if (!loaded) {
      io.Fonts->AddFontDefault();
    }
  }

  // Mutable config for device hot-switching
  Config active_config = config;

  // Recorder is created on-demand when Record is pressed
  std::unique_ptr<Recorder> rec;

  // Device scanning state
  std::set<std::string> known_devices;
  auto last_scan = std::chrono::steady_clock::now();
  constexpr double scan_interval_secs = 2.0;

  // Seed known devices from initial scan
  {
    auto devices = scan_input_devices();
    for (auto& d : devices) {
      known_devices.insert(d.name);
    }
  }

  // Track selected device name for display
  std::string selected_device_name;
  int selected_channels = active_config.channels;
  // Un elemento per canale del device corrente; true = incluso nella
  // prossima registrazione. Tutti true di default (registra tutto, come
  // prima dell'introduzione di questa funzionalita'). Ridimensionato e
  // ripristinato a tutti true ogni volta che cambia il device attivo.
  std::vector<bool> record_channel_selected;
  {
    auto preferred = find_preferred_device();
    if (preferred) {
      active_config.device_index = preferred->index;
      active_config.channels = preferred->max_input_channels;
      selected_device_name = preferred->name;
      selected_channels = preferred->max_input_channels;
    }
  }
  record_channel_selected.assign(std::max(active_config.channels, 0), true);

  // Calcola l'elenco di canali selezionati per la prossima registrazione:
  // vuoto se sono selezionati tutti (attiva il percorso rapido in
  // Recorder, identico al comportamento pre-selezione), altrimenti gli
  // indici 0-based dei canali spuntati. Usata sia dal bottone Record in
  // GUI sia dal comando RecordStart via controllo remoto.
  auto compute_record_channels = [&]() -> std::vector<int> {
    std::vector<int> sel;
    for (int c = 0; c < active_config.channels; ++c) {
      if (c < static_cast<int>(record_channel_selected.size()) && record_channel_selected[c]) {
        sel.push_back(c);
      }
    }
    if (static_cast<int>(sel.size()) == active_config.channels) return {};  // tutti selezionati
    return sel;
  };

  // Track current USB disk path
  std::string current_usb_disk = find_usb_disk();
  std::string output_basename = std::filesystem::path(active_config.output_file_base).filename().string();

  float split_minutes = static_cast<float>(active_config.split_seconds / 60.0);

  std::string disk_space_str;
  auto update_disk_space = [&]() {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto dir = fs::path(active_config.output_file).parent_path();
    if (dir.empty()) dir = ".";
    auto si = fs::space(dir, ec);
    if (ec) { disk_space_str.clear(); return; }
    double bytes = static_cast<double>(si.available);
    char buf[32];
    if (bytes >= 1e12) {
      std::snprintf(buf, sizeof(buf), "%.1f TB free", bytes / 1e12);
    } else {
      std::snprintf(buf, sizeof(buf), "%.1f GB free", bytes / 1e9);
    }
    disk_space_str = buf;
  };
  update_disk_space();

  // Audio monitor for VU meters when not recording
  AudioMonitor monitor;
  if (active_config.device_index >= 0) {
    monitor.start(active_config.device_index, active_config.channels, active_config.sample_rate);
  }

  // Smoothed levels for display (registrazione / monitor live)
  float display_peak[MAX_CHANNELS] = {};

  std::string error_msg;

  // Stats from last completed recording (for display after stop)
  uint64_t last_total_frames = 0;
  uint64_t last_overruns = 0;
  int last_files_written = 0;
  double last_elapsed = 0.0;

  // --- Playback state ---
  GuiMode gui_mode = GuiMode::Record;
  std::unique_ptr<AudioPlayer> player;
  std::vector<std::string> playback_files;
  std::vector<std::string> playback_subdirs;
  // Cartella attualmente sfogliata. Inizializzata sulla radice (la USB
  // se presente, altrimenti la cartella locale) e aggiornata quando si
  // naviga tra sottocartelle o quando la USB viene inserita/rimossa.
  std::string playback_dir = current_usb_disk.empty() ? "." : current_usb_disk;
  std::string last_scanned_dir;  // vuota forza una scansione immediata al primo giro
  int selected_file_idx = -1;
  auto last_file_scan = std::chrono::steady_clock::now();
  float playback_display_peak[MAX_CHANNELS] = {};
  int channel_offset = 0;
  // Persiste tra un file e l'altro (non si resetta creando un nuovo
  // AudioPlayer): di default -5dB, come richiesto per attenuare gli MP3
  // che spesso arrivano al mixer piu' "caldi" delle registrazioni dirette.
  float playback_volume_db = -5.0f;
  int output_max_channels = query_output_channel_count(active_config.device_index);
  bool playlist_mode = false;   // checkbox: se attivo, Play avvia la riproduzione dell'intera cartella
  bool playlist_active = false; // true mentre l'auto-avanzamento e' effettivamente in corso
  int playlist_index = -1;      // index into playback_files of the file playing in the playlist
  float file_list_scroll_delta = 0.0f;  // pending scroll (px) applied to FileList on the next frame

  // --- Remote control (web server per smartphone/tablet) ---
  constexpr int kRemotePort = 8080;
  RemoteControl remote;
  bool remote_ok = remote.start(kRemotePort);
  std::string remote_addr_display;  // mostrato sia in console che in GUI, es. "192.168.1.23:8080"
  auto last_ip_check = std::chrono::steady_clock::now();
  auto refresh_remote_addr_display = [&]() {
    if (!remote_ok) {
      remote_addr_display.clear();
      return;
    }
    auto ips = get_local_ip_addresses();
    if (ips.empty()) {
      remote_addr_display = "nessuna rete rilevata";
    } else {
      remote_addr_display.clear();
      for (size_t i = 0; i < ips.size(); ++i) {
        if (i > 0) remote_addr_display += "  /  ";
        remote_addr_display += ips[i] + ":" + std::to_string(kRemotePort);
      }
    }
  };
  refresh_remote_addr_display();

  if (!remote_ok) {
    std::fprintf(stderr, "Controllo remoto non disponibile (porta %d occupata?).\n", kRemotePort);
  } else if (remote_addr_display == "nessuna rete rilevata") {
    std::fprintf(stderr, "Controllo remoto avviato sulla porta %d, ma non e' stata rilevata "
                 "nessuna interfaccia di rete attiva.\n", kRemotePort);
  } else {
    std::fprintf(stderr, "Controllo remoto disponibile su http://%s\n", remote_addr_display.c_str());
  }

  while (!glfwWindowShouldClose(window) && g_gui_running.load(std::memory_order_relaxed)) {
    glfwWaitEventsTimeout(1.0 / 20.0);

    bool is_idle = !rec || rec->state() == Recorder::State::Idle ||
                   rec->state() == Recorder::State::Stopped;

    // --- Device hot-detection while not recording ---
    if (is_idle && gui_mode == GuiMode::Record) {
      auto now = std::chrono::steady_clock::now();
      double since_scan = std::chrono::duration<double>(now - last_scan).count();
      if (since_scan >= scan_interval_secs) {
        last_scan = now;
        auto devices = scan_input_devices();

        // Check for newly appeared USB devices
        for (auto& d : devices) {
          if (known_devices.find(d.name) == known_devices.end()) {
            known_devices.insert(d.name);
            if (is_usb_device(d.name)) {
              active_config.device_index = d.index;
              active_config.channels = d.max_input_channels;
              selected_device_name = d.name;
              selected_channels = d.max_input_channels;
              record_channel_selected.assign(active_config.channels, true);
              rec.reset();
              monitor.stop();
              monitor.start(d.index, d.max_input_channels, active_config.sample_rate);
              error_msg.clear();
              output_max_channels = query_output_channel_count(active_config.device_index);
              std::fprintf(stderr, "New USB device detected: %s (%dch)\n", d.name.c_str(),
                           d.max_input_channels);
            }
          }
        }

        // Update known set and detect removal of selected device
        std::set<std::string> current_names;
        for (auto& d : devices) {
          current_names.insert(d.name);
        }

        if (!selected_device_name.empty() &&
            current_names.find(selected_device_name) == current_names.end()) {
          std::fprintf(stderr, "Device removed: %s\n", selected_device_name.c_str());
          rec.reset();
          monitor.stop();

          auto fallback = find_preferred_device();
          if (fallback) {
            active_config.device_index = fallback->index;
            active_config.channels = fallback->max_input_channels;
            selected_device_name = fallback->name;
            selected_channels = fallback->max_input_channels;
            record_channel_selected.assign(active_config.channels, true);
            monitor.start(fallback->index, fallback->max_input_channels, active_config.sample_rate);
            output_max_channels = query_output_channel_count(active_config.device_index);
            std::fprintf(stderr, "Switched to: %s (%dch)\n", fallback->name.c_str(),
                         fallback->max_input_channels);
          } else {
            selected_device_name.clear();
            selected_channels = 0;
            active_config.device_index = -1;
            record_channel_selected.clear();
          }
        }

        known_devices = current_names;
      }
    }

    // --- Refresh periodico dell'indirizzo IP mostrato (puo' cambiare, es. Wi-Fi riconnesso) ---
    if (remote_ok) {
      auto now = std::chrono::steady_clock::now();
      if (std::chrono::duration<double>(now - last_ip_check).count() >= 10.0) {
        last_ip_check = now;
        refresh_remote_addr_display();
      }
    }

    // --- USB disk hot-detection (runs always, not just when idle) ---
    {
      auto now = std::chrono::steady_clock::now();
      static auto last_disk_scan = std::chrono::steady_clock::now();
      double since_disk_scan = std::chrono::duration<double>(now - last_disk_scan).count();
      if (since_disk_scan >= 2.0) {
        last_disk_scan = now;
        std::string usb_disk = find_usb_disk();
        if (usb_disk != current_usb_disk) {
          std::string old_disk = current_usb_disk;
          current_usb_disk = usb_disk;
          playback_dir = current_usb_disk.empty() ? "." : current_usb_disk;
          selected_file_idx = -1;
          playlist_active = false;

          if (!usb_disk.empty()) {
            if (!rec) {
              active_config.output_file = unique_filename(usb_disk + "/" + output_basename);
            }
            std::fprintf(stderr, "USB disk detected: %s\n", usb_disk.c_str());
          } else {
            std::fprintf(stderr, "USB disk removed: %s\n", old_disk.c_str());
            if (rec && (rec->state() == Recorder::State::Recording ||
                        rec->state() == Recorder::State::Paused)) {
              std::fprintf(stderr, "Disk removed during recording — stopping.\n");
              rec->stop();
              last_total_frames = rec->total_frames();
              last_overruns = rec->overruns();
              last_files_written = rec->files_written();
              last_elapsed = rec->elapsed_seconds();
              rec.reset();
              monitor.start(active_config.device_index, active_config.channels,
                            active_config.sample_rate);
              error_msg = "USB disk removed. Recording stopped.";
            }
            if (player) {
              playlist_active = false;
              player->stop();
              player.reset();
            }
            active_config.output_file = unique_filename(output_basename);
          }
        }
        update_disk_space();
      }
    }

    // Auto-stop on duration
    if (rec && rec->state() == Recorder::State::Recording && active_config.duration_seconds > 0 &&
        rec->elapsed_seconds() >= active_config.duration_seconds) {
      rec->stop();
    }

    // Auto-stop on writer error
    if (rec && rec->state() == Recorder::State::Recording && rec->has_error()) {
      rec->stop();
      error_msg = "FLAC writer error. Recording stopped.";
    }

    // --- Auto-avanzamento playlist: se il file corrente e' finito da
    // solo (non per uno Stop manuale dell'utente), passa al successivo
    // nella cartella corrente.
    if (playlist_active && player && player->state() == AudioPlayer::State::Stopped &&
        player->finished_naturally()) {
      player->stop();
      player.reset();
      playlist_index++;
      if (playlist_index < static_cast<int>(playback_files.size())) {
        selected_file_idx = playlist_index;
        player = std::make_unique<AudioPlayer>(playback_files[playlist_index], active_config.device_index,
                                                channel_offset, active_config.sample_rate);
        if (!player->open() || !player->start()) {
          error_msg = "Unable to play the next file in the folder.";
          player.reset();
          playlist_active = false;
        }
      } else {
        playlist_active = false;  // fine della cartella
      }
    }

    // --- Esegue i comandi arrivati dal server di controllo remoto ---
    // Ogni comando fa esattamente quello che farebbe il pulsante
    // corrispondente in GUI: eseguito qui, nel thread principale, mai
    // toccando Recorder/AudioPlayer/PortAudio da un altro thread.
    while (auto cmd = remote.poll_command()) {
      Recorder::State rstate = rec ? rec->state() : Recorder::State::Idle;
      switch (cmd->type) {
        case RemoteCommandType::SwitchToRecordMode:
          gui_mode = GuiMode::Record;
          // Il player NON viene fermato: vedi commento sull'analogo
          // toggle locale piu' sopra nel file.
          if (active_config.device_index >= 0 && !monitor.running()) {
            monitor.start(active_config.device_index, active_config.channels, active_config.sample_rate);
          }
          break;
        case RemoteCommandType::SwitchToPlaybackMode:
          if (!(rec && (rstate == Recorder::State::Recording || rstate == Recorder::State::Paused))) {
            gui_mode = GuiMode::Playback;
            monitor.stop();
          }
          break;
        case RemoteCommandType::PlaybackNavigate: {
          std::string root = current_usb_disk.empty() ? "." : current_usb_disk;
          if (cmd->file_arg == "..") {
            if (playback_dir != root) {
              auto parent = std::filesystem::path(playback_dir).parent_path();
              playback_dir = parent.empty() ? "." : parent.string();
            }
          } else if (cmd->file_arg.empty()) {
            playback_dir = root;
          } else {
            for (auto& subdir : playback_subdirs) {
              if (std::filesystem::path(subdir).filename().string() == cmd->file_arg) {
                playback_dir = subdir;
                break;
              }
            }
          }
          break;
        }
        case RemoteCommandType::RecordStart:
          if (!selected_device_name.empty() && (!rec || rstate == Recorder::State::Idle ||
                                                 rstate == Recorder::State::Stopped)) {
            bool any_channel_selected = std::any_of(record_channel_selected.begin(),
                                                     record_channel_selected.end(),
                                                     [](bool b) { return b; });
            if (!any_channel_selected) {
              error_msg = "Select at least one channel to record.";
            } else {
              error_msg.clear();
              playlist_active = false;
              if (player) {
                player->stop();
                player.reset();
              }
              active_config.record_channels = compute_record_channels();
              std::string base = current_usb_disk.empty() ? output_basename
                                                            : current_usb_disk + "/" + output_basename;
              active_config.output_file = unique_filename(base);
              monitor.stop();
              rec = std::make_unique<Recorder>(active_config);
              if (!rec->open() || !rec->start()) {
                error_msg = "Failed to start recording.";
                rec.reset();
                monitor.start(active_config.device_index, active_config.channels, active_config.sample_rate);
              }
            }
          }
          break;
        case RemoteCommandType::RecordStop:
          if (rec && (rstate == Recorder::State::Recording || rstate == Recorder::State::Paused)) {
            rec->stop();
            rec.reset();
            monitor.start(active_config.device_index, active_config.channels, active_config.sample_rate);
            std::string base = current_usb_disk.empty() ? output_basename
                                                          : current_usb_disk + "/" + output_basename;
            active_config.output_file = unique_filename(base);
          }
          break;
        case RemoteCommandType::RecordPause:
          if (rec && rstate == Recorder::State::Recording) rec->pause();
          break;
        case RemoteCommandType::RecordResume:
          if (rec && rstate == Recorder::State::Paused) rec->resume();
          break;
        case RemoteCommandType::PlaybackPlay: {
          std::string full_path = playback_dir + "/" + cmd->file_arg;
          playlist_active = false;
          if (player) { player->stop(); player.reset(); }
          error_msg.clear();
          channel_offset = cmd->int_arg;
          player = std::make_unique<AudioPlayer>(full_path, active_config.device_index,
                                                  channel_offset, active_config.sample_rate);
          if (!player->open() || !player->start()) {
            error_msg = "Unable to play the file.";
            player.reset();
          }
          break;
        }
        case RemoteCommandType::PlaybackPlayFolder: {
          if (player) { player->stop(); player.reset(); }
          error_msg.clear();
          channel_offset = cmd->int_arg;
          // Se e' stato indicato un file di partenza, cerca il suo indice
          // nella lista corrente; altrimenti si parte dal primo file.
          int start_idx = 0;
          if (!cmd->file_arg.empty()) {
            for (int i = 0; i < static_cast<int>(playback_files.size()); ++i) {
              if (std::filesystem::path(playback_files[i]).filename().string() == cmd->file_arg) {
                start_idx = i;
                break;
              }
            }
          }
          if (start_idx < static_cast<int>(playback_files.size())) {
            playlist_active = true;
            playlist_index = start_idx;
            selected_file_idx = start_idx;
            player = std::make_unique<AudioPlayer>(playback_files[start_idx], active_config.device_index,
                                                    channel_offset, active_config.sample_rate);
            if (!player->open() || !player->start()) {
              error_msg = "Unable to play the folder.";
              player.reset();
              playlist_active = false;
            }
          } else {
            error_msg = "No files to play in this folder.";
          }
          break;
        }
        case RemoteCommandType::PlaybackStop:
          playlist_active = false;
          if (player) { player->stop(); player.reset(); }
          break;
        case RemoteCommandType::PlaybackPause:
          if (player) player->pause();
          break;
        case RemoteCommandType::PlaybackResume:
          if (player) player->resume();
          break;
        case RemoteCommandType::PlaybackSeek:
          if (player) player->seek(cmd->float_arg);
          break;
        case RemoteCommandType::PlaybackSetVolume:
          playback_volume_db = std::clamp(static_cast<float>(cmd->float_arg), -24.0f, 6.0f);
          break;
        case RemoteCommandType::Quit:
          if (rec && (rstate == Recorder::State::Recording || rstate == Recorder::State::Paused)) {
            rec->stop();
          }
          if (player) player->stop();
          glfwSetWindowShouldClose(window, GLFW_TRUE);
          break;
        case RemoteCommandType::Shutdown:
          if (rec && (rstate == Recorder::State::Recording || rstate == Recorder::State::Paused)) {
            rec->stop();
          }
          if (player) player->stop();
          std::system("sync");
          std::system("sudo systemctl poweroff");
          glfwSetWindowShouldClose(window, GLFW_TRUE);
          break;
      }
    }

    // Applica il volume corrente al player attivo, se presente. Fatto
    // qui una volta per frame (non nei singoli punti di creazione del
    // player) cosi' copre sia l'inizializzazione di un player appena
    // creato sia i cambi live dello slider durante la riproduzione.
    if (player) {
      player->set_volume_db(playback_volume_db);
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);

    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoScrollbar);

    Recorder::State state = rec ? rec->state() : Recorder::State::Idle;
    bool is_recording = state == Recorder::State::Recording || state == Recorder::State::Paused;
    float btn_h = 40.0f;

    // --- Mode toggle: Record / Playback (disabilitato durante la registrazione,
    // sono mutuamente esclusivi) ---
    if (is_recording) ImGui::BeginDisabled();
    // "##mode" da' un ID interno univoco al radio button, distinto dal
    // Button "Record" mostrato piu' sotto quando gui_mode == Record:
    // stesso testo visibile, nessuna collisione di ID ImGui.
    if (ImGui::RadioButton("Record##mode", gui_mode == GuiMode::Record)) {
      gui_mode = GuiMode::Record;
      // Il player NON viene piu' fermato qui: passare alla schermata
      // Record e' solo un cambio di vista, non deve interrompere una
      // riproduzione in corso. Si ferma solo quando parte davvero una
      // nuova registrazione (vedi il bottone Record piu' sotto).
      if (active_config.device_index >= 0 && !monitor.running()) {
        monitor.start(active_config.device_index, active_config.channels, active_config.sample_rate);
      }
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Playback##mode", gui_mode == GuiMode::Playback)) {
      gui_mode = GuiMode::Playback;
      monitor.stop();  // libera il device di input mentre siamo in playback
    }
    if (is_recording) ImGui::EndDisabled();

    if (!remote_addr_display.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.65f, 0.9f, 1.0f));
      ImGui::TextWrapped("Remote: %s", remote_addr_display.c_str());
      ImGui::PopStyleColor();
    }

    ImGui::Separator();

    if (gui_mode == GuiMode::Record) {
      // ==================== RECORD MODE ====================

      // --- Status dot + text ---
      ImVec4 status_color;
      const char* status_text;
      switch (state) {
        case Recorder::State::Idle:
          status_color = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
          status_text = "Ready";
          break;
        case Recorder::State::Recording:
          status_color = ImVec4(1.0f, 0.2f, 0.2f, 1.0f);
          status_text = "Recording";
          break;
        case Recorder::State::Paused:
          status_color = ImVec4(1.0f, 0.8f, 0.0f, 1.0f);
          status_text = "Paused";
          break;
        case Recorder::State::Stopped:
          status_color = ImVec4(0.4f, 0.7f, 1.0f, 1.0f);
          status_text = "Stopped";
          break;
      }

      float radius = 6.0f;
      ImVec2 dot_pos = ImGui::GetCursorScreenPos();
      ImGui::GetWindowDrawList()->AddCircleFilled(
          ImVec2(dot_pos.x + radius, dot_pos.y + ImGui::GetTextLineHeight() * 0.5f), radius,
          ImGui::ColorConvertFloat4ToU32(status_color));
      ImGui::Dummy(ImVec2(radius * 2 + 4, 0));
      ImGui::SameLine();

      if (rec && state != Recorder::State::Idle) {
        char time_buf[32];
        format_time(rec->elapsed_seconds(), time_buf, sizeof(time_buf));
        ImGui::Text("%s  %s", status_text, time_buf);
        uint64_t overruns = rec->overruns();
        if (overruns > 0) {
          ImGui::SameLine();
          ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "  OVR:%llu",
                             static_cast<unsigned long long>(overruns));
        }
        ImGui::Text("%s", rec->current_file().c_str());
        if (!disk_space_str.empty()) {
          ImGui::SameLine();
          ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), " (%s)", disk_space_str.c_str());
        }
      } else {
        if (!selected_device_name.empty()) {
          int display_bits = format_needs_bit_shift(active_config.output_file)
                                 ? active_config.bit_depth
                                 : 32;
          ImGui::Text("%s  %s  %dch / %dHz / %dbit", status_text, selected_device_name.c_str(),
                      active_config.channels, active_config.sample_rate, display_bits);
          ImGui::Text("%s", active_config.output_file.c_str());
          if (!disk_space_str.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), " (%s)", disk_space_str.c_str());
          }
        } else {
          ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "No input device found");
        }
      }

      // --- Error ---
      if (!error_msg.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", error_msg.c_str());
      }

      // --- Recording channel selection (only editable while idle: changing
      // which channels are captured mid-recording would require reopening
      // both the stream and the file writer) ---
      if (!is_recording && !selected_device_name.empty() && active_config.channels > 0) {
        if (ImGui::CollapsingHeader("Recording Channels")) {
          if (static_cast<int>(record_channel_selected.size()) != active_config.channels) {
            record_channel_selected.assign(active_config.channels, true);
          }
          if (ImGui::Button("All", ImVec2(60, 0))) {
            std::fill(record_channel_selected.begin(), record_channel_selected.end(), true);
          }
          ImGui::SameLine();
          if (ImGui::Button("None", ImVec2(60, 0))) {
            std::fill(record_channel_selected.begin(), record_channel_selected.end(), false);
          }

          constexpr int kCols = 8;
          ImGuiTableFlags table_flags = ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit;
          if (ImGui::BeginTable("ChannelGrid", kCols, table_flags)) {
            for (int c = 0; c < active_config.channels; ++c) {
              ImGui::TableNextColumn();
              // Numero a 2 cifre (zero-padded) cosi' ogni cella ha la
              // stessa larghezza indipendentemente dal canale (1 vs 32).
              char label[8];
              std::snprintf(label, sizeof(label), "%02d", c + 1);
              bool sel = record_channel_selected[c];
              if (ImGui::Checkbox(label, &sel)) {
                record_channel_selected[c] = sel;
              }
            }
            ImGui::EndTable();
          }
        }
      }

      // --- Buttons, riga 1: Record/Pause/Resume, Stop, Eject ---
      bool has_device = !selected_device_name.empty();
      bool can_stop = state == Recorder::State::Recording || state == Recorder::State::Paused;
      bool can_eject = !current_usb_disk.empty() && !is_recording;

      if (state == Recorder::State::Recording) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.6f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.7f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.6f, 0.5f, 0.05f, 1.0f));
        if (row_button("Pause", 0, 3, btn_h)) rec->pause();
        ImGui::PopStyleColor(3);
      } else if (state == Recorder::State::Paused) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.7f, 0.3f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.5f, 0.15f, 1.0f));
        if (row_button("Resume", 0, 3, btn_h)) rec->resume();
        ImGui::PopStyleColor(3);
      } else {
        if (!has_device) ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.7f, 0.3f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.5f, 0.15f, 1.0f));
        if (row_button("Record", 0, 3, btn_h)) {
          bool any_channel_selected = std::any_of(record_channel_selected.begin(),
                                                   record_channel_selected.end(),
                                                   [](bool b) { return b; });
          if (!any_channel_selected) {
            error_msg = "Select at least one channel to record.";
          } else {
            error_msg.clear();
            if (rec) {
              last_total_frames = rec->total_frames();
              last_overruns = rec->overruns();
              last_files_written = rec->files_written();
              last_elapsed = rec->elapsed_seconds();
            }
            // Solo ora, all'avvio vero e proprio di una nuova
            // registrazione, fermiamo un'eventuale riproduzione in corso.
            playlist_active = false;
            if (player) {
              player->stop();
              player.reset();
            }
            active_config.record_channels = compute_record_channels();
            std::string base = current_usb_disk.empty()
                                   ? output_basename
                                   : current_usb_disk + "/" + output_basename;
            active_config.output_file = unique_filename(base);
            monitor.stop();
            rec = std::make_unique<Recorder>(active_config);
            if (!rec->open()) {
              error_msg = "Failed to open audio device.";
              rec.reset();
              monitor.start(active_config.device_index, active_config.channels, active_config.sample_rate);
            } else if (!rec->start()) {
              error_msg = "Failed to start recording.";
              rec.reset();
              monitor.start(active_config.device_index, active_config.channels, active_config.sample_rate);
            }
          }
        }
        ImGui::PopStyleColor(3);
        if (!has_device) ImGui::EndDisabled();
      }

      if (!can_stop) ImGui::BeginDisabled();
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.15f, 0.15f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.25f, 0.25f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.6f, 0.1f, 0.1f, 1.0f));
      if (row_button("Stop", 1, 3, btn_h)) {
        rec->stop();
        last_total_frames = rec->total_frames();
        last_overruns = rec->overruns();
        last_files_written = rec->files_written();
        last_elapsed = rec->elapsed_seconds();
        rec.reset();
        monitor.start(active_config.device_index, active_config.channels, active_config.sample_rate);
        std::string base = current_usb_disk.empty()
                               ? output_basename
                               : current_usb_disk + "/" + output_basename;
        active_config.output_file = unique_filename(base);
      }
      ImGui::PopStyleColor(3);
      if (!can_stop) ImGui::EndDisabled();

      if (!can_eject) ImGui::BeginDisabled();
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.4f, 0.4f, 0.5f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.5f, 0.5f, 0.6f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.3f, 0.3f, 0.4f, 1.0f));
      if (row_button("Eject", 2, 3, btn_h)) {
        // sync to flush writes, then unmount via udisksctl for clean desktop notification
        std::system("sync");
        std::string cmd = "udisksctl unmount -b $(findmnt -n -o SOURCE " + current_usb_disk +
                           ") 2>&1 || umount " + current_usb_disk + " 2>&1";
        FILE* p = popen(cmd.c_str(), "r");
        if (p) {
          char result[512] = {};
          fgets(result, sizeof(result), p);
          int ret = pclose(p);
          if (ret == 0) {
            std::fprintf(stderr, "Ejected: %s\n", current_usb_disk.c_str());
            current_usb_disk.clear();
            active_config.output_file = unique_filename(output_basename);
          } else {
            error_msg = "Eject failed: " + std::string(result);
          }
        }
      }
      ImGui::PopStyleColor(3);
      if (!can_eject) ImGui::EndDisabled();

      // --- Buttons, riga 2: Desktop, Quit, Shutdown ---
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.3f, 0.35f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.4f, 0.4f, 0.45f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.2f, 0.2f, 0.25f, 1.0f));
      if (row_button("Desktop", 0, 3, btn_h)) {
        glfwIconifyWindow(window);
      }
      ImGui::PopStyleColor(3);

      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.35f, 0.35f, 0.35f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.45f, 0.45f, 0.45f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.25f, 0.25f, 0.25f, 1.0f));
      if (row_button("Quit", 1, 3, btn_h)) {
        ImGui::OpenPopup("Confirm Quit");
      }
      ImGui::PopStyleColor(3);

      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.15f, 0.5f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.6f, 0.2f, 0.6f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.4f, 0.1f, 0.4f, 1.0f));
      if (row_button("Shutdown", 2, 3, btn_h)) {
        ImGui::OpenPopup("Confirm Shutdown");
      }
      ImGui::PopStyleColor(3);

      // Confirmation popup (Quit)
      if (ImGui::BeginPopupModal("Confirm Quit", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Quit the application?");
        if (rec && (state == Recorder::State::Recording || state == Recorder::State::Paused)) {
          ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "A recording is in progress: it will be stopped.");
        }
        ImGui::Spacing();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
          ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Quit", ImVec2(120, 0))) {
          if (rec && (state == Recorder::State::Recording || state == Recorder::State::Paused)) {
            rec->stop();  // chiude i file in modo pulito prima di uscire
          }
          glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
        ImGui::EndPopup();
      }

      // Confirmation popup (Shutdown)
      if (ImGui::BeginPopupModal("Confirm Shutdown", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Shut down the Raspberry Pi?");
        if (rec && (state == Recorder::State::Recording || state == Recorder::State::Paused)) {
          ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "A recording is in progress: it will be stopped.");
        }
        ImGui::Spacing();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
          ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Shut Down", ImVec2(120, 0))) {
          if (rec && (state == Recorder::State::Recording || state == Recorder::State::Paused)) {
            rec->stop();
          }
          std::system("sync");
          std::system("sudo systemctl poweroff");
          glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
        ImGui::EndPopup();
      }

      // --- VU Meters ---
      ImGui::Spacing();
      LevelData* lvl = nullptr;
      if (rec && (state == Recorder::State::Recording || state == Recorder::State::Paused)) {
        lvl = &rec->levels();
      } else if (monitor.running()) {
        lvl = &monitor.levels;
      }
      draw_vu_meters(lvl, active_config.channels, display_peak);

    } else {
      // ==================== PLAYBACK MODE ====================

      // Rescan (sottocartelle + file audio) ogni 2s, o immediatamente
      // quando si e' appena navigato in una cartella diversa.
      {
        auto now = std::chrono::steady_clock::now();
        bool dir_changed = (playback_dir != last_scanned_dir);
        if (dir_changed || std::chrono::duration<double>(now - last_file_scan).count() >= 2.0) {
          last_file_scan = now;
          last_scanned_dir = playback_dir;
          playback_files.clear();
          playback_subdirs.clear();
          std::error_code ec;
          for (auto& entry : std::filesystem::directory_iterator(playback_dir, ec)) {
            if (entry.is_directory(ec)) {
              playback_subdirs.push_back(entry.path().string());
            } else {
              auto ext = entry.path().extension().string();
              if (ext == ".wav" || ext == ".flac" || ext == ".mp3") {
                playback_files.push_back(entry.path().string());
              }
            }
          }
          std::sort(playback_subdirs.begin(), playback_subdirs.end());
          std::sort(playback_files.begin(), playback_files.end());
          if (dir_changed) selected_file_idx = -1;
        }
      }

      if (!error_msg.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", error_msg.c_str());
      }

      // Current path shown relative to the root (USB drive or local
      // folder), so the full absolute path isn't exposed on screen.
      std::string playback_root = current_usb_disk.empty() ? "." : current_usb_disk;
      {
        std::error_code ec;
        auto rel = std::filesystem::relative(playback_dir, playback_root, ec);
        std::string rel_str = (!ec && rel.string() != ".") ? "/" + rel.string() : "/";
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s", rel_str.c_str());
      }

      constexpr float kFileListHeight = 130.0f;
      // Narrower child window: leaves room on the right for the two
      // up/down scroll buttons, handy on a touchscreen with no mouse wheel.
      ImGui::BeginChild("FileList", ImVec2(-44, kFileListHeight), true);

      // Applies any scroll requested by the up/down buttons on the
      // previous frame — SetScrollY must be called while this child
      // window is the current one, so it can't happen inside the
      // buttons' own code (they're drawn after EndChild for layout).
      if (file_list_scroll_delta != 0.0f) {
        ImGui::SetScrollY(ImGui::GetScrollY() + file_list_scroll_delta);
        file_list_scroll_delta = 0.0f;
      }

      if (playback_dir != playback_root) {
        if (ImGui::Selectable("[..]")) {
          auto parent = std::filesystem::path(playback_dir).parent_path();
          playback_dir = parent.empty() ? "." : parent.string();
        }
      }

      for (auto& subdir : playback_subdirs) {
        std::string name = std::filesystem::path(subdir).filename().string();
        std::string label = "[" + name + "]";
        if (ImGui::Selectable(label.c_str())) {
          playback_dir = subdir;
        }
      }

      for (int i = 0; i < static_cast<int>(playback_files.size()); ++i) {
        std::string label = std::filesystem::path(playback_files[i]).filename().string();
        if (ImGui::Selectable(label.c_str(), selected_file_idx == i)) {
          selected_file_idx = i;
          playlist_active = false;
          if (player) {
            player->stop();
            player.reset();
          }
        }
      }
      ImGui::EndChild();

      ImGui::SameLine();
      ImGui::BeginGroup();
      float scroll_btn_h = (kFileListHeight - ImGui::GetStyle().ItemSpacing.y) * 0.5f;
      if (ImGui::Button("^", ImVec2(40, scroll_btn_h))) {
        file_list_scroll_delta = -60.0f;
      }
      if (ImGui::Button("v", ImVec2(40, scroll_btn_h))) {
        file_list_scroll_delta = 60.0f;
      }
      ImGui::EndGroup();

      // --- Output channel selector (e.g. "Ch 1-2", "Ch 3-4", ...) ---
      // Disabilitato mentre un player e' attivo: cambiare l'uscita a
      // meta' riproduzione richiederebbe stop + riapertura dello stream.
      // Frecce < / > invece di un menu a tendina: piu' rapido da toccare
      // su schermo piccolo, senza dover aprire/chiudere un popup.
      bool is_playing_state = player && (player->state() == AudioPlayer::State::Playing ||
                                          player->state() == AudioPlayer::State::Paused);
      int max_pairs = std::max(1, output_max_channels / 2);
      int max_offset = (max_pairs - 1) * 2;
      if (channel_offset > max_offset) channel_offset = max_offset;
      if (channel_offset < 0) channel_offset = 0;

      if (is_playing_state) ImGui::BeginDisabled();
      ImGui::Text("Output:");
      ImGui::SameLine();
      if (ImGui::Button("<", ImVec2(32, 0)) && channel_offset > 0) {
        channel_offset -= 2;
      }
      ImGui::SameLine();
      ImGui::Text("Ch %d-%d", channel_offset + 1, channel_offset + 2);
      ImGui::SameLine();
      if (ImGui::Button(">", ImVec2(32, 0)) && channel_offset < max_offset) {
        channel_offset += 2;
      }
      if (is_playing_state) ImGui::EndDisabled();

      // Il volume, a differenza del canale di uscita, si puo' cambiare
      // liberamente anche durante la riproduzione: il guadagno si applica
      // in tempo reale nel callback audio, senza bisogno di fermare o
      // riaprire lo stream.
      ImGui::SetNextItemWidth(-1);
      ImGui::SliderFloat("Volume (dB)", &playback_volume_db, -24.0f, 6.0f, "%.1f dB");

      // Se attivo, premere Play avvia la riproduzione sequenziale di
      // tutti i file della cartella a partire da quello selezionato,
      // avanzando automaticamente quando ciascuno finisce da solo.
      if (is_playing_state) ImGui::BeginDisabled();
      ImGui::Checkbox("Play entire folder", &playlist_mode);
      if (is_playing_state) ImGui::EndDisabled();

      // --- Buttons, riga 1: Play/Pause/Resume, Stop ---
      bool has_selection = selected_file_idx >= 0 && selected_file_idx < static_cast<int>(playback_files.size());

      if (!has_selection) ImGui::BeginDisabled();
      if (!player || player->state() == AudioPlayer::State::Stopped ||
          player->state() == AudioPlayer::State::Idle) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.7f, 0.3f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.5f, 0.15f, 1.0f));
        if (row_button("Play", 0, 2, btn_h)) {
          error_msg.clear();
          playlist_active = playlist_mode;
          playlist_index = selected_file_idx;
          player = std::make_unique<AudioPlayer>(playback_files[selected_file_idx],
                                                  active_config.device_index, channel_offset,
                                                  active_config.sample_rate);
          if (!player->open() || !player->start()) {
            error_msg = "Unable to play the file.";
            player.reset();
            playlist_active = false;
          }
        }
        ImGui::PopStyleColor(3);
      } else if (player->state() == AudioPlayer::State::Playing) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.6f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.7f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.6f, 0.5f, 0.05f, 1.0f));
        if (row_button("Pause", 0, 2, btn_h)) player->pause();
        ImGui::PopStyleColor(3);
      } else if (player->state() == AudioPlayer::State::Paused) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.7f, 0.3f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.5f, 0.15f, 1.0f));
        if (row_button("Resume", 0, 2, btn_h)) player->resume();
        ImGui::PopStyleColor(3);
      }
      if (!has_selection) ImGui::EndDisabled();

      if (!is_playing_state) ImGui::BeginDisabled();
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.15f, 0.15f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.25f, 0.25f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.6f, 0.1f, 0.1f, 1.0f));
      if (row_button("Stop", 1, 2, btn_h)) {
        playlist_active = false;
        player->stop();
        player.reset();
      }
      ImGui::PopStyleColor(3);
      if (!is_playing_state) ImGui::EndDisabled();

      // --- Buttons, riga 2: Desktop, Quit ---
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.3f, 0.35f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.4f, 0.4f, 0.45f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.2f, 0.2f, 0.25f, 1.0f));
      if (row_button("Desktop", 0, 2, btn_h)) {
        glfwIconifyWindow(window);
      }
      ImGui::PopStyleColor(3);

      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.35f, 0.35f, 0.35f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.45f, 0.45f, 0.45f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.25f, 0.25f, 0.25f, 1.0f));
      if (row_button("Quit", 1, 2, btn_h)) {
        ImGui::OpenPopup("Confirm Quit");
      }
      ImGui::PopStyleColor(3);

      // Confirmation popup (Quit) — stessa finestra di dialogo usata in
      // Record mode (stesso ID "Confirm Quit"), ma richiamata qui perche'
      // il ramo Record del codice non viene eseguito mentre si e' in
      // Playback mode, quindi il popup non comparirebbe altrimenti.
      if (ImGui::BeginPopupModal("Confirm Quit", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Quit the application?");
        if (player && player->state() == AudioPlayer::State::Playing) {
          ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Playback will be stopped.");
        }
        ImGui::Spacing();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
          ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Quit", ImVec2(120, 0))) {
          if (player) player->stop();
          glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
        ImGui::EndPopup();
      }

      // Seek bar
      if (player) {
        float pos = static_cast<float>(player->position_seconds());
        float dur = static_cast<float>(player->duration_seconds());
        char t1[16], t2[16];
        format_time(pos, t1, sizeof(t1));
        format_time(dur, t2, sizeof(t2));
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##seek", &pos, 0.0f, dur > 0.0f ? dur : 1.0f, "");
        if (ImGui::IsItemDeactivatedAfterEdit()) {
          player->seek(pos);
        }
        ImGui::Text("%s / %s", t1, t2);
      } else {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Select a file and press Play");
      }

      // --- VU Meters ---
      ImGui::Spacing();
      LevelData* play_lvl = (player && player->state() != AudioPlayer::State::Idle)
                                ? &player->levels() : nullptr;
      draw_vu_meters(play_lvl, player ? player->channels() : 0, playback_display_peak);
    }

    ImGui::End();
    ImGui::Render();

    int display_w, display_h;
    glfwGetFramebufferSize(window, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    glfwSwapBuffers(window);

    // --- Pubblica lo stato per il server di controllo remoto ---
    if (remote_ok) {
      RemoteStatus st;
      st.mode = (gui_mode == GuiMode::Record) ? "record" : "playback";
      switch (state) {
        case Recorder::State::Idle: st.record_state = "idle"; break;
        case Recorder::State::Recording: st.record_state = "recording"; break;
        case Recorder::State::Paused: st.record_state = "paused"; break;
        case Recorder::State::Stopped: st.record_state = "stopped"; break;
      }
      st.record_elapsed = rec ? rec->elapsed_seconds() : 0.0;
      st.record_file = rec ? rec->current_file() : active_config.output_file;
      st.record_overruns = rec ? rec->overruns() : 0;
      st.has_input_device = !selected_device_name.empty();
      st.input_device_name = selected_device_name;

      for (auto& f : playback_files) {
        st.playback_files.push_back(std::filesystem::path(f).filename().string());
      }
      for (auto& d : playback_subdirs) {
        st.playback_subdirs.push_back(std::filesystem::path(d).filename().string());
      }
      {
        std::string root = current_usb_disk.empty() ? "." : current_usb_disk;
        std::error_code ec;
        auto rel = std::filesystem::relative(playback_dir, root, ec);
        st.playback_current_dir = (!ec && rel.string() != ".") ? "/" + rel.string() : "/";
        st.playback_can_go_up = (playback_dir != root);
      }
      if (selected_file_idx >= 0 && selected_file_idx < static_cast<int>(playback_files.size())) {
        st.playback_current_file = std::filesystem::path(playback_files[selected_file_idx]).filename().string();
      }
      if (player) {
        switch (player->state()) {
          case AudioPlayer::State::Idle: st.playback_state = "idle"; break;
          case AudioPlayer::State::Playing: st.playback_state = "playing"; break;
          case AudioPlayer::State::Paused: st.playback_state = "paused"; break;
          case AudioPlayer::State::Stopped: st.playback_state = "stopped"; break;
        }
        st.playback_position = player->position_seconds();
        st.playback_duration = player->duration_seconds();
        st.playback_channels = player->channels();
      } else {
        st.playback_state = "idle";
      }
      // Pubblicato sempre (non solo dentro "if (player)"): channel_offset
      // e' la variabile app-level usata per il PROSSIMO Play, quindi deve
      // riflettersi anche quando nessun file sta suonando in questo momento.
      st.playback_channel_offset = channel_offset;
      st.output_max_channels = output_max_channels;
      st.playlist_active = playlist_active;
      st.playback_volume_db = playback_volume_db;

      st.error_message = error_msg;
      st.disk_space = disk_space_str;

      remote.publish_status(st);
    }
  }

  remote.stop();
  monitor.stop();
  if (player) {
    player->stop();
    player.reset();
  }

  // Clean up if still recording when window is closed
  if (rec) {
    auto s = rec->state();
    if (s == Recorder::State::Recording || s == Recorder::State::Paused) {
      rec->stop();
    }

    auto stats_frames = rec->total_frames();
    if (stats_frames > 0) {
      double duration = static_cast<double>(stats_frames) / active_config.sample_rate;
      std::fprintf(stderr, "\nDone. %llu frames (%.1fs)",
                   static_cast<unsigned long long>(stats_frames), duration);
      int fc = rec->files_written();
      if (fc > 1) {
        std::fprintf(stderr, " across %d files", fc);
      }
      std::fprintf(stderr, "\n");
      auto ov = rec->overruns();
      if (ov > 0) {
        std::fprintf(stderr, "Warning: %llu frames dropped (buffer overrun)\n",
                     static_cast<unsigned long long>(ov));
      }
    }
  }

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(window);
  glfwTerminate();

  return 0;
}

}  // namespace recorder
