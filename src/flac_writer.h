#pragma once

#include <FLAC/stream_encoder.h>

#include <cstdint>
#include <string>

namespace recorder {

class FlacWriter {
 public:
  FlacWriter(const std::string& filename, int channels, int sample_rate, int bits_per_sample);
  ~FlacWriter();

  FlacWriter(const FlacWriter&) = delete;
  FlacWriter& operator=(const FlacWriter&) = delete;

  bool init();
  bool write_samples(const int32_t* interleaved, size_t num_frames);
  bool finalize();
  uint64_t total_frames() const { return _total_frames; }

 private:
  std::string _filename;
  int _channels;
  int _sample_rate;
  int _bits_per_sample;
  FLAC__StreamEncoder* _encoder = nullptr;
  uint64_t _total_frames = 0;
  bool _finalized = false;
};

}  // namespace recorder
