#include "gui.h"

#define GL_SILENCE_DEPRECATION
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
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

// Confronto "naturale" tra due stringhe: i blocchi numerici vengono
// confrontati come numeri (quindi "take2" < "take10"), non carattere per
// carattere come farebbe l'ordinamento alfabetico puro (che metterebbe
// "take10" prima di "take2"). Usato per ordinare file/cartelle nel
// browser di riproduzione — da cui derivano anche l'ordine della
// playlist e di Prev/Next.
static bool natural_less(const std::string& a, const std::string& b) {
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    if (std::isdigit(static_cast<unsigned char>(a[i])) &&
        std::isdigit(static_cast<unsigned char>(b[j]))) {
      size_t i0 = i, j0 = j;
      while (i < a.size() && std::isdigit(static_cast<unsigned char>(a[i]))) ++i;
      while (j < b.size() && std::isdigit(static_cast<unsigned char>(b[j]))) ++j;
      std::string num_a = a.substr(i0, i - i0);
      std::string num_b = b.substr(j0, j - j0);
      // Toglie gli zeri iniziali solo per il confronto numerico (es.
      // "007" e "7" valgono lo stesso qui), il testo originale resta
      // intatto per il resto del confronto.
      size_t na = num_a.find_first_not_of('0');
      size_t nb = num_b.find_first_not_of('0');
      std::string trimmed_a = na == std::string::npos ? "0" : num_a.substr(na);
      std::string trimmed_b = nb == std::string::npos ? "0" : num_b.substr(nb);
      if (trimmed_a.size() != trimmed_b.size()) return trimmed_a.size() < trimmed_b.size();
      if (trimmed_a != trimmed_b) return trimmed_a < trimmed_b;
      continue;  // blocco numerico uguale: confronta il resto della stringa
    }
    if (a[i] != b[j]) return a[i] < b[j];
    ++i;
    ++j;
  }
  return (a.size() - i) < (b.size() - j);
}

// Riconosce il pattern "<base>_chNN-MM.<ext>" generato dallo split FLAC
// multicanale (vedi make_channel_suffixed_filename in recorder.cpp).
// Ritorna true e popola base/ext/first_ch se il nome file combacia.
static bool parse_channel_group_filename(const std::string& filename, std::string& base,
                                         std::string& ext, int& first_ch) {
  auto dot = filename.rfind('.');
  if (dot == std::string::npos) return false;
  ext = filename.substr(dot);
  std::string stem = filename.substr(0, dot);

  auto ch_pos = stem.rfind("_ch");
  if (ch_pos == std::string::npos) return false;
  std::string suffix = stem.substr(ch_pos + 3);  // "NN-MM" (o "NN-MM_PP-QQ..." per selezioni
                                                  // non contigue: qui basta il primo blocco)
  auto dash = suffix.find('-');
  if (dash == std::string::npos) return false;
  std::string first_str = suffix.substr(0, dash);
  if (first_str.empty() ||
      !std::all_of(first_str.begin(), first_str.end(),
                   [](unsigned char c) { return std::isdigit(c); })) {
    return false;
  }

  base = stem.substr(0, ch_pos);
  first_ch = std::stoi(first_str);
  return true;
}

// Una voce selezionabile nel browser di riproduzione: quasi sempre un
// singolo file, ma se il raggruppamento e' attivo puo' rappresentare
// PIU' file da riprodurre sincronizzati insieme (uno split FLAC a N
// canali diviso in file da max 8 canali ciascuno per limite di formato —
// qualsiasi N, non solo 4: 2 file da 16ch, 3 da 8+8+8, ecc.).
struct PlaybackEntry {
  std::vector<std::string> files;  // percorsi completi, ordinati per canale crescente
  std::string display_name;
};

// Raggruppa playback_files in voci PlaybackEntry. Se grouping_enabled e'
// false, ogni file resta una voce a se stante (comportamento di sempre).
// Se true, i file che condividono lo stesso "<base>.<ext>" col pattern
// "_chNN-MM" vengono uniti in un'unica voce riproducibile insieme;
// gruppi di un solo file restano trattati come file singoli.
static std::vector<PlaybackEntry> group_playback_files(const std::vector<std::string>& files,
                                                        bool grouping_enabled) {
  std::vector<PlaybackEntry> entries;

  if (!grouping_enabled) {
    for (auto& f : files) {
      PlaybackEntry e;
      e.files = {f};
      e.display_name = std::filesystem::path(f).filename().string();
      entries.push_back(std::move(e));
    }
    return entries;
  }

  std::map<std::string, std::vector<std::pair<int, std::string>>> groups;  // "base|ext" -> [(first_ch, path)]
  std::vector<std::string> standalone;

  for (auto& f : files) {
    std::string filename = std::filesystem::path(f).filename().string();
    std::string base, ext;
    int first_ch = 0;
    if (parse_channel_group_filename(filename, base, ext, first_ch)) {
      groups[base + "|" + ext].emplace_back(first_ch, f);
    } else {
      standalone.push_back(f);
    }
  }

  for (auto& [key, members] : groups) {
    if (members.size() < 2) {
      // Un solo file con quel pattern: nessun gruppo da fare, trattalo
      // come file singolo (es. un file gia' entro il limite canali che
      // per qualche motivo ha comunque un suffisso _chNN-MM nel nome).
      standalone.push_back(members[0].second);
      continue;
    }
    auto sorted_members = members;
    std::sort(sorted_members.begin(), sorted_members.end());  // per first_ch crescente

    PlaybackEntry e;
    for (auto& [ch, path] : sorted_members) e.files.push_back(path);
    std::string base = key.substr(0, key.find('|'));
    std::string base_name = std::filesystem::path(base).filename().string();
    e.display_name = base_name + " [" + std::to_string(sorted_members.size()) + " files]";
    entries.push_back(std::move(e));
  }

  for (auto& f : standalone) {
    PlaybackEntry e;
    e.files = {f};
    e.display_name = std::filesystem::path(f).filename().string();
    entries.push_back(std::move(e));
  }

  std::sort(entries.begin(), entries.end(), [](const PlaybackEntry& a, const PlaybackEntry& b) {
    return natural_less(a.display_name, b.display_name);
  });

  return entries;
}

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

