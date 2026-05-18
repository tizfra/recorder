#include "cli.h"

#include <getopt.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "device_list.h"

namespace recorder {

namespace fs = std::filesystem;

static std::string find_usb_disk() {
#ifdef __APPLE__
  // macOS: external disks mount under /Volumes/. Skip the boot volume.
  const fs::path volumes("/Volumes");
  if (!fs::is_directory(volumes)) return {};
  for (auto& entry : fs::directory_iterator(volumes)) {
    if (!entry.is_directory()) continue;
    auto name = entry.path().filename().string();
    if (name == "Macintosh HD") continue;
    // Check it's writable
    if (access(entry.path().c_str(), W_OK) == 0) {
      return entry.path().string();
    }
  }
#else
  // Linux: external disks mount under /media/$USER/ or /mnt/
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
  // Fallback: check /mnt/ for mounted volumes
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

static std::string default_output_path() {
  std::string usb = find_usb_disk();
  if (!usb.empty()) {
    return usb + "/recording.flac";
  }
  return "recording.flac";
}

static void print_usage(const char* prog) {
  std::cerr
      << "Usage: " << prog << " [options]\n"
      << "\n"
      << "Options:\n"
      << "  -l, --list-devices         List available input devices and exit\n"
      << "  -d, --device <index>       Input device index (default: USB interface, or first available)\n"
      << "  -c, --channels <n>         Number of channels, 1-32 (default: device max)\n"
      << "  -r, --rate <hz>            Sample rate in Hz (default: 48000)\n"
      << "  -b, --bits <n>             Bit depth: 16 or 24 (default: 24)\n"
      << "  -o, --output <file>        Output FLAC filename (default: <usb disk>/recording.flac)\n"
      << "  -t, --duration <seconds>   Recording duration, 0 = until Ctrl+C (default: 0)\n"
      << "  -s, --split <minutes>      Split into new file every N minutes, 0 = no split (default: 0)\n"
      << "  -g, --gui                  Launch with graphical interface\n"
      << "  -h, --help                 Show this help message\n"
      << "\n"
      << "Examples:\n"
      << "  " << prog << " --list-devices\n"
      << "  " << prog << " -d 3 -c 8 -r 48000 -b 24 -o session.flac\n"
      << "  " << prog << " -c 2 -t 60 -o one_minute.flac\n"
      << "  " << prog << " -c 2 -s 30 -o session.flac   # splits into session_001.flac, session_002.flac, ...\n";
}

std::optional<Config> parse_args(int argc, char* argv[]) {
  Config cfg;
  bool output_specified = false;
  bool device_specified = false;
  bool channels_specified = false;

  static struct option long_options[] = {
      {"list-devices", no_argument, nullptr, 'l'},
      {"device", required_argument, nullptr, 'd'},
      {"channels", required_argument, nullptr, 'c'},
      {"rate", required_argument, nullptr, 'r'},
      {"bits", required_argument, nullptr, 'b'},
      {"output", required_argument, nullptr, 'o'},
      {"duration", required_argument, nullptr, 't'},
      {"split", required_argument, nullptr, 's'},
      {"gui", no_argument, nullptr, 'g'},
      {"help", no_argument, nullptr, 'h'},
      {nullptr, 0, nullptr, 0},
  };

  int opt;
  while ((opt = getopt_long(argc, argv, "ld:c:r:b:o:t:s:gh", long_options, nullptr)) != -1) {
    switch (opt) {
      case 'l':
        cfg.list_devices = true;
        return cfg;
      case 'd':
        cfg.device_index = std::atoi(optarg);
        device_specified = true;
        break;
      case 'c':
        cfg.channels = std::atoi(optarg);
        channels_specified = true;
        break;
      case 'r':
        cfg.sample_rate = std::atoi(optarg);
        break;
      case 'b':
        cfg.bit_depth = std::atoi(optarg);
        break;
      case 'o':
        cfg.output_file = optarg;
        output_specified = true;
        break;
      case 't':
        cfg.duration_seconds = std::atof(optarg);
        break;
      case 's':
        cfg.split_seconds = std::atof(optarg) * 60.0;
        break;
      case 'g':
        cfg.gui = true;
        break;
      case 'h':
        print_usage(argv[0]);
        return std::nullopt;
      default:
        print_usage(argv[0]);
        return std::nullopt;
    }
  }

  if (!device_specified) {
    auto preferred = find_preferred_device();
    if (preferred) {
      cfg.device_index = preferred->index;
      if (!channels_specified) {
        cfg.channels = preferred->max_input_channels;
      }
      std::cerr << "Selected device: " << preferred->name << " (" << preferred->max_input_channels
                << "ch)\n";
    }
  } else if (!channels_specified) {
    auto info = get_device_info(cfg.device_index);
    if (info) {
      cfg.channels = info->max_input_channels;
    }
  }

  if (cfg.channels < 1 || cfg.channels > 32) {
    std::cerr << "Error: channels must be 1-32, got " << cfg.channels << "\n";
    return std::nullopt;
  }
  if (cfg.bit_depth != 16 && cfg.bit_depth != 24) {
    std::cerr << "Error: bit depth must be 16 or 24, got " << cfg.bit_depth << "\n";
    return std::nullopt;
  }
  if (cfg.sample_rate < 8000 || cfg.sample_rate > 192000) {
    std::cerr << "Error: sample rate must be 8000-192000, got " << cfg.sample_rate << "\n";
    return std::nullopt;
  }
  if (cfg.duration_seconds < 0) {
    std::cerr << "Error: duration must be non-negative\n";
    return std::nullopt;
  }
  if (cfg.split_seconds < 0) {
    std::cerr << "Error: split must be non-negative\n";
    return std::nullopt;
  }

  if (!output_specified) {
    cfg.output_file = default_output_path();
  }

  return cfg;
}

}  // namespace recorder
