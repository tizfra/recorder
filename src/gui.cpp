#include "gui.h"

#define GL_SILENCE_DEPRECATION
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>

#include <atomic>
#include <cstdio>
#include <csignal>
#include <string>

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
  GLFWwindow* window = glfwCreateWindow(480, 320, "Audio Recorder", nullptr, nullptr);
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

  Recorder rec(config);
  bool device_ok = rec.open();
  std::string error_msg;
  if (!device_ok) {
    error_msg = "Failed to open audio device. Check --list-devices.";
  }

  while (!glfwWindowShouldClose(window) && g_gui_running.load(std::memory_order_relaxed)) {
    glfwPollEvents();

    // Auto-stop on duration
    auto state = rec.state();
    if (state == Recorder::State::Recording && config.duration_seconds > 0 &&
        rec.elapsed_seconds() >= config.duration_seconds) {
      rec.stop();
    }

    // Auto-stop on writer error
    if (state == Recorder::State::Recording && rec.has_error()) {
      rec.stop();
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
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    state = rec.state();

    // --- Header ---
    ImGui::TextUnformatted("Audio Recorder");
    ImGui::Separator();
    ImGui::Spacing();

    // --- Device info ---
    if (device_ok) {
      ImGui::Text("Device: %s", rec.device_name().c_str());
      ImGui::Text("Config: %dch, %dHz, %dbit", config.channels, config.sample_rate,
                   config.bit_depth);
      ImGui::Text("Output: %s", config.output_file.c_str());
      if (config.split_seconds > 0) {
        ImGui::SameLine();
        ImGui::Text(" (split every %.0fm)", config.split_seconds / 60.0);
      }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // --- Status ---
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
    ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddCircleFilled(
        ImVec2(cursor.x + radius, cursor.y + ImGui::GetTextLineHeight() * 0.5f), radius,
        ImGui::ColorConvertFloat4ToU32(status_color));
    ImGui::Dummy(ImVec2(radius * 2 + 4, 0));
    ImGui::SameLine();
    ImGui::Text("%s", status_text);

    // --- Time and stats ---
    if (state != Recorder::State::Idle) {
      char time_buf[32];
      format_time(rec.elapsed_seconds(), time_buf, sizeof(time_buf));
      ImGui::Text("Time:   %s", time_buf);

      std::string cur_file = rec.current_file();
      if (!cur_file.empty()) {
        ImGui::Text("File:   %s", cur_file.c_str());
      }

      uint64_t frames = rec.total_frames();
      ImGui::Text("Frames: %llu", static_cast<unsigned long long>(frames));

      uint64_t overruns = rec.overruns();
      if (overruns > 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Overruns: %llu",
                           static_cast<unsigned long long>(overruns));
      }

      int fc = rec.files_written();
      if (fc > 1) {
        ImGui::Text("Files:  %d", fc);
      }
    }

    // --- Error ---
    if (!error_msg.empty()) {
      ImGui::Spacing();
      ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", error_msg.c_str());
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // --- Buttons ---
    if (!device_ok) {
      ImGui::BeginDisabled();
      ImGui::Button("Record", ImVec2(120, 36));
      ImGui::SameLine();
      ImGui::Button("Pause", ImVec2(120, 36));
      ImGui::SameLine();
      ImGui::Button("Stop", ImVec2(120, 36));
      ImGui::EndDisabled();
    } else {
      // Record button
      bool can_record = state == Recorder::State::Idle;
      if (!can_record) ImGui::BeginDisabled();
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.7f, 0.3f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.5f, 0.15f, 1.0f));
      if (ImGui::Button("Record", ImVec2(120, 36))) {
        error_msg.clear();
        if (!rec.start()) {
          error_msg = "Failed to start recording.";
        }
      }
      ImGui::PopStyleColor(3);
      if (!can_record) ImGui::EndDisabled();

      ImGui::SameLine();

      // Pause / Resume button
      bool can_pause = state == Recorder::State::Recording || state == Recorder::State::Paused;
      if (!can_pause) ImGui::BeginDisabled();
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.6f, 0.1f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.7f, 0.2f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.6f, 0.5f, 0.05f, 1.0f));
      const char* pause_label = state == Recorder::State::Paused ? "Resume" : "Pause";
      if (ImGui::Button(pause_label, ImVec2(120, 36))) {
        if (state == Recorder::State::Paused) {
          rec.resume();
        } else {
          rec.pause();
        }
      }
      ImGui::PopStyleColor(3);
      if (!can_pause) ImGui::EndDisabled();

      ImGui::SameLine();

      // Stop button
      bool can_stop = state == Recorder::State::Recording || state == Recorder::State::Paused;
      if (!can_stop) ImGui::BeginDisabled();
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.15f, 0.15f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.25f, 0.25f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.6f, 0.1f, 0.1f, 1.0f));
      if (ImGui::Button("Stop", ImVec2(120, 36))) {
        rec.stop();
      }
      ImGui::PopStyleColor(3);
      if (!can_stop) ImGui::EndDisabled();
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

  // Clean up if still recording when window is closed
  if (rec.state() == Recorder::State::Recording || rec.state() == Recorder::State::Paused) {
    rec.stop();
  }

  auto stats_frames = rec.total_frames();
  if (stats_frames > 0) {
    double duration = static_cast<double>(stats_frames) / config.sample_rate;
    std::fprintf(stderr, "\nDone. %llu frames (%.1fs)",
                 static_cast<unsigned long long>(stats_frames), duration);
    int fc = rec.files_written();
    if (fc > 1) {
      std::fprintf(stderr, " across %d files", fc);
    }
    std::fprintf(stderr, "\n");
    auto ov = rec.overruns();
    if (ov > 0) {
      std::fprintf(stderr, "Warning: %llu frames dropped (buffer overrun)\n",
                   static_cast<unsigned long long>(ov));
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