// Legge la temperatura della CPU dal sysfs standard Linux (millesimi di
// grado in /sys/class/thermal/thermal_zone0/temp — su Raspberry Pi
// corrisponde alla temperatura del SoC). Ritorna -1 se il file non
// esiste o non e' leggibile (es. eseguito su un sistema diverso).
static float read_cpu_temp_c() {
  std::ifstream f("/sys/class/thermal/thermal_zone0/temp");
  if (!f) return -1.0f;
  int millidegrees = 0;
  f >> millidegrees;
  if (!f) return -1.0f;
  return millidegrees / 1000.0f;
}

// Legge RAM usata/totale da /proc/meminfo (standard Linux). "Usata" e'
// calcolata come MemTotal - MemAvailable (MemAvailable tiene gia' conto
// di cache/buffer riutilizzabili, e' la stima piu' vicina a "quanto e'
// realmente disponibile" rispetto a MemFree da solo). Ritorna false se
// il file non e' leggibile o manca uno dei due campi.
static bool read_ram_usage_mb(long& used_mb, long& total_mb) {
  std::ifstream f("/proc/meminfo");
  if (!f) return false;

  long mem_total_kb = -1, mem_available_kb = -1;
  std::string key;
  long value;
  std::string unit;
  while (f >> key >> value >> unit) {
    if (key == "MemTotal:") mem_total_kb = value;
    else if (key == "MemAvailable:") mem_available_kb = value;
    if (mem_total_kb >= 0 && mem_available_kb >= 0) break;
  }
  if (mem_total_kb < 0 || mem_available_kb < 0) return false;

  total_mb = mem_total_kb / 1024;
  used_mb = (mem_total_kb - mem_available_kb) / 1024;
  return true;
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
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);  // niente barra del titolo/bordi
    // Su Wayland questo hint da solo spesso non basta: molti compositor
    // (labwc incluso) disegnano le decorazioni lato server per default,
    // e vanno disattivate con una regola nella LORO configurazione (vedi
    // ~/.config/labwc/rc.xml), non solo richieste dal client. Impostiamo
    // qui un app_id esplicito cosi' quella regola puo' riferirsi
    // all'app con certezza, invece di indovinare dal titolo (che include
    // il suffisso di versione ed e' quindi meno affidabile da abbinare).
    if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
      glfwWindowHintString(GLFW_WAYLAND_APP_ID, "audio-recorder");
    }
    GLFWmonitor* primary = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode = primary ? glfwGetVideoMode(primary) : nullptr;
    if (mode) {
      // Finestra grande quanto lo schermo ma NON fullscreen esclusivo
      // (nullptr al posto di `primary`): stesso risultato visivo (senza
      // decorazioni, riempie tutto lo schermo), ma evita lo stato
      // "fullscreen" del protocollo Wayland (xdg_toplevel_set_fullscreen).
      // Su labwc quello stato ha una gestione piu' delicata nella
      // transizione minimizza/ripristina — con l'esclusivo si osservava
      // un ripristino non sempre affidabile al primo tocco dopo aver
      // premuto Desktop. Una finestra normale (anche se grande quanto lo
      // schermo) segue invece il percorso di minimize/restore standard,
      // il piu' testato in assoluto in qualsiasi compositor.
      return glfwCreateWindow(mode->width, mode->height, "Audio Recorder " RECORDER_VERSION, nullptr, nullptr);
    }
    return glfwCreateWindow(640, 360, "Audio Recorder " RECORDER_VERSION, nullptr, nullptr);
  };
