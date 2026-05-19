#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace recorder {

class AudioWriter {
 public:
  virtual ~AudioWriter() = default;
  virtual bool init() = 0;
  virtual bool write_samples(const int32_t* interleaved, size_t num_frames) = 0;
  virtual bool finalize() = 0;
  virtual uint64_t total_frames() const = 0;
};

std::unique_ptr<AudioWriter> create_writer(const std::string& filename, int channels,
                                            int sample_rate, int bits_per_sample);

}  // namespace recorder
