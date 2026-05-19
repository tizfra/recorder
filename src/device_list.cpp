#include "device_list.h"

#include <portaudio.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace recorder {

static bool is_builtin_or_virtual(const char* name) {
  static const char* skip_patterns[] = {
      "MacBook",    "Built-in",  "Internal",
      "Microsoft Teams", "ZoomAudioDevice",
      "BlackHole",  "Soundflower", "Loopback",
      "default",    "pulse",      "sysdefault",
      "dmix",       "surround",   "front:",
      "lavrate",    "samplerate", "speexrate",
      "speex",      "upmix",      "vdownmix",
      "usbstream",  "jack",       "oss",
      "spdif",
  };
  for (auto* pattern : skip_patterns) {
    if (std::strstr(name, pattern)) return true;
  }
  return false;
}

static bool looks_like_usb(const char* name) {
  return std::strstr(name, "USB") || std::strstr(name, "usb");
}

static bool is_hw_device(const char* name) {
  // ALSA direct hardware devices contain "hw:" in their name
  return std::strstr(name, "(hw:") || std::strstr(name, "hw:") == name;
}

bool is_usb_device(const std::string& name) {
  return looks_like_usb(name.c_str()) || !is_builtin_or_virtual(name.c_str());
}

// Higher score = more preferred
static int device_score(const char* name, int channels) {
  bool usb = looks_like_usb(name);
  bool hw = is_hw_device(name);
  bool builtin = is_builtin_or_virtual(name);

  int score = channels;

  if (usb)      score += 10000;
  if (hw)       score += 5000;
  if (!builtin) score += 1000;

  return score;
}

std::vector<DeviceInfo> scan_input_devices() {
  std::vector<DeviceInfo> devices;
  Pa_Initialize();
  int count = Pa_GetDeviceCount();
  for (int i = 0; i < count; ++i) {
    const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
    if (!info || info->maxInputChannels <= 0) continue;
    devices.push_back({i, info->maxInputChannels, info->name});
  }
  Pa_Terminate();
  return devices;
}

std::optional<DeviceInfo> find_preferred_device() {
  Pa_Initialize();

  int count = Pa_GetDeviceCount();
  if (count <= 0) {
    Pa_Terminate();
    return std::nullopt;
  }

  int best_index = -1;
  int best_score = -1;
  int best_channels = 0;
  std::string best_name;

  for (int i = 0; i < count; ++i) {
    const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
    if (!info || info->maxInputChannels <= 0) continue;

    int score = device_score(info->name, info->maxInputChannels);
    if (score > best_score) {
      best_score = score;
      best_index = i;
      best_channels = info->maxInputChannels;
      best_name = info->name;
    }
  }

  Pa_Terminate();

  if (best_index < 0) return std::nullopt;
  return DeviceInfo{best_index, best_channels, best_name};
}

std::optional<DeviceInfo> get_device_info(int index) {
  Pa_Initialize();
  const PaDeviceInfo* info = Pa_GetDeviceInfo(index);
  std::optional<DeviceInfo> result;
  if (info && info->maxInputChannels > 0) {
    result = DeviceInfo{index, info->maxInputChannels, info->name};
  }
  Pa_Terminate();
  return result;
}

void print_input_devices() {
  Pa_Initialize();

  int count = Pa_GetDeviceCount();
  if (count <= 0) {
    std::fprintf(stderr, "No audio devices found.\n");
    Pa_Terminate();
    return;
  }

  std::printf("%-5s  %-50s  %8s  %10s\n", "Index", "Name", "Channels", "Rate (Hz)");
  std::printf("%-5s  %-50s  %8s  %10s\n", "-----", "--------------------------------------------------",
              "--------", "----------");

  for (int i = 0; i < count; ++i) {
    const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
    if (!info || info->maxInputChannels <= 0) continue;

    std::printf("%-5d  %-50.50s  %8d  %10.0f\n", i, info->name, info->maxInputChannels,
                info->defaultSampleRate);
  }

  Pa_Terminate();
}

std::string find_usb_disk() {
  namespace fs = std::filesystem;
#ifdef __APPLE__
  const fs::path volumes("/Volumes");
  if (!fs::is_directory(volumes)) return {};
  for (auto& entry : fs::directory_iterator(volumes)) {
    if (!entry.is_directory()) continue;
    auto name = entry.path().filename().string();
    if (name == "Macintosh HD") continue;
    if (access(entry.path().c_str(), W_OK) == 0) {
      return entry.path().string();
    }
  }
#else
  if (const char* user = std::getenv("USER")) {
    fs::path media = fs::path("/media") / user;
    if (fs::is_directory(media)) {
      for (auto& entry : fs::directory_iterator(media)) {
        if (entry.is_directory() && access(entry.path().c_str(), W_OK) == 0) {
          return entry.path().string();
        }
      }
    }
  }
  const fs::path mnt("/mnt");
  if (fs::is_directory(mnt)) {
    for (auto& entry : fs::directory_iterator(mnt)) {
      if (entry.is_directory() && access(entry.path().c_str(), W_OK) == 0) {
        return entry.path().string();
      }
    }
  }
#endif
  return {};
}

std::string unique_filename(const std::string& path) {
  namespace fs = std::filesystem;

  std::string stem, ext;
  auto dot = path.rfind('.');
  if (dot != std::string::npos) {
    stem = path.substr(0, dot);
    ext = path.substr(dot);
  } else {
    stem = path;
  }

  auto has_split_files = [&](const std::string& s) {
    std::error_code ec;
    auto parent = fs::path(s).parent_path();
    auto prefix = fs::path(s).stem().string() + "_";
    if (parent.empty()) parent = ".";
    for (auto& entry : fs::directory_iterator(parent, ec)) {
      auto name = entry.path().filename().string();
      if (name.rfind(prefix, 0) == 0) return true;
    }
    return false;
  };

  bool in_use = fs::exists(path) || has_split_files(stem + ext);
  if (!in_use) return path;

  for (int i = 2; i < 10000; ++i) {
    std::string candidate = stem + "_" + std::to_string(i) + ext;
    if (!fs::exists(candidate) && !has_split_files(candidate)) return candidate;
  }
  return path;
}

}  // namespace recorder
