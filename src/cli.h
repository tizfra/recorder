#pragma once

#include <optional>
#include <string>

namespace recorder {

struct Config {
  int device_index = -1;
  int channels = 2;
  int sample_rate = 48000;
  int bit_depth = 24;
  std::string output_file = "recording.wav";
  std::string output_file_base;
  double duration_seconds = 0.0;
  double split_seconds = 1800.0;
  bool list_devices = false;
  bool gui = false;
};

std::optional<Config> parse_args(int argc, char* argv[]);

}  // namespace recorder
