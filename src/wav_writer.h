#pragma once

#include "audio_writer.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace recorder {

class WavWriter : public AudioWriter {
 public:
  WavWriter(const std::string& filename, int channels, int sample_rate, int bits_per_sample);
  ~WavWriter() override;

  WavWriter(const WavWriter&) = delete;
  WavWriter& operator=(const WavWriter&) = delete;

  bool init() override;
  bool write_samples(const int32_t* interleaved, size_t num_frames) override;
  bool finalize() override;
  uint64_t total_frames() const override { return _total_frames; }

 private:
  std::string _filename;
  int _channels;
  int _sample_rate;
  int _bits_per_sample;
  int _bytes_per_sample;
  FILE* _file = nullptr;
  uint64_t _total_frames = 0;
  uint32_t _data_bytes = 0;
  bool _finalized = false;

  void write_header();
  void patch_header();
};

}  // namespace recorder
