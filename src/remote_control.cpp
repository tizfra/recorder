#include "remote_control.h"

#include <httplib.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <sstream>
#include <thread>

namespace recorder {

namespace {

// Escape minimale per stringhe dentro JSON — sufficiente per nomi di file
// e messaggi di errore che non contengono caratteri di controllo esotici.
std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += c;
    }
  }
  return out;
}

std::string status_to_json(const RemoteStatus& s) {
  std::ostringstream j;
  j << "{";
  j << "\"mode\":\"" << json_escape(s.mode) << "\",";
  j << "\"record_state\":\"" << json_escape(s.record_state) << "\",";
  j << "\"record_elapsed\":" << s.record_elapsed << ",";
  j << "\"record_file\":\"" << json_escape(s.record_file) << "\",";
  j << "\"record_overruns\":" << s.record_overruns << ",";
  j << "\"has_input_device\":" << (s.has_input_device ? "true" : "false") << ",";
  j << "\"input_device_name\":\"" << json_escape(s.input_device_name) << "\",";

  j << "\"playback_files\":[";
  for (size_t i = 0; i < s.playback_files.size(); ++i) {
    if (i > 0) j << ",";
    j << "\"" << json_escape(s.playback_files[i]) << "\"";
  }
  j << "],";
  j << "\"playback_subdirs\":[";
  for (size_t i = 0; i < s.playback_subdirs.size(); ++i) {
    if (i > 0) j << ",";
    j << "\"" << json_escape(s.playback_subdirs[i]) << "\"";
  }
  j << "],";
  j << "\"playback_current_dir\":\"" << json_escape(s.playback_current_dir) << "\",";
  j << "\"playback_can_go_up\":" << (s.playback_can_go_up ? "true" : "false") << ",";
  j << "\"playback_current_file\":\"" << json_escape(s.playback_current_file) << "\",";
  j << "\"playback_state\":\"" << json_escape(s.playback_state) << "\",";
  j << "\"playback_position\":" << s.playback_position << ",";
  j << "\"playback_duration\":" << s.playback_duration << ",";
  j << "\"playback_channels\":" << s.playback_channels << ",";
  j << "\"playback_channel_offset\":" << s.playback_channel_offset << ",";
  j << "\"output_max_channels\":" << s.output_max_channels << ",";
  j << "\"playlist_active\":" << (s.playlist_active ? "true" : "false") << ",";

  j << "\"error_message\":\"" << json_escape(s.error_message) << "\",";
  j << "\"disk_space\":\"" << json_escape(s.disk_space) << "\"";
  j << "}";
  return j.str();
}

// Estrae un campo stringa/numero molto semplice da un body JSON piatto,
// tipo {"file":"take1.wav","channel_offset":4}. Non e' un parser JSON
// completo (niente oggetti/array annidati, niente escape complessi) ma
// e' sufficiente per il body dei pochi endpoint POST di questa API.
std::optional<std::string> json_get_string(const std::string& body, const std::string& key) {
  std::string needle = "\"" + key + "\"";
  size_t pos = body.find(needle);
  if (pos == std::string::npos) return std::nullopt;
  pos = body.find(':', pos);
  if (pos == std::string::npos) return std::nullopt;
  pos = body.find('"', pos);
  if (pos == std::string::npos) return std::nullopt;
  size_t end = body.find('"', pos + 1);
  if (end == std::string::npos) return std::nullopt;
  return body.substr(pos + 1, end - pos - 1);
}

std::optional<double> json_get_number(const std::string& body, const std::string& key) {
  std::string needle = "\"" + key + "\"";
  size_t pos = body.find(needle);
  if (pos == std::string::npos) return std::nullopt;
  pos = body.find(':', pos);
  if (pos == std::string::npos) return std::nullopt;
  ++pos;
  while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos]))) ++pos;
  size_t end = pos;
  while (end < body.size() && (std::isdigit(static_cast<unsigned char>(body[end])) ||
                                body[end] == '-' || body[end] == '.')) {
    ++end;
  }
  if (end == pos) return std::nullopt;
  try {
    return std::stod(body.substr(pos, end - pos));
  } catch (...) {
    return std::nullopt;
  }
}

