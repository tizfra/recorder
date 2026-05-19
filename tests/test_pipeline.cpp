#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "../src/audio_writer.h"
#include "../src/device_list.h"
#include "../src/ring_buffer.h"
#include "flac_reader.h"
#include "wav_reader.h"

namespace fs = std::filesystem;

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, msg)                                                 \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__);    \
      return false;                                                      \
    }                                                                    \
  } while (0)

#define RUN_TEST(fn)                                                     \
  do {                                                                   \
    ++tests_run;                                                         \
    std::fprintf(stderr, "%-50s", #fn);                                  \
    if (fn()) {                                                          \
      ++tests_passed;                                                    \
      std::fprintf(stderr, " OK\n");                                     \
    } else {                                                             \
      std::fprintf(stderr, " FAILED\n");                                 \
    }                                                                    \
  } while (0)

static std::string test_dir;

static std::string test_path(const std::string& name) { return test_dir + "/" + name; }

// Generate deterministic test samples: each sample = channel * 10000 + frame_index
static std::vector<int32_t> generate_samples(int channels, size_t frames) {
  std::vector<int32_t> samples(channels * frames);
  for (size_t f = 0; f < frames; ++f) {
    for (int c = 0; c < channels; ++c) {
      samples[f * channels + c] = static_cast<int32_t>(c * 10000 + f);
    }
  }
  return samples;
}

// Write samples through create_writer, just like the recorder's writer thread
static bool write_through_pipeline(const std::string& filename, int channels, int sample_rate,
                                    int bits_per_sample, const std::vector<int32_t>& samples) {
  size_t frames = samples.size() / channels;
  auto writer = recorder::create_writer(filename, channels, sample_rate, bits_per_sample);
  if (!writer || !writer->init()) return false;
  if (!writer->write_samples(samples.data(), frames)) return false;
  return writer->finalize();
}

// Write samples through ring buffer → writer, simulating the full PA callback → writer thread flow
static bool write_through_ring_buffer(const std::string& filename, int channels, int sample_rate,
                                       int bits_per_sample, const std::vector<int32_t>& samples) {
  size_t total_samples = samples.size();
  size_t frames = total_samples / channels;

  recorder::SpscRingBuffer<int32_t> ring(sample_rate * channels * 2);

  auto writer = recorder::create_writer(filename, channels, sample_rate, bits_per_sample);
  if (!writer || !writer->init()) return false;

  // Push into ring buffer in chunks (simulating PA callback)
  size_t written = 0;
  const size_t chunk_samples = 1024 * channels;

  while (written < total_samples) {
    size_t to_write = std::min(chunk_samples, total_samples - written);
    ring.write(samples.data() + written, to_write);
    written += to_write;

    // Consume from ring buffer (simulating writer thread)
    std::vector<int32_t> buf(chunk_samples);
    while (ring.read_available() > 0) {
      size_t avail = ring.read_available();
      size_t to_read = std::min(avail, chunk_samples);
      to_read -= to_read % channels;
      size_t got = ring.read(buf.data(), to_read);
      size_t f = got / channels;
      if (!writer->write_samples(buf.data(), f)) return false;
    }
  }

  return writer->finalize();
}

// --- WAV tests ---

static bool test_wav_mono() {
  std::string path = test_path("wav_mono.wav");
  int channels = 1, rate = 48000;
  size_t frames = 4800;
  auto input = generate_samples(channels, frames);

  CHECK(write_through_ring_buffer(path, channels, rate, 32, input), "write failed");

  auto wav = read_wav(path);
  CHECK(wav.channels == channels, "wrong channel count");
  CHECK(wav.sample_rate == rate, "wrong sample rate");
  CHECK(wav.bits_per_sample == 32, "wrong bit depth");
  CHECK(wav.num_frames() == frames, "wrong frame count");

  for (size_t i = 0; i < input.size(); ++i) {
    CHECK(wav.samples[i] == input[i], "sample mismatch");
  }
  return true;
}

static bool test_wav_stereo() {
  std::string path = test_path("wav_stereo.wav");
  int channels = 2, rate = 48000;
  size_t frames = 4800;
  auto input = generate_samples(channels, frames);

  CHECK(write_through_ring_buffer(path, channels, rate, 32, input), "write failed");

  auto wav = read_wav(path);
  CHECK(wav.channels == channels, "wrong channel count");
  CHECK(wav.sample_rate == rate, "wrong sample rate");
  CHECK(wav.num_frames() == frames, "wrong frame count");

  for (size_t i = 0; i < input.size(); ++i) {
    CHECK(wav.samples[i] == input[i], "sample mismatch");
  }
  return true;
}

static bool test_wav_16ch() {
  std::string path = test_path("wav_16ch.wav");
  int channels = 16, rate = 48000;
  size_t frames = 4800;
  auto input = generate_samples(channels, frames);

  CHECK(write_through_ring_buffer(path, channels, rate, 32, input), "write failed");

  auto wav = read_wav(path);
  CHECK(wav.channels == channels, "wrong channel count");
  CHECK(wav.num_frames() == frames, "wrong frame count");

  for (size_t i = 0; i < input.size(); ++i) {
    CHECK(wav.samples[i] == input[i], "sample mismatch");
  }
  return true;
}

// --- FLAC tests ---

static bool test_flac_mono_24bit() {
  std::string path = test_path("flac_mono_24.flac");
  int channels = 1, rate = 48000, bits = 24;
  size_t frames = 4800;
  auto input = generate_samples(channels, frames);

  CHECK(write_through_ring_buffer(path, channels, rate, bits, input), "write failed");

  auto flac = read_flac(path);
  CHECK(flac.channels == channels, "wrong channel count");
  CHECK(flac.sample_rate == rate, "wrong sample rate");
  CHECK(flac.bits_per_sample == bits, "wrong bit depth");
  CHECK(flac.num_frames() == frames, "wrong frame count");

  for (size_t i = 0; i < input.size(); ++i) {
    CHECK(flac.samples[i] == input[i], "sample mismatch");
  }
  return true;
}

static bool test_flac_stereo_24bit() {
  std::string path = test_path("flac_stereo_24.flac");
  int channels = 2, rate = 48000, bits = 24;
  size_t frames = 4800;
  auto input = generate_samples(channels, frames);

  CHECK(write_through_ring_buffer(path, channels, rate, bits, input), "write failed");

  auto flac = read_flac(path);
  CHECK(flac.channels == channels, "wrong channel count");
  CHECK(flac.sample_rate == rate, "wrong sample rate");
  CHECK(flac.num_frames() == frames, "wrong frame count");

  for (size_t i = 0; i < input.size(); ++i) {
    CHECK(flac.samples[i] == input[i], "sample mismatch");
  }
  return true;
}

static bool test_flac_16ch_grouped() {
  // 16 channels in FLAC should produce two files: ch01-08 and ch09-16
  std::string base = test_path("flac_16ch.flac");
  int channels = 16, rate = 48000, bits = 24;
  size_t frames = 4800;
  auto input = generate_samples(channels, frames);

  int max_ch = recorder::max_channels_for_format(base);
  CHECK(max_ch == 8, "FLAC max channels should be 8");

  // Write two group files like the recorder does
  std::string path_a = test_path("flac_16ch_ch01-08.flac");
  std::string path_b = test_path("flac_16ch_ch09-16.flac");

  // De-interleave group A (channels 0-7)
  std::vector<int32_t> group_a(8 * frames);
  for (size_t f = 0; f < frames; ++f) {
    for (int c = 0; c < 8; ++c) {
      group_a[f * 8 + c] = input[f * channels + c];
    }
  }

  // De-interleave group B (channels 8-15)
  std::vector<int32_t> group_b(8 * frames);
  for (size_t f = 0; f < frames; ++f) {
    for (int c = 0; c < 8; ++c) {
      group_b[f * 8 + c] = input[f * channels + 8 + c];
    }
  }

  CHECK(write_through_pipeline(path_a, 8, rate, bits, group_a), "write group A failed");
  CHECK(write_through_pipeline(path_b, 8, rate, bits, group_b), "write group B failed");

  // Read back and verify
  auto flac_a = read_flac(path_a);
  auto flac_b = read_flac(path_b);

  CHECK(flac_a.channels == 8, "group A wrong channels");
  CHECK(flac_b.channels == 8, "group B wrong channels");
  CHECK(flac_a.num_frames() == frames, "group A wrong frames");
  CHECK(flac_b.num_frames() == frames, "group B wrong frames");

  for (size_t i = 0; i < group_a.size(); ++i) {
    CHECK(flac_a.samples[i] == group_a[i], "group A sample mismatch");
  }
  for (size_t i = 0; i < group_b.size(); ++i) {
    CHECK(flac_b.samples[i] == group_b[i], "group B sample mismatch");
  }
  return true;
}

// --- Split test ---

static bool test_wav_time_split() {
  // At 48kHz, 4800 frames = 0.1s. With split at 0.05s (2400 frames), expect 2 files.
  std::string base = test_path("split_test.wav");
  int channels = 2, rate = 48000;
  size_t frames = 4800;
  auto input = generate_samples(channels, frames);

  size_t frames_per_split = 2400;

  // Write first split
  std::string path1 = test_path("split_test_001.wav");
  std::vector<int32_t> part1(input.begin(), input.begin() + frames_per_split * channels);
  CHECK(write_through_pipeline(path1, channels, rate, 32, part1), "write split 1 failed");

  // Write second split
  std::string path2 = test_path("split_test_002.wav");
  std::vector<int32_t> part2(input.begin() + frames_per_split * channels, input.end());
  CHECK(write_through_pipeline(path2, channels, rate, 32, part2), "write split 2 failed");

  auto wav1 = read_wav(path1);
  auto wav2 = read_wav(path2);

  CHECK(wav1.num_frames() == frames_per_split, "split 1 wrong frames");
  CHECK(wav2.num_frames() == frames - frames_per_split, "split 2 wrong frames");

  // Verify content
  for (size_t i = 0; i < part1.size(); ++i) {
    CHECK(wav1.samples[i] == part1[i], "split 1 sample mismatch");
  }
  for (size_t i = 0; i < part2.size(); ++i) {
    CHECK(wav2.samples[i] == part2[i], "split 2 sample mismatch");
  }
  return true;
}

// --- Unique filename test ---

static bool test_unique_filename() {
  std::string path = test_path("unique_test.wav");

  // First call: file doesn't exist, should return same path
  std::string result1 = recorder::unique_filename(path);
  CHECK(result1 == path, "first call should return original path");

  // Create the file
  FILE* f = std::fopen(path.c_str(), "w");
  CHECK(f != nullptr, "failed to create test file");
  std::fclose(f);

  // Second call: file exists, should return _2 variant
  std::string result2 = recorder::unique_filename(path);
  std::string expected2 = test_path("unique_test_2.wav");
  CHECK(result2 == expected2, "second call should return _2 variant");

  // Create _2 file, third call should return _3
  f = std::fopen(expected2.c_str(), "w");
  std::fclose(f);
  std::string result3 = recorder::unique_filename(path);
  std::string expected3 = test_path("unique_test_3.wav");
  CHECK(result3 == expected3, "third call should return _3 variant");

  // Test split file detection
  std::string split_base = test_path("unique_split.wav");
  std::string split_001 = test_path("unique_split_001.wav");
  f = std::fopen(split_001.c_str(), "w");
  std::fclose(f);
  std::string split_result = recorder::unique_filename(split_base);
  std::string split_expected = test_path("unique_split_2.wav");
  CHECK(split_result == split_expected, "should detect split file and bump to _2");

  return true;
}

// --- Format detection test ---

static bool test_max_channels_for_format() {
  CHECK(recorder::max_channels_for_format("test.wav") > 8, "WAV should support >8 channels");
  CHECK(recorder::max_channels_for_format("test.flac") == 8, "FLAC max should be 8");
  CHECK(recorder::max_channels_for_format("test.WAV") > 8, "WAV uppercase");
  CHECK(recorder::max_channels_for_format("test.FLAC") == 8, "FLAC uppercase");
  return true;
}

// --- Ring buffer test ---

static bool test_ring_buffer() {
  recorder::SpscRingBuffer<int32_t> ring(1024);

  CHECK(ring.capacity() == 1024, "capacity should be 1024");
  CHECK(ring.read_available() == 0, "should start empty");

  int32_t data[100];
  for (int i = 0; i < 100; ++i) data[i] = i;

  size_t written = ring.write(data, 100);
  CHECK(written == 100, "should write all 100");
  CHECK(ring.read_available() == 100, "should have 100 available");

  int32_t out[100];
  size_t got = ring.read(out, 100);
  CHECK(got == 100, "should read all 100");

  for (int i = 0; i < 100; ++i) {
    CHECK(out[i] == i, "ring buffer data mismatch");
  }
  CHECK(ring.read_available() == 0, "should be empty after read");

  return true;
}

int main() {
  // Create temp directory
  test_dir = fs::temp_directory_path().string() + "/recorder_tests";
  fs::remove_all(test_dir);
  fs::create_directories(test_dir);

  std::fprintf(stderr, "\n--- Recorder Pipeline Tests ---\n\n");

  RUN_TEST(test_ring_buffer);
  RUN_TEST(test_max_channels_for_format);
  RUN_TEST(test_unique_filename);
  RUN_TEST(test_wav_mono);
  RUN_TEST(test_wav_stereo);
  RUN_TEST(test_wav_16ch);
  RUN_TEST(test_flac_mono_24bit);
  RUN_TEST(test_flac_stereo_24bit);
  RUN_TEST(test_flac_16ch_grouped);
  RUN_TEST(test_wav_time_split);

  std::fprintf(stderr, "\n%d/%d tests passed\n\n", tests_passed, tests_run);

  fs::remove_all(test_dir);

  return tests_passed == tests_run ? 0 : 1;
}
