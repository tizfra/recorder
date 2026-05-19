#include "audio_writer.h"

#include <algorithm>
#include <string>

#include "flac_writer.h"
#include "wav_writer.h"

namespace recorder {

static bool ends_with(const std::string& s, const std::string& suffix) {
  if (suffix.size() > s.size()) return false;
  return std::equal(suffix.rbegin(), suffix.rend(), s.rbegin(), [](char a, char b) {
    return std::tolower(a) == std::tolower(b);
  });
}

static bool is_flac(const std::string& filename) { return ends_with(filename, ".flac"); }

int max_channels_for_format(const std::string& filename) {
  return is_flac(filename) ? 8 : 65535;
}

std::unique_ptr<AudioWriter> create_writer(const std::string& filename, int channels,
                                            int sample_rate, int bits_per_sample) {
  if (ends_with(filename, ".flac")) {
    return std::make_unique<FlacWriter>(filename, channels, sample_rate, bits_per_sample);
  }
  return std::make_unique<WavWriter>(filename, channels, sample_rate, bits_per_sample);
}

}  // namespace recorder
