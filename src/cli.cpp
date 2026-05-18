#include "cli.h"

#include <getopt.h>

#include <cstdlib>
#include <iostream>

namespace recorder {

static void print_usage(const char* prog) {
  std::cerr
      << "Usage: " << prog << " [options]\n"
      << "\n"
      << "Options:\n"
      << "  -l, --list-devices         List available input devices and exit\n"
      << "  -d, --device <index>       Input device index (default: system default)\n"
      << "  -c, --channels <n>         Number of channels, 1-32 (default: 2)\n"
      << "  -r, --rate <hz>            Sample rate in Hz (default: 48000)\n"
      << "  -b, --bits <n>             Bit depth: 16 or 24 (default: 24)\n"
      << "  -o, --output <file>        Output FLAC filename (default: recording.flac)\n"
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
        break;
      case 'c':
        cfg.channels = std::atoi(optarg);
        break;
      case 'r':
        cfg.sample_rate = std::atoi(optarg);
        break;
      case 'b':
        cfg.bit_depth = std::atoi(optarg);
        break;
      case 'o':
        cfg.output_file = optarg;
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

  return cfg;
}

}  // namespace recorder
