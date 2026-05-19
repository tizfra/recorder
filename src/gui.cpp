#include "gui.h"

#define GL_SILENCE_DEPRECATION
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <csignal>
#include <filesystem>
#include <memory>
#include <set>
#include <string>

#include "device_list.h"
#include "recorder.h"

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

int run_gui(const Config& config) {
  std::signal(SIGINT, gui_signal_handler);
  std::signal(SIGTERM, gui_signal_handler);

  if (!glfwInit()) {
    std::fprintf(stderr, "Error: GLFW init failed\n");
    return 1;
  }

#ifdef __APPLE__
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
  const char* glsl_version = "#version 150";
#else
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
  const char* glsl_version = "#version 130";
#endif

  glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
#ifdef __linux__
  GLFWmonitor* primary = glfwGetPrimaryMonitor();
  const GLFWvidmode* mode = primary ? glfwGetVideoMode(primary) : nullptr;
  GLFWwindow* window = nullptr;
  if (mode) {
    window = glfwCreateWindow(mode->width, mode->height, "Audio Recorder", primary, nullptr);
  } else {
    window = glfwCreateWindow(640, 360, "Audio Recorder", nullptr, nullptr);
  }
#else
  GLFWwindow* window = glfwCreateWindow(640, 360, "Audio Recorder", nullptr, nullptr);
#endif
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
  {
    auto preferred = find_preferred_device();
    if (preferred) {
      active_config.device_index = preferred->index;
      active_config.channels = preferred->max_input_channels;
      selected_device_name = preferred->name;
      selected_channels = preferred->max_input_channels;
    }
  }

  // Track current USB disk path
  std::string current_usb_disk = find_usb_disk();
  std::string output_basename = std::filesystem::path(active_config.output_file_base).filename().string();

  float split_minutes = static_cast<float>(active_config.split_seconds / 60.0);

  // Audio monitor for VU meters when not recording
  AudioMonitor monitor;
  if (active_config.device_index >= 0) {
    monitor.start(active_config.device_index, active_config.channels, active_config.sample_rate);
  }

  // Smoothed levels for display
  float display_peak[MAX_CHANNELS] = {};

  std::string error_msg;

  // Stats from last completed recording (for display after stop)
  uint64_t last_total_frames = 0;
  uint64_t last_overruns = 0;
  int last_files_written = 0;
  double last_elapsed = 0.0;

  while (!glfwWindowShouldClose(window) && g_gui_running.load(std::memory_order_relaxed)) {
    glfwPollEvents();

    bool is_idle = !rec || rec->state() == Recorder::State::Idle ||
                   rec->state() == Recorder::State::Stopped;

    // --- Device hot-detection while not recording ---
    if (is_idle) {
      auto now = std::chrono::steady_clock::now();
      double since_scan = std::chrono::duration<double>(now - last_scan).count();
      if (since_scan >= scan_interval_secs) {
        last_scan = now;
        monitor.stop();
        auto devices = scan_input_devices();

        // Check for newly appeared USB devices
        for (auto& d : devices) {
          if (known_devices.find(d.name) == known_devices.end()) {
            // New device appeared
            known_devices.insert(d.name);
            if (is_usb_device(d.name)) {
              active_config.device_index = d.index;
              active_config.channels = d.max_input_channels;
              selected_device_name = d.name;
              selected_channels = d.max_input_channels;
              rec.reset();
              monitor.stop();
              monitor.start(d.index, d.max_input_channels, active_config.sample_rate);
              error_msg.clear();
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

          auto fallback = find_preferred_device();
          if (fallback) {
            active_config.device_index = fallback->index;
            active_config.channels = fallback->max_input_channels;
            selected_device_name = fallback->name;
            selected_channels = fallback->max_input_channels;
            std::fprintf(stderr, "Switched to: %s (%dch)\n", fallback->name.c_str(),
                         fallback->max_input_channels);
          } else {
            selected_device_name.clear();
            selected_channels = 0;
            active_config.device_index = -1;
          }
        }

        known_devices = current_names;

        // --- USB disk hot-detection ---
        std::string usb_disk = find_usb_disk();
        if (usb_disk != current_usb_disk) {
          current_usb_disk = usb_disk;
          if (!usb_disk.empty()) {
            active_config.output_file = unique_filename(usb_disk + "/" + output_basename);
            std::fprintf(stderr, "USB disk detected: %s → %s\n", usb_disk.c_str(),
                         active_config.output_file.c_str());
          } else {
            active_config.output_file = unique_filename(output_basename);
            std::fprintf(stderr, "USB disk removed, output: %s\n",
                         active_config.output_file.c_str());
          }
        }

        // Restart monitor with current device
        if (active_config.device_index >= 0) {
          monitor.start(active_config.device_index, active_config.channels, active_config.sample_rate);
        }
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

    Recorder::State state =
        rec ? rec->state() : Recorder::State::Idle;

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

    // Compact info line
    if (rec && state != Recorder::State::Idle) {
      char time_buf[32];
      format_time(rec->elapsed_seconds(), time_buf, sizeof(time_buf));
      std::string cur_file = rec->current_file();
      ImGui::Text("%s  %s  %s", status_text, time_buf, cur_file.c_str());

      uint64_t overruns = rec->overruns();
      if (overruns > 0) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "  OVR:%llu",
                           static_cast<unsigned long long>(overruns));
      }
    } else {
      if (!selected_device_name.empty()) {
        ImGui::Text("%s  %s  %dch/%dHz/%dbit  %s", status_text, selected_device_name.c_str(),
                    active_config.channels, active_config.sample_rate, active_config.bit_depth,
                    active_config.output_file.c_str());
      } else {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "No input device found");
      }
    }

    // --- Error ---
    if (!error_msg.empty()) {
      ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", error_msg.c_str());
    }

    // --- Buttons ---
    bool has_device = !selected_device_name.empty();
    float btn_h = 40.0f;

    bool can_record = has_device && (state == Recorder::State::Idle || state == Recorder::State::Stopped);
    if (!can_record) ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.7f, 0.3f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.5f, 0.15f, 1.0f));
    if (ImGui::Button("Record", ImVec2(130, btn_h))) {
      error_msg.clear();
      if (rec) {
        last_total_frames = rec->total_frames();
        last_overruns = rec->overruns();
        last_files_written = rec->files_written();
        last_elapsed = rec->elapsed_seconds();
      }
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
    ImGui::PopStyleColor(3);
    if (!can_record) ImGui::EndDisabled();

    ImGui::SameLine();

    bool can_pause = state == Recorder::State::Recording || state == Recorder::State::Paused;
    if (!can_pause) ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.6f, 0.1f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.7f, 0.2f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.6f, 0.5f, 0.05f, 1.0f));
    const char* pause_label = state == Recorder::State::Paused ? "Resume" : "Pause";
    if (ImGui::Button(pause_label, ImVec2(130, btn_h))) {
      if (state == Recorder::State::Paused) {
        rec->resume();
      } else {
        rec->pause();
      }
    }
    ImGui::PopStyleColor(3);
    if (!can_pause) ImGui::EndDisabled();

    ImGui::SameLine();

    bool can_stop = state == Recorder::State::Recording || state == Recorder::State::Paused;
    if (!can_stop) ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.15f, 0.15f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.25f, 0.25f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.6f, 0.1f, 0.1f, 1.0f));
    if (ImGui::Button("Stop", ImVec2(130, btn_h))) {
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

    // --- VU Meters (vertical, full width below) ---
    ImGui::Spacing();

    LevelData* lvl = nullptr;
    if (rec && (state == Recorder::State::Recording || state == Recorder::State::Paused)) {
      lvl = &rec->levels();
    } else if (monitor.running()) {
      lvl = &monitor.levels;
    }

    int meter_channels = lvl ? lvl->channels.load(std::memory_order_relaxed) : active_config.channels;
    if (meter_channels > MAX_CHANNELS) meter_channels = MAX_CHANNELS;

    float meter_avail_w = ImGui::GetContentRegionAvail().x;
    float meter_height = ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeightWithSpacing() - 4;
    if (meter_height < 20) meter_height = 20;

    float spacing = meter_channels > 16 ? 2.0f : 4.0f;
    float bar_width = meter_channels > 0
        ? (meter_avail_w - spacing * (meter_channels - 1)) / meter_channels
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

    ImGui::End();
    ImGui::Render();

    int display_w, display_h;
    glfwGetFramebufferSize(window, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    glfwSwapBuffers(window);
  }

  monitor.stop();

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
