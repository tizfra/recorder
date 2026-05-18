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
      "default",    "pulse",
  };
  for (auto* pattern : skip_patterns) {
    if (std::strstr(name, pattern)) return true;
  }
  return false;
}

static bool looks_like_usb(const char* name) {
  return std::strstr(name, "USB") || std::strstr(name, "usb");
}

bool is_usb_device(const std::string& name) {
  return looks_like_usb(name.c_str()) || !is_builtin_or_virtual(name.c_str());
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

  // Collect all input devices
  struct Candidate {
    int index;
    int channels;
    std::string name;
    bool is_usb_like;
  };
  std::vector<Candidate> candidates;

  for (int i = 0; i < count; ++i) {
    const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
    if (!info || info->maxInputChannels <= 0) continue;

    bool builtin = is_builtin_or_virtual(info->name);
    bool usb = looks_like_usb(info->name);

    // On macOS, a device that isn't built-in/virtual is likely USB/Thunderbolt
    bool usb_like = usb || !builtin;

    candidates.push_back({i, info->maxInputChannels, info->name, usb_like});
  }

  Pa_Terminate();

  if (candidates.empty()) return std::nullopt;

  // Prefer USB-like devices; among those, prefer the one with most channels
  Candidate* best = nullptr;
  for (auto& c : candidates) {
    if (!best) {
      best = &c;
      continue;
    }
    if (c.is_usb_like && !best->is_usb_like) {
      best = &c;
    } else if (c.is_usb_like == best->is_usb_like && c.channels > best->channels) {
      best = &c;
    }
  }

  return DeviceInfo{best->index, best->channels, best->name};
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

}  // namespace recorder
