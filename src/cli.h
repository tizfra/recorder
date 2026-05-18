#pragma once

#include <optional>
#include <string>

namespace recorder {

struct Config {
  int device_index = -1;
  int channels = 2;
  int sample_rate = 48000;
  int bit_depth = 24;
  std::string output_file = "recording.flac";
  double duration_seconds = 0.0;
  double split_seconds = 0.0;
  bool list_devices = false;
};

std::optional<Config> parse_args(int argc, char* argv[]);

}  // namespace recorder