#endif

  // On some systems (Pi), the first window after boot doesn't render.
  // Create and destroy a throwaway window to prime the GPU/compositor.
  // Deve essere una finestra piccola e NORMALE (non fullscreen): usare
  // create_window() qui farebbe scattare due richieste di fullscreen
  // esclusivo consecutive all'avvio (una per il warmup, una per la
  // finestra vera), confondendo il window manager e facendo partire
  // l'app gia' minimizzata invece che in primo piano.
  glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
  GLFWwindow* warmup = glfwCreateWindow(64, 64, "warmup", nullptr, nullptr);
  if (warmup) {
    glfwMakeContextCurrent(warmup);
    glfwSwapBuffers(warmup);
    glfwDestroyWindow(warmup);
  }
  glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);  // ripristina il default per la finestra vera

  GLFWwindow* window = create_window();
  if (!window) {
    std::fprintf(stderr, "Error: failed to create window\n");
    glfwTerminate();
    return 1;
  }
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);
  // Con GLFW compilato per Wayland nativo (vedi CMakeLists.txt,
  // GLFW_BUILD_WAYLAND), il compositor (labwc) mette a fuoco
  // automaticamente una finestra appena mappata — su Wayland questa
  // chiamata e' quindi un no-op innocuo. La lasciamo solo come rete di
  // sicurezza nel caso il binario venga eseguito in una sessione X11
  // pura (dove invece l'effetto e' reale).
  glfwFocusWindow(window);

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
  // Tutti i dischi USB scrivibili attualmente rilevati (non solo quello
  // in uso): serve per il selettore quando ce n'e' piu' di uno. Popolata
  // dallo stesso ciclo di scansione che aggiorna current_usb_disk.
  std::vector<std::string> available_usb_disks;
  // Stem senza estensione: l'estensione vera e propria viene aggiunta
  // sotto in base al formato selezionato (record_format_flac), cosi' il
  // toggle FLAC/WAV in GUI puo' ricalcolare output_basename senza dover
  // toccare gli 8 punti del file che gia' la usano cosi' com'e'.
  std::string output_stem = std::filesystem::path(active_config.output_file_base).stem().string();
  if (output_stem.empty()) output_stem = "recording";
  bool record_format_flac = true;  // default FLAC, come richiesto
  std::string output_basename = output_stem + (record_format_flac ? ".flac" : ".wav");
  // Inizializza subito il path di output in base allo stato USB rilevato
  // ORA, non solo quando lo stato cambia in seguito (hot-plug) o quando
  // si preme Record: se la USB era gia' inserita all'avvio, senza questa
  // riga la GUI mostrerebbe il path di default (locale) finche' non
  // scatta un altro evento — pur registrando comunque nel posto giusto,
  // dato che il bottone Record ricalcola active_config.output_file al
  // momento della pressione usando current_usb_disk (gia' corretto).
  active_config.output_file =
      unique_filename(current_usb_disk.empty() ? output_basename
                                                : current_usb_disk + "/" + output_basename);

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

    // Stima il tempo di registrazione residuo in base ai canali
    // EFFETTIVAMENTE selezionati (non tutti quelli del device) e al
    // formato corrente. Per FLAC e' solo una stima: la compressione
    // lossless dipende dal contenuto audio — 60% della dimensione WAV
    // e' una media ragionevole per materiale reale, ma silenzio o
    // rumore molto denso possono discostarsene parecchio.
    auto sel = compute_record_channels();
    int rec_channels = sel.empty() ? active_config.channels : static_cast<int>(sel.size());
    if (rec_channels > 0 && active_config.sample_rate > 0) {
      double bytes_per_sec_wav = static_cast<double>(active_config.sample_rate) * rec_channels *
                                 (active_config.bit_depth / 8.0);
      double bytes_per_sec = record_format_flac ? bytes_per_sec_wav * 0.6 : bytes_per_sec_wav;
      if (bytes_per_sec > 0) {
        double seconds = bytes / bytes_per_sec;
        int total_min = static_cast<int>(seconds / 60.0);
        int h = total_min / 60;
        int m = total_min % 60;
        char time_buf[32];
        if (h > 0) {
          std::snprintf(time_buf, sizeof(time_buf), " (~%dh %dm %s)", h, m,
                        record_format_flac ? "FLAC" : "WAV");
        } else {
          std::snprintf(time_buf, sizeof(time_buf), " (~%dm %s)", m,
                        record_format_flac ? "FLAC" : "WAV");
        }
        disk_space_str += time_buf;
      }
    }
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
  // Voci effettivamente mostrate/selezionabili nel browser: derivate da
  // playback_files, eventualmente raggruppando insieme gli split FLAC
  // multicanale (vedi group_playback_files). selected_file_idx e tutta
  // la logica di Play/Prev/Next/playlist lavorano su QUESTA lista, non
  // direttamente su playback_files.
  std::vector<PlaybackEntry> playback_entries;
  bool group_split_files = false;  // checkbox "Group split files"; off di default
  bool group_split_files_prev = false;  // per rilevare il cambio e ricalcolare subito
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
  // Stato della seek bar, persistente tra un frame e l'altro: mentre
  // seek_dragging e' true (utente sta trascinando lo slider), il valore
  // NON viene piu' sovrascritto dalla posizione reale di riproduzione —
  // altrimenti ogni frame la resetterebbe al punto attuale, dando
  // l'impressione che lo slider "torni indietro" da solo appena rilasciato
  // il dito, invece di seguire il trascinamento.
  float seek_display_pos = 0.0f;
  bool seek_dragging = false;
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

  // Temperatura CPU, aggiornata ogni 3s (cambia lentamente, non serve
  // rileggere il sysfs ad ogni frame).
  float cpu_temp_c = read_cpu_temp_c();
  auto last_temp_check = std::chrono::steady_clock::now();
  long ram_used_mb = 0, ram_total_mb = 0;
  bool ram_available = read_ram_usage_mb(ram_used_mb, ram_total_mb);

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

  // Imposta il disco USB attivo, gestendo tutte le conseguenze del
  // cambio (path di playback, file di output, stop di un'eventuale
  // registrazione/riproduzione in corso se il disco sparisce). Centrale
  // e riusabile da: rilevamento automatico (un disco solo), selezione
  // manuale in GUI (piu' di un disco), comando remoto equivalente.
  auto set_active_usb_disk = [&](const std::string& new_disk) {
    std::string old_disk = current_usb_disk;
    current_usb_disk = new_disk;
    playback_dir = current_usb_disk.empty() ? "." : current_usb_disk;
    selected_file_idx = -1;
    playlist_active = false;

    if (!new_disk.empty()) {
      if (!rec) {
        active_config.output_file = unique_filename(new_disk + "/" + output_basename);
      }
      std::fprintf(stderr, "USB disk selected: %s\n", new_disk.c_str());
    } else {
      if (!old_disk.empty()) {
        std::fprintf(stderr, "USB disk removed/deselected: %s\n", old_disk.c_str());
      }
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
  };

  // Salta alla voce di playback_entries all'indice dato: ferma il player
  // corrente e, se stava effettivamente suonando (non solo selezionato/
  // fermo), avvia subito la riproduzione della nuova voce — cosi'
  // Prev/Next si comportano come ci si aspetta durante l'ascolto. Se la
  // playlist e' attiva, aggiorna anche il suo indice cosi' l'auto-
  // avanzamento continua correttamente da qui in poi. Condivisa tra i
  // pulsanti Prev/Next della GUI locale e gli analoghi comandi remoti.
  auto play_index = [&](int idx) {
    if (idx < 0 || idx >= static_cast<int>(playback_entries.size())) return;
    bool was_playing = player && (player->state() == AudioPlayer::State::Playing ||
                                   player->state() == AudioPlayer::State::Paused);
    if (player) {
      player->stop();
      player.reset();
    }
    selected_file_idx = idx;
    if (playlist_active) playlist_index = idx;
    if (was_playing) {
      error_msg.clear();
      player = std::make_unique<AudioPlayer>(playback_entries[idx].files,
                                              active_config.device_index, channel_offset,
                                              active_config.sample_rate);
      if (!player->open() || !player->start()) {
        error_msg = "Unable to play the file.";
        player.reset();
        playlist_active = false;
      }
    }
  };

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

    // --- Refresh periodico della temperatura CPU e della RAM ---
    {
      auto now = std::chrono::steady_clock::now();
      if (std::chrono::duration<double>(now - last_temp_check).count() >= 3.0) {
        last_temp_check = now;
        cpu_temp_c = read_cpu_temp_c();
        ram_available = read_ram_usage_mb(ram_used_mb, ram_total_mb);
      }
    }

    // --- USB disk hot-detection (runs always, not just when idle) ---
    {
      auto now = std::chrono::steady_clock::now();
      static auto last_disk_scan = std::chrono::steady_clock::now();
      double since_disk_scan = std::chrono::duration<double>(now - last_disk_scan).count();
      if (since_disk_scan >= 2.0) {
        last_disk_scan = now;
        available_usb_disks = scan_usb_disks();

        // Decide quale disco dovrebbe essere quello attivo:
        // - se quello gia' in uso e' ancora presente, non lo tocchiamo
        //   (non vogliamo interrompere una sessione attiva solo perche'
        //   e' comparso un secondo disco);
        // - altrimenti, se c'e' ESATTAMENTE un disco disponibile, lo
        //   selezioniamo da soli — stesso comportamento di sempre quando
        //   ce n'e' uno solo, zero azioni richieste all'utente;
        // - se sono zero o piu' di uno (e nessuno gia' attivo), lasciamo
        //   la selezione vuota: con piu' di un disco la scelta spetta
        //   all'utente tramite il selettore in GUI/remoto.
        bool current_still_present =
            !current_usb_disk.empty() &&
            std::find(available_usb_disks.begin(), available_usb_disks.end(), current_usb_disk) !=
                available_usb_disks.end();
        std::string target_disk = current_usb_disk;
        if (!current_still_present) {
          target_disk = (available_usb_disks.size() == 1) ? available_usb_disks[0] : std::string();
        }

        if (target_disk != current_usb_disk) {
          set_active_usb_disk(target_disk);
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
      if (playlist_index < static_cast<int>(playback_entries.size())) {
        selected_file_idx = playlist_index;
        player = std::make_unique<AudioPlayer>(playback_entries[playlist_index].files,
                                                active_config.device_index, channel_offset,
                                                active_config.sample_rate);
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
          // Il player NON viene fermato al solo cambio vista: si ferma
          // solo quando parte davvero una registrazione (RecordStart /
          // bottone Record).
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
              // Breve pausa: lascia al device USB / ALSA il tempo di
              // rilasciare davvero lo stream di output prima di aprire
              // la cattura, riducendo residui sui canali usati in playback.
              std::this_thread::sleep_for(std::chrono::milliseconds(80));
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
          playlist_active = false;
          if (player) { player->stop(); player.reset(); }
          error_msg.clear();
          channel_offset = cmd->int_arg;

          // Cerca la voce che contiene il file/id indicato (anche dentro
          // un gruppo multi-file), cosi' da riprodurre l'intero gruppo
          // sincronizzato se e' uno split FLAC multicanale, esattamente
          // come fa il Play della GUI locale.
          int found_idx = -1;
          for (int i = 0; i < static_cast<int>(playback_entries.size()) && found_idx < 0; ++i) {
            for (auto& f : playback_entries[i].files) {
              if (std::filesystem::path(f).filename().string() == cmd->file_arg) {
                found_idx = i;
                break;
              }
            }
          }

          if (found_idx < 0) {
            error_msg = "Unable to play the file.";
          } else {
            selected_file_idx = found_idx;
            player = std::make_unique<AudioPlayer>(playback_entries[found_idx].files,
                                                    active_config.device_index, channel_offset,
                                                    active_config.sample_rate);
            if (!player->open() || !player->start()) {
              error_msg = "Unable to play the file.";
              player.reset();
            }
          }
          break;
        }
        case RemoteCommandType::PlaybackPlayFolder: {
          if (player) { player->stop(); player.reset(); }
          error_msg.clear();
          channel_offset = cmd->int_arg;
          // Se e' stato indicato un file di partenza, cerca la voce che
          // lo contiene (anche dentro un gruppo multi-file); altrimenti
          // si parte dalla prima voce.
          int start_idx = 0;
          if (!cmd->file_arg.empty()) {
            for (int i = 0; i < static_cast<int>(playback_entries.size()); ++i) {
              for (auto& f : playback_entries[i].files) {
                if (std::filesystem::path(f).filename().string() == cmd->file_arg) {
                  start_idx = i;
                  goto found_start_idx;
                }
              }
            }
            found_start_idx:;
          }
          if (start_idx < static_cast<int>(playback_entries.size())) {
            playlist_active = true;
            playlist_index = start_idx;
            selected_file_idx = start_idx;
            player = std::make_unique<AudioPlayer>(playback_entries[start_idx].files,
                                                    active_config.device_index, channel_offset,
                                                    active_config.sample_rate);
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
        case RemoteCommandType::PlaybackSetGrouping:
          group_split_files = (cmd->int_arg != 0);
          break;
        case RemoteCommandType::PlaybackPrev:
          play_index(selected_file_idx - 1);
          break;
        case RemoteCommandType::PlaybackNext:
          play_index(selected_file_idx + 1);
          break;
        case RemoteCommandType::SetUsbDisk: {
          bool recording_now =
              rec && (rstate == Recorder::State::Recording || rstate == Recorder::State::Paused);
          if (recording_now) {
            error_msg = "Can't change USB disk while recording.";
            break;
          }
          if (cmd->file_arg.empty()) {
            set_active_usb_disk("");
          } else {
            std::string match;
            for (auto& disk : available_usb_disks) {
              if (std::filesystem::path(disk).filename().string() == cmd->file_arg) {
                match = disk;
                break;
              }
            }
            if (!match.empty()) {
              set_active_usb_disk(match);
            } else {
              error_msg = "USB disk not found.";
            }
          }
          break;
        }
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
      // Il player NON viene fermato al solo cambio vista: si ferma
      // solo quando parte davvero una registrazione (bottone Record).
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

    if (cpu_temp_c >= 0.0f) {
      // Soglie indicative per Raspberry Pi 4: il throttling termico
      // scatta di norma intorno agli 80C.
      ImVec4 temp_color = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
      if (cpu_temp_c >= 75.0f) {
        temp_color = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
      } else if (cpu_temp_c >= 65.0f) {
        temp_color = ImVec4(1.0f, 0.75f, 0.2f, 1.0f);
      }
      ImGui::SameLine();
      ImGui::TextColored(temp_color, "  CPU: %.0f\xC2\xB0" "C", cpu_temp_c);
    }
    if (ram_available) {
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "  RAM: %ld/%ld MB", ram_used_mb,
                         ram_total_mb);
    }

    if (!remote_addr_display.empty()) {
      // Riga propria (non SameLine): con piu' interfacce di rete attive
      // l'indirizzo puo' essere lungo (piu' IP uniti da "/"), TextWrapped
      // lo gestisce andando a capo invece di uscire dai bordi.
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

      // --- USB disk selector: visibile solo con piu' di un disco
      // collegato (con uno solo il comportamento resta automatico come
      // sempre). Non editabile durante una registrazione: cambiare
      // disco a meta' avrebbe bisogno di chiudere/riaprire il writer.
      if (available_usb_disks.size() > 1) {
        if (is_recording) ImGui::BeginDisabled();
        std::string current_label = current_usb_disk.empty()
            ? "(none selected)" : std::filesystem::path(current_usb_disk).filename().string();
        ImGui::SetNextItemWidth(220);
        if (ImGui::BeginCombo("USB Disk", current_label.c_str())) {
          for (auto& disk : available_usb_disks) {
            std::string label = std::filesystem::path(disk).filename().string();
            if (ImGui::Selectable(label.c_str(), disk == current_usb_disk)) {
              set_active_usb_disk(disk);
            }
          }
          ImGui::EndCombo();
        }
        if (is_recording) ImGui::EndDisabled();
      }

      // --- Error ---
      if (!error_msg.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", error_msg.c_str());
      }

      // --- Recording format (FLAC/WAV): solo quando non si sta
      // registrando, cambiarlo a meta' registrazione non avrebbe senso
      // (il file writer e' gia' aperto sul formato scelto all'avvio). ---
      if (!is_recording) {
        ImGui::Text("Format:");
        ImGui::SameLine();
        bool format_changed = false;
        if (ImGui::RadioButton("FLAC", record_format_flac)) {
          record_format_flac = true;
          format_changed = true;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("WAV", !record_format_flac)) {
          record_format_flac = false;
          format_changed = true;
        }
        if (format_changed) {
          output_basename = output_stem + (record_format_flac ? ".flac" : ".wav");
          std::string base = current_usb_disk.empty() ? output_basename
                                                        : current_usb_disk + "/" + output_basename;
          active_config.output_file = unique_filename(base);
          update_disk_space();
        }
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
            update_disk_space();
          }
          ImGui::SameLine();
          if (ImGui::Button("None", ImVec2(60, 0))) {
            std::fill(record_channel_selected.begin(), record_channel_selected.end(), false);
            update_disk_space();
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
                update_disk_space();
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
        // Quote per path con spazi (es. "/media/pi/My USB").
        auto shell_quote = [](const std::string& s) {
          std::string out = "'";
          for (char c : s) {
            if (c == '\'') out += "'\\''";
            else out += c;
          }
          out += "'";
          return out;
        };
        auto trim_nl = [](std::string s) {
          while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
          return s;
        };
        auto run_cmd = [&](const std::string& cmd, std::string* out_line) -> int {
          FILE* p = popen(cmd.c_str(), "r");
          if (!p) return -1;
          char buf[512] = {};
          if (fgets(buf, sizeof(buf), p) && out_line) *out_line = trim_nl(buf);
          return pclose(p);
        };

        // Se stiamo riproducendo dalla USB, l'unmount fallirebbe (busy).
        if (player) {
          playlist_active = false;
          player->stop();
          player.reset();
        }

        // Flush pending writes before unmount.
        std::system("sync");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        const std::string mount_path = current_usb_disk;
        const std::string mount_q = shell_quote(mount_path);

        // Risolve il block device (es. /dev/sda1) dal mount point.
        std::string block_dev;
        {
          std::string line;
          run_cmd("findmnt -n -o SOURCE " + mount_q + " 2>/dev/null", &line);
          block_dev = line;
        }

        // 1) UNMOUNT via udisks/gvfs (mai umount grezzo).
        // 2) POWER-OFF del drive padre = vera espulsione USB (LED off,
        //    device non piu' esportato). Solo dopo unmount verificato.
        int ret = -1;
        std::string result;
        const char* method = nullptr;

        if (!block_dev.empty()) {
          ret = run_cmd("udisksctl unmount -b " + shell_quote(block_dev) + " 2>&1", &result);
          if (ret == 0) method = "udisksctl";
        }
        if (ret != 0) {
          std::string gio_msg;
          int gio = run_cmd("gio mount -e " + mount_q + " 2>&1", &gio_msg);
          if (gio == 0) {
            ret = 0;
            result.clear();
            method = "gio";
          } else {
            std::string gvfs_msg;
            int gvfs = run_cmd("gvfs-mount -u " + mount_q + " 2>&1", &gvfs_msg);
            if (gvfs == 0) {
              ret = 0;
              result.clear();
              method = "gvfs-mount";
            } else {
              if (result.empty()) result = gio_msg;
              if (result.empty()) result = gvfs_msg;
              else if (!gio_msg.empty()) result += " / " + gio_msg;
            }
          }
        }

        if (ret == 0) {
          std::string check;
          run_cmd("findmnt -n " + mount_q + " 2>/dev/null", &check);
          if (!check.empty()) {
            ret = -1;
            result = "still mounted after unmount";
            method = nullptr;
          }
        }

        // Parent disk for power-off (e.g. /dev/sda from /dev/sda1).
        std::string power_dev;
        if (ret == 0 && !block_dev.empty()) {
          std::string pk;
          run_cmd("lsblk -n -o PKNAME " + shell_quote(block_dev) + " 2>/dev/null", &pk);
          if (!pk.empty()) {
            if (pk.rfind("/dev/", 0) != 0) pk = "/dev/" + pk;
            power_dev = pk;
          } else {
            power_dev = block_dev;
          }
        }

        bool powered_off = false;
        std::string po_msg;  // dichiarata qui (non nello scope dell'if sotto) cosi' resta
                              // leggibile piu' in basso, dove decidiamo cosa mostrare
        if (ret == 0) {
          std::system("sync");
          std::this_thread::sleep_for(std::chrono::milliseconds(400));
          if (!power_dev.empty()) {
            int po = run_cmd("udisksctl power-off -b " + shell_quote(power_dev) + " 2>&1",
                             &po_msg);
            if (po == 0) {
              powered_off = true;
            } else {
              std::fprintf(stderr, "Eject: unmount ok (%s), power-off failed (%s): %s\n",
                           method ? method : "?", power_dev.c_str(), po_msg.c_str());
            }
          }
        }

        if (ret == 0 && powered_off) {
          std::fprintf(stderr, "Ejected via %s (power-off=yes): %s (part=%s drive=%s)\n",
                       method ? method : "?", mount_path.c_str(),
                       block_dev.empty() ? "?" : block_dev.c_str(),
                       power_dev.empty() ? "?" : power_dev.c_str());
          current_usb_disk.clear();
          playback_dir = ".";
          selected_file_idx = -1;
          playlist_active = false;
          active_config.output_file = unique_filename(output_basename);
          update_disk_space();
          error_msg.clear();
          ImGui::OpenPopup("USB Ejected");
        } else if (ret == 0 && !powered_off) {
          // L'unmount e' riuscito (il filesystem e' scritto/pulito, puoi
          // navigare altrove senza perdita dati), ma il power-off USB e'
          // fallito: il device resta acceso/autorizzato a livello bus.
          // Scollegarlo ORA equivale comunque a una rimozione non pulita
          // dal punto di vista del sistema — meglio dirlo chiaramente
          // invece di mostrare un falso "espulso con successo".
          std::fprintf(stderr, "Eject: unmount ok (%s) ma power-off fallito (%s): %s\n",
                       method ? method : "?", power_dev.empty() ? "?" : power_dev.c_str(),
                       po_msg.c_str());
          current_usb_disk.clear();
          playback_dir = ".";
          selected_file_idx = -1;
          playlist_active = false;
          active_config.output_file = unique_filename(output_basename);
          update_disk_space();
          std::string reason = power_dev.empty()
              ? "couldn't identify the parent USB device"
              : (po_msg.empty() ? "unknown error" : po_msg);
          error_msg = "Unmounted OK, but couldn't power off the USB device (" + reason +
                     "). Data is safe, but wait a moment or check permissions before unplugging.";
        } else {
          std::string msg = result.empty() ? "unknown error" : result;
          if (msg.find("Not authorized") != std::string::npos ||
              msg.find("not authorized") != std::string::npos ||
              msg.find("Permission denied") != std::string::npos ||
              msg.find("permission denied") != std::string::npos) {
            msg += " (need user session with udisks/polkit; check DBUS_SESSION_BUS_ADDRESS)";
          }
          error_msg = "Eject failed: " + msg;
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

      // Shown after a successful USB eject (unmount).
      if (ImGui::BeginPopupModal("USB Ejected", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("The USB drive was ejected successfully.");
        ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.4f, 1.0f),
                           "It is now safe to unplug the USB drive.");
        ImGui::Spacing();
        if (ImGui::Button("OK", ImVec2(160, 0))) {
          ImGui::CloseCurrentPopup();
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
      bool entries_dirty = false;
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
          std::sort(playback_subdirs.begin(), playback_subdirs.end(), natural_less);
          std::sort(playback_files.begin(), playback_files.end(), natural_less);
          if (dir_changed) selected_file_idx = -1;
          entries_dirty = true;
        }
      }
      // Ricalcola anche se la checkbox di raggruppamento e' appena
      // cambiata, senza aspettare il prossimo rescan periodico.
      if (group_split_files != group_split_files_prev) {
        group_split_files_prev = group_split_files;
        entries_dirty = true;
        selected_file_idx = -1;  // gli indici cambiano significato tra le due modalita'
      }
      if (entries_dirty) {
        playback_entries = group_playback_files(playback_files, group_split_files);
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

      constexpr float kFileListHeight = 100.0f;
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

      for (int i = 0; i < static_cast<int>(playback_entries.size()); ++i) {
        if (ImGui::Selectable(playback_entries[i].display_name.c_str(), selected_file_idx == i)) {
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

      ImGui::SameLine();
      if (ImGui::Checkbox("Group split files", &group_split_files)) {
        // group_split_files_prev viene confrontato piu' sotto nel loop
        // per rilevare il cambio e ricalcolare playback_entries subito,
        // senza aspettare il prossimo rescan periodico.
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Plays multi-file FLAC splits (e.g. 4 files x 8ch) together in sync,\n"
                          "as if they were one recording.");
      }

      // Se attivo, premere Play avvia la riproduzione sequenziale di
      // tutti i file della cartella a partire da quello selezionato,
      // avanzando automaticamente quando ciascuno finisce da solo.
      if (is_playing_state) ImGui::BeginDisabled();
      ImGui::Checkbox("Play entire folder", &playlist_mode);
      if (is_playing_state) ImGui::EndDisabled();

      // Il volume, a differenza del canale di uscita, si puo' cambiare
      // liberamente anche durante la riproduzione: il guadagno si applica
      // in tempo reale nel callback audio, senza bisogno di fermare o
      // riaprire lo stream. Sulla stessa riga della checkbox sopra per
      // risparmiare spazio verticale (lasciato ai VU meter piu' sotto).
      ImGui::SameLine();
      ImGui::SetNextItemWidth(-1);
      ImGui::SliderFloat("Volume (dB)", &playback_volume_db, -24.0f, 6.0f, "%.1f dB");

      // --- Buttons, riga 1: Prev, Play/Pause/Resume, Next, Stop ---
      bool has_selection = selected_file_idx >= 0 && selected_file_idx < static_cast<int>(playback_entries.size());
      bool has_prev = selected_file_idx > 0;
      bool has_next = selected_file_idx >= 0 &&
                      selected_file_idx + 1 < static_cast<int>(playback_entries.size());

      if (!has_prev) ImGui::BeginDisabled();
      if (row_button("Prev", 0, 4, btn_h)) {
        play_index(selected_file_idx - 1);
      }
      if (!has_prev) ImGui::EndDisabled();

      if (!has_selection) ImGui::BeginDisabled();
      if (!player || player->state() == AudioPlayer::State::Stopped ||
          player->state() == AudioPlayer::State::Idle) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.7f, 0.3f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.5f, 0.15f, 1.0f));
        if (row_button("Play", 1, 4, btn_h)) {
          error_msg.clear();
          playlist_active = playlist_mode;
          playlist_index = selected_file_idx;
          player = std::make_unique<AudioPlayer>(playback_entries[selected_file_idx].files,
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
        if (row_button("Pause", 1, 4, btn_h)) player->pause();
        ImGui::PopStyleColor(3);
      } else if (player->state() == AudioPlayer::State::Paused) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.7f, 0.3f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.5f, 0.15f, 1.0f));
        if (row_button("Resume", 1, 4, btn_h)) player->resume();
        ImGui::PopStyleColor(3);
      }
      if (!has_selection) ImGui::EndDisabled();

      if (!has_next) ImGui::BeginDisabled();
      if (row_button("Next", 2, 4, btn_h)) {
        play_index(selected_file_idx + 1);
      }
      if (!has_next) ImGui::EndDisabled();

      if (!is_playing_state) ImGui::BeginDisabled();
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.15f, 0.15f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.25f, 0.25f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.6f, 0.1f, 0.1f, 1.0f));
      if (row_button("Stop", 3, 4, btn_h)) {
        playlist_active = false;
        player->stop();
        player.reset();
      }
      ImGui::PopStyleColor(3);
      if (!is_playing_state) ImGui::EndDisabled();

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

      // Confirmation popup (Shutdown) — stesso ID "Confirm Shutdown"
      // usato in Record mode, stesso motivo del duplicato sopra per Quit.
      if (ImGui::BeginPopupModal("Confirm Shutdown", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Shut down the Raspberry Pi?");
        if (player && player->state() == AudioPlayer::State::Playing) {
          ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Playback will be stopped.");
        }
        ImGui::Spacing();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
          ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Shut Down", ImVec2(120, 0))) {
          if (player) player->stop();
          std::system("sync");
          std::system("sudo systemctl poweroff");
          glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
        ImGui::EndPopup();
      }

      // Seek bar
      if (player) {
        if (!seek_dragging) {
          seek_display_pos = static_cast<float>(player->position_seconds());
        }
        float dur = static_cast<float>(player->duration_seconds());
        char t1[16], t2[16];
        format_time(seek_display_pos, t1, sizeof(t1));
        format_time(dur, t2, sizeof(t2));
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##seek", &seek_display_pos, 0.0f, dur > 0.0f ? dur : 1.0f, "");
        seek_dragging = ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit()) {
          player->seek(seek_display_pos);
        }
        ImGui::Text("%s / %s", t1, t2);
      } else {
        seek_dragging = false;
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
      for (auto& disk : available_usb_disks) {
        st.available_usb_disks.push_back(std::filesystem::path(disk).filename().string());
      }
      st.current_usb_disk = current_usb_disk.empty()
          ? "" : std::filesystem::path(current_usb_disk).filename().string();

      for (auto& entry : playback_entries) {
        st.playback_files.push_back(entry.display_name);
        st.playback_file_ids.push_back(std::filesystem::path(entry.files.front()).filename().string());
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
      if (selected_file_idx >= 0 && selected_file_idx < static_cast<int>(playback_entries.size())) {
        st.playback_current_file = playback_entries[selected_file_idx].display_name;
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
      st.playback_group_split_files = group_split_files;
      st.cpu_temp_c = cpu_temp_c;
      st.ram_used_mb = ram_used_mb;
      st.ram_total_mb = ram_available ? ram_total_mb : 0;

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
