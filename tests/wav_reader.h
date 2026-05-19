#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

struct WavData {
  int channels = 0;
  int sample_rate = 0;
  int bits_per_sample = 0;
  std::vector<int32_t> samples;

  size_t num_frames() const { return channels > 0 ? samples.size() / channels : 0; }
};

inline WavData read_wav(const std::string& path) {
  WavData wav;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return wav;

  auto read_u16 = [&]() -> uint16_t {
    uint8_t b[2];
    std::fread(b, 1, 2, f);
    return b[0] | (b[1] << 8);
  };
  auto read_u32 = [&]() -> uint32_t {
    uint8_t b[4];
    std::fread(b, 1, 4, f);
    return b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24);
  };

  char id[4];
  std::fread(id, 1, 4, f);
  if (std::memcmp(id, "RIFF", 4) != 0) { std::fclose(f); return wav; }
  read_u32();
  std::fread(id, 1, 4, f);
  if (std::memcmp(id, "WAVE", 4) != 0) { std::fclose(f); return wav; }

  while (std::fread(id, 1, 4, f) == 4) {
    uint32_t chunk_size = read_u32();

    if (std::memcmp(id, "fmt ", 4) == 0) {
      read_u16();
      wav.channels = read_u16();
      wav.sample_rate = read_u32();
      read_u32();
      read_u16();
      wav.bits_per_sample = read_u16();
      if (chunk_size > 16) std::fseek(f, chunk_size - 16, SEEK_CUR);
    } else if (std::memcmp(id, "data", 4) == 0) {
      int bps = wav.bits_per_sample / 8;
      size_t total_samples = chunk_size / bps;
      wav.samples.resize(total_samples);

      if (bps == 4) {
        std::fread(wav.samples.data(), sizeof(int32_t), total_samples, f);
      } else if (bps == 3) {
        for (size_t i = 0; i < total_samples; ++i) {
          uint8_t b[3];
          std::fread(b, 1, 3, f);
          int32_t v = b[0] | (b[1] << 8) | (b[2] << 16);
          if (v & 0x800000) v |= 0xFF000000;
          wav.samples[i] = v;
        }
      } else if (bps == 2) {
        for (size_t i = 0; i < total_samples; ++i) {
          int16_t v;
          std::fread(&v, sizeof(int16_t), 1, f);
          wav.samples[i] = v;
        }
      } else {
        std::fseek(f, chunk_size, SEEK_CUR);
      }
    } else {
      std::fseek(f, chunk_size, SEEK_CUR);
    }
  }

  std::fclose(f);
  return wav;
}
