#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace recorder {

class AudioReader {
 public:
  virtual ~AudioReader() = default;
  virtual bool init() = 0;
  // Legge fino a num_frames frame interleaved in `out` (buffer di almeno
  // num_frames * channels() int32_t). Ritorna i frame letti; 0 = fine file.
  virtual size_t read_samples(int32_t* interleaved, size_t num_frames) = 0;
  virtual bool seek(uint64_t frame) = 0;
  virtual int channels() const = 0;
  virtual int sample_rate() const = 0;
  virtual int bits_per_sample() const = 0;
  virtual uint64_t total_frames() const = 0;
};

// Sceglie il reader in base all'estensione del file (.wav, .flac, .mp3).
std::unique_ptr<AudioReader> create_reader(const std::string& filename);

}  // namespace recorder