// Pagina di controllo, mobile-friendly, servita direttamente dal
// programma — nessun asset esterno da distribuire separatamente.
constexpr const char* kIndexHtml = R"HTML(<!doctype html>
<html lang="it">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Recorder — Controllo remoto</title>
<style>
  body { font-family: -apple-system, system-ui, sans-serif; background:#181818; color:#eee;
         margin:0; padding:16px; }
  h1 { font-size:1.1em; color:#aaa; margin:0 0 12px; }
  .card { background:#242424; border-radius:10px; padding:14px; margin-bottom:14px; }
  .row { display:flex; gap:8px; flex-wrap:wrap; margin:8px 0; }
  button { flex:1; min-width:80px; padding:14px 8px; font-size:1em; border:none;
           border-radius:8px; background:#3a3a3a; color:#eee; }
  button:active { background:#555; }
  button.record { background:#4a1f1f; }
  button.stop { background:#5a1a1a; }
  button.go { background:#1f4a1f; }
  button.danger { background:#4a1f4a; }
  select, input[type=range] { width:100%; padding:8px; border-radius:6px; background:#333;
           color:#eee; border:1px solid #444; }
  .status-line { color:#9c9; font-size:0.95em; margin:4px 0; }
  .error { color:#e66; }
  .filelist { max-height:160px; overflow-y:auto; }
  .filelist div { padding:8px; border-radius:6px; margin:2px 0; }
  .filelist div.sel { background:#2f4f2f; }
  .tabs { display:flex; margin-bottom:14px; }
  .tabs button { border-radius:0; }
  .tabs button.active { background:#2f6f2f; }
</style>
</head>
<body>

<h1>Audio Recorder — remoto</h1>

<div class="tabs">
  <button id="tabRecord" class="active" onclick="setMode('record')">Record</button>
  <button id="tabPlayback" onclick="setMode('playback')">Playback</button>
</div>

<div id="recordCard" class="card">
  <div class="status-line" id="recStatus">—</div>
  <div class="row">
    <button class="record" onclick="cmd('record_start')">● Record</button>
    <button onclick="cmd('record_pause')">Pause</button>
    <button onclick="cmd('record_resume')">Resume</button>
    <button class="stop" onclick="cmd('record_stop')">■ Stop</button>
  </div>
</div>

<div id="playbackCard" class="card" style="display:none">
  <div class="status-line" id="playStatus">—</div>
  <div class="status-line" id="pathLine" style="color:#888">/</div>
  <div class="filelist" id="fileList"></div>
  <input type="range" id="seekBar" min="0" max="100" value="0"
         onchange="seek(this.value)">
  <div class="row">
    <select id="channelSelect"></select>
  </div>
  <div class="row">
    <button class="go" onclick="playSelected()">▶ Play</button>
    <button class="go" onclick="playFolder()">▶▶ Tutta la cartella</button>
  </div>
  <div class="row">
    <button onclick="cmd('playback_pause')">Pause</button>
    <button onclick="cmd('playback_resume')">Resume</button>
    <button class="stop" onclick="cmd('playback_stop')">■ Stop</button>
  </div>
</div>

<div class="card">
  <div class="row">
    <button class="danger" onclick="if(confirm('Uscire dal programma?')) cmd('quit')">Quit</button>
    <button class="danger" onclick="if(confirm('Spegnere il Raspberry?')) cmd('shutdown')">Shutdown</button>
  </div>
</div>

<div class="error" id="errorLine"></div>

<script>
let selectedFile = null;

function setMode(mode) {
  document.getElementById('recordCard').style.display = mode === 'record' ? '' : 'none';
  document.getElementById('playbackCard').style.display = mode === 'playback' ? '' : 'none';
  document.getElementById('tabRecord').classList.toggle('active', mode === 'record');
  document.getElementById('tabPlayback').classList.toggle('active', mode === 'playback');
  cmd(mode === 'record' ? 'mode_record' : 'mode_playback');
}

async function cmd(name, body) {
  await fetch('/api/' + name, {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: body ? JSON.stringify(body) : undefined,
  });
  refresh();
}

function playSelected() {
  if (!selectedFile) return;
  const off = parseInt(document.getElementById('channelSelect').value || '0', 10);
  cmd('playback_play', {file: selectedFile, channel_offset: off});
}

function playFolder() {
  const off = parseInt(document.getElementById('channelSelect').value || '0', 10);
  cmd('playback_play_folder', selectedFile ? {file: selectedFile, channel_offset: off} : {channel_offset: off});
}

function navigate(target) {
  selectedFile = null;
  cmd('playback_navigate', {target: target});
}

function seek(value) {
  cmd('playback_seek', {seconds: parseFloat(value)});
}

function fmtTime(s) {
  s = Math.floor(s);
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), sec = s % 60;
  const mm = String(m).padStart(2, '0'), ss = String(sec).padStart(2, '0');
  return h > 0 ? `${h}:${mm}:${ss}` : `${mm}:${ss}`;
}

async function refresh() {
  try {
    const res = await fetch('/api/status');
    const st = await res.json();

    document.getElementById('recStatus').textContent =
      `${st.record_state}  ${fmtTime(st.record_elapsed)}  ${st.record_file || ''}`;
    document.getElementById('playStatus').textContent =
      `${st.playback_state}  ${fmtTime(st.playback_position)} / ${fmtTime(st.playback_duration)}` +
      (st.playback_current_file ? `  ${st.playback_current_file}` : '') +
      (st.playlist_active ? '  [cartella]' : '');
    document.getElementById('errorLine').textContent = st.error_message || '';
    document.getElementById('pathLine').textContent = st.playback_current_dir || '/';

    if (!document.getElementById('seekBar').matches(':active')) {
      document.getElementById('seekBar').max = st.playback_duration || 100;
      document.getElementById('seekBar').value = st.playback_position || 0;
    }

    const list = document.getElementById('fileList');
    list.innerHTML = '';

    if (st.playback_can_go_up) {
      const up = document.createElement('div');
      up.textContent = '⬆ ..';
      up.style.color = '#9cf';
      up.onclick = () => navigate('..');
      list.appendChild(up);
    }

    (st.playback_subdirs || []).forEach(d => {
      const div = document.createElement('div');
      div.textContent = '📁 ' + d;
      div.style.color = '#9cf';
      div.onclick = () => navigate(d);
      list.appendChild(div);
    });

    st.playback_files.forEach(f => {
      const div = document.createElement('div');
      div.textContent = f;
      if (f === selectedFile) div.classList.add('sel');
      div.onclick = () => { selectedFile = f; refresh(); };
      list.appendChild(div);
    });

    const sel = document.getElementById('channelSelect');
    const wantOptions = Math.max(1, Math.floor((st.output_max_channels || 2) / 2));
    if (sel.options.length !== wantOptions) {
      sel.innerHTML = '';
      for (let i = 0; i < wantOptions; i++) {
        const opt = document.createElement('option');
        opt.value = i * 2;
        opt.textContent = `Ch ${i * 2 + 1}-${i * 2 + 2}`;
        sel.appendChild(opt);
      }
    }
  } catch (e) {
    document.getElementById('errorLine').textContent = 'Connessione persa';
  }
}

setInterval(refresh, 1000);
refresh();
</script>
</body>
</html>
)HTML";

}  // namespace

std::vector<std::string> get_local_ip_addresses() {
  std::vector<std::string> result;
  struct ifaddrs* ifaddr = nullptr;
  if (getifaddrs(&ifaddr) != 0) return result;

  for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr) continue;
    if (ifa->ifa_addr->sa_family != AF_INET) continue;          // solo IPv4, piu' semplice da leggere/digitare
    if (ifa->ifa_flags & IFF_LOOPBACK) continue;                 // esclude 127.0.0.1
    if (!(ifa->ifa_flags & IFF_UP)) continue;                    // solo interfacce attive

    char buf[INET_ADDRSTRLEN];
    auto* sa = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
    if (inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf))) {
      result.emplace_back(buf);
    }
  }
  freeifaddrs(ifaddr);
  return result;
}

RemoteControl::RemoteControl() : _server(std::make_unique<httplib::Server>()) {}

RemoteControl::~RemoteControl() { stop(); }

void RemoteControl::push_command(RemoteCommand cmd) {
  std::lock_guard<std::mutex> lock(_cmd_mutex);
  _cmd_queue.push_back(std::move(cmd));
}

std::optional<RemoteCommand> RemoteControl::poll_command() {
  std::lock_guard<std::mutex> lock(_cmd_mutex);
  if (_cmd_queue.empty()) return std::nullopt;
  RemoteCommand cmd = std::move(_cmd_queue.front());
  _cmd_queue.pop_front();
  return cmd;
}

void RemoteControl::publish_status(const RemoteStatus& status) {
  std::lock_guard<std::mutex> lock(_status_mutex);
  _status = status;
}

RemoteStatus RemoteControl::get_status_copy() {
  std::lock_guard<std::mutex> lock(_status_mutex);
  return _status;
}

bool RemoteControl::start(int port) {
  _server->Get("/", [](const httplib::Request&, httplib::Response& res) {
    res.set_content(kIndexHtml, "text/html; charset=utf-8");
  });

  _server->Get("/api/status", [this](const httplib::Request&, httplib::Response& res) {
    res.set_content(status_to_json(get_status_copy()), "application/json");
  });

  auto simple_post = [this](RemoteCommandType type) {
    return [this, type](const httplib::Request&, httplib::Response& res) {
      push_command(RemoteCommand{type});
      res.set_content("{\"ok\":true}", "application/json");
    };
  };

  _server->Post("/api/record_start", simple_post(RemoteCommandType::RecordStart));
  _server->Post("/api/record_stop", simple_post(RemoteCommandType::RecordStop));
  _server->Post("/api/record_pause", simple_post(RemoteCommandType::RecordPause));
  _server->Post("/api/record_resume", simple_post(RemoteCommandType::RecordResume));
  _server->Post("/api/playback_stop", simple_post(RemoteCommandType::PlaybackStop));
  _server->Post("/api/playback_pause", simple_post(RemoteCommandType::PlaybackPause));
  _server->Post("/api/playback_resume", simple_post(RemoteCommandType::PlaybackResume));
  _server->Post("/api/mode_record", simple_post(RemoteCommandType::SwitchToRecordMode));
  _server->Post("/api/mode_playback", simple_post(RemoteCommandType::SwitchToPlaybackMode));
  _server->Post("/api/quit", simple_post(RemoteCommandType::Quit));
  _server->Post("/api/shutdown", simple_post(RemoteCommandType::Shutdown));

  _server->Post("/api/playback_play", [this](const httplib::Request& req, httplib::Response& res) {
    RemoteCommand cmd{RemoteCommandType::PlaybackPlay};
    auto file = json_get_string(req.body, "file");
    auto offset = json_get_number(req.body, "channel_offset");
    if (!file) {
      res.status = 400;
      res.set_content("{\"ok\":false,\"error\":\"missing file\"}", "application/json");
      return;
    }
    cmd.file_arg = *file;
    cmd.int_arg = offset ? static_cast<int>(*offset) : 0;
    push_command(std::move(cmd));
    res.set_content("{\"ok\":true}", "application/json");
  });

  _server->Post("/api/playback_play_folder", [this](const httplib::Request& req, httplib::Response& res) {
    RemoteCommand cmd{RemoteCommandType::PlaybackPlayFolder};
    auto file = json_get_string(req.body, "file");    // opzionale: file da cui partire
    auto offset = json_get_number(req.body, "channel_offset");
    if (file) cmd.file_arg = *file;
    cmd.int_arg = offset ? static_cast<int>(*offset) : 0;
    push_command(std::move(cmd));
    res.set_content("{\"ok\":true}", "application/json");
  });

  _server->Post("/api/playback_navigate", [this](const httplib::Request& req, httplib::Response& res) {
    RemoteCommand cmd{RemoteCommandType::PlaybackNavigate};
    auto target = json_get_string(req.body, "target");
    cmd.file_arg = target ? *target : "";
    push_command(std::move(cmd));
    res.set_content("{\"ok\":true}", "application/json");
  });

  _server->Post("/api/playback_seek", [this](const httplib::Request& req, httplib::Response& res) {
    auto seconds = json_get_number(req.body, "seconds");
    if (!seconds) {
      res.status = 400;
      res.set_content("{\"ok\":false,\"error\":\"missing seconds\"}", "application/json");
      return;
    }
    RemoteCommand cmd{RemoteCommandType::PlaybackSeek};
    cmd.float_arg = *seconds;
    push_command(std::move(cmd));
    res.set_content("{\"ok\":true}", "application/json");
  });

  _server_thread = std::thread([this, port]() {
    if (!_server->listen("0.0.0.0", port)) {
      std::fprintf(stderr, "RemoteControl: impossibile aprire la porta %d\n", port);
    }
  });

  // Piccola pausa per far emergere subito un eventuale fallimento di bind
  // (es. porta occupata) prima di ritornare start() come "riuscito".
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  return _server->is_running() || _server_thread.joinable();
}

void RemoteControl::stop() {
  if (_server) _server->stop();
  if (_server_thread.joinable()) _server_thread.join();
}

}  // namespace recorder
