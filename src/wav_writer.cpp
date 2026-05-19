#include "wav_writer.h"

#include <cstring>
#include <iostream>

namespace recorder {

WavWriter::WavWriter(const std::string& filename, int channels, int sample_rate,
                     int bits_per_sample)
    : _filename(filename),
      _channels(channels),
      _sample_rate(sample_rate),
      _bits_per_sample(bits_per_sample),
      _bytes_per_sample(bits_per_sample / 8) {}

WavWriter::~WavWriter() {
  finalize();
  if (_file) std::fclose(_file);
}

bool WavWriter::init() {
  _file = std::fopen(_filename.c_str(), "wb");
  if (!_file) {
    std::cerr << "Error: cannot open " << _filename << " for writing\n";
    return false;
  }
  write_header();
  return true;
}

bool WavWriter::write_samples(const int32_t* interleaved, size_t num_frames) {
  if (!_file || _finalized) return false;

  size_t total_samples = num_frames * _channels;

  if (_bytes_per_sample == 3) {
    for (size_t i = 0; i < total_samples; ++i) {
      int32_t s = interleaved[i];
      uint8_t bytes[3] = {static_cast<uint8_t>(s), static_cast<uint8_t>(s >> 8),
                          static_cast<uint8_t>(s >> 16)};
      if (std::fwrite(bytes, 1, 3, _file) != 3) return false;
    }
  } else {
    for (size_t i = 0; i < total_samples; ++i) {
      int16_t s = static_cast<int16_t>(interleaved[i]);
      if (std::fwrite(&s, sizeof(int16_t), 1, _file) != 1) return false;
    }
  }

  uint32_t bytes_written = static_cast<uint32_t>(total_samples * _bytes_per_sample);
  _data_bytes += bytes_written;
  _total_frames += num_frames;
  return true;
}

bool WavWriter::finalize() {
  if (!_file || _finalized) return true;
  _finalized = true;
  patch_header();
  std::fflush(_file);
  return true;
}

static void write_u32_le(FILE* f, uint32_t v) {
  uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8),
                  static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)};
  std::fwrite(b, 1, 4, f);
}

static void write_u16_le(FILE* f, uint16_t v) {
  uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
  std::fwrite(b, 1, 2, f);
}

void WavWriter::write_header() {
  uint16_t block_align = _channels * _bytes_per_sample;
  uint32_t byte_rate = _sample_rate * block_align;

  std::fwrite("RIFF", 1, 4, _file);
  write_u32_le(_file, 0);
  std::fwrite("WAVE", 1, 4, _file);

  std::fwrite("fmt ", 1, 4, _file);
  write_u32_le(_file, 16);
  write_u16_le(_file, 1);
  write_u16_le(_file, _channels);
  write_u32_le(_file, _sample_rate);
  write_u32_le(_file, byte_rate);
  write_u16_le(_file, block_align);
  write_u16_le(_file, _bits_per_sample);

  std::fwrite("data", 1, 4, _file);
  write_u32_le(_file, 0);
}

void WavWriter::patch_header() {
  std::fseek(_file, 4, SEEK_SET);
  write_u32_le(_file, 36 + _data_bytes);
  std::fseek(_file, 40, SEEK_SET);
  write_u32_le(_file, _data_bytes);
}

}  // namespace recorder
