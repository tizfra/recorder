#include "audio_reader.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include <FLAC/stream_decoder.h>

#define MINIMP3_IMPLEMENTATION
#include <minimp3_ex.h>

namespace recorder {

namespace {

// --- WAV reader -------------------------------------------------------

class WavReader : public AudioReader {
 public:
  explicit WavReader(std::string path) : path_(std::move(path)) {}
  ~WavReader() override { if (fp_) std::fclose(fp_); }

  bool init() override {
    fp_ = std::fopen(path_.c_str(), "rb");
    if (!fp_) return false;

    char riff[4], wave[4];
    if (std::fread(riff, 1, 4, fp_) != 4 || std::memcmp(riff, "RIFF", 4) != 0) return false;
    std::fseek(fp_, 4, SEEK_CUR);  // chunk size
    if (std::fread(wave, 1, 4, fp_) != 4 || std::memcmp(wave, "WAVE", 4) != 0) return false;

    bool got_fmt = false;
    while (true) {
      char id[4];
      uint32_t size;
      if (std::fread(id, 1, 4, fp_) != 4) break;
      if (std::fread(&size, 4, 1, fp_) != 1) break;

      if (std::memcmp(id, "fmt ", 4) == 0) {
        uint16_t audio_format, num_channels, block_align, bits;
        uint32_t sample_rate, byte_rate;
        std::fread(&audio_format, 2, 1, fp_);
        std::fread(&num_channels, 2, 1, fp_);
        std::fread(&sample_rate, 4, 1, fp_);
        std::fread(&byte_rate, 4, 1, fp_);
        std::fread(&block_align, 2, 1, fp_);
        std::fread(&bits, 2, 1, fp_);
        channels_ = num_channels;
        sample_rate_ = sample_rate;
        bits_per_sample_ = bits;
        if (size > 16) std::fseek(fp_, size - 16, SEEK_CUR);  // salta chunk extension
        got_fmt = true;
      } else if (std::memcmp(id, "data", 4) == 0) {
        data_start_ = std::ftell(fp_);
        data_size_ = size;
        break;  // assumiamo data dopo fmt, valido per i file che scrive questo progetto
      } else {
        std::fseek(fp_, size, SEEK_CUR);
      }
    }

    if (!got_fmt || data_size_ == 0 || channels_ <= 0) return false;
    int bytes_per_sample = bits_per_sample_ / 8;
    total_frames_ = data_size_ / (bytes_per_sample * channels_);
    std::fseek(fp_, data_start_, SEEK_SET);
    return true;
  }

  size_t read_samples(int32_t* out, size_t num_frames) override {
    int bytes_per_sample = bits_per_sample_ / 8;
    size_t frame_bytes = bytes_per_sample * channels_;
    std::vector<uint8_t> raw(num_frames * frame_bytes);
    size_t got = std::fread(raw.data(), frame_bytes, num_frames, fp_);

    for (size_t i = 0; i < got * channels_; ++i) {
      const uint8_t* p = &raw[i * bytes_per_sample];
      int32_t sample = 0;
      if (bits_per_sample_ == 16) {
        sample = static_cast<int16_t>(p[0] | (p[1] << 8));
        sample <<= 16;  // porta a scala int32 come fa il resto della pipeline
      } else if (bits_per_sample_ == 24) {
        int32_t v = p[0] | (p[1] << 8) | (p[2] << 16);
        if (v & 0x800000) v |= 0xFF000000;  // sign extend
        sample = v << 8;
      } else if (bits_per_sample_ == 32) {
        sample = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
      }
      out[i] = sample;
    }
    frames_read_ += got;
    return got;
  }

  bool seek(uint64_t frame) override {
    int bytes_per_sample = bits_per_sample_ / 8;
    long offset = data_start_ + static_cast<long>(frame) * bytes_per_sample * channels_;
    if (std::fseek(fp_, offset, SEEK_SET) != 0) return false;
    frames_read_ = frame;
    return true;
  }

  int channels() const override { return channels_; }
  int sample_rate() const override { return sample_rate_; }
  int bits_per_sample() const override { return bits_per_sample_; }
  uint64_t total_frames() const override { return total_frames_; }

 private:
  std::string path_;
  FILE* fp_ = nullptr;
  long data_start_ = 0;
  uint32_t data_size_ = 0;
  int channels_ = 0;
  int sample_rate_ = 0;
  int bits_per_sample_ = 0;
  uint64_t total_frames_ = 0;
  uint64_t frames_read_ = 0;
};

// --- FLAC reader --------------------------------------------------------
// Usa il decoder a callback di libFLAC; bufferizza i frame decodificati
// internamente perche' read_samples() deve restituire esattamente cio'
// che viene richiesto.

class FlacReader : public AudioReader {
 public:
  explicit FlacReader(std::string path) : path_(std::move(path)) {}
  ~FlacReader() override {
    if (decoder_) {
      FLAC__stream_decoder_finish(decoder_);
      FLAC__stream_decoder_delete(decoder_);
    }
  }

  bool init() override {
    decoder_ = FLAC__stream_decoder_new();
    if (!decoder_) return false;
    FLAC__stream_decoder_set_metadata_respond(decoder_, FLAC__METADATA_TYPE_STREAMINFO);
    auto status = FLAC__stream_decoder_init_file(
        decoder_, path_.c_str(), write_cb, metadata_cb, error_cb, this);
    if (status != FLAC__STREAM_DECODER_INIT_STATUS_OK) return false;
    if (!FLAC__stream_decoder_process_until_end_of_metadata(decoder_)) return false;
    return channels_ > 0 && total_frames_ > 0;
  }

  size_t read_samples(int32_t* out, size_t num_frames) override {
    size_t produced = 0;
    while (produced < num_frames) {
      if (pending_pos_ < pending_.size() / channels_) {
        size_t avail = pending_.size() / channels_ - pending_pos_;
        size_t take = std::min(avail, num_frames - produced);
        std::memcpy(&out[produced * channels_], &pending_[pending_pos_ * channels_],
                    take * channels_ * sizeof(int32_t));
        pending_pos_ += take;
        produced += take;
        continue;
      }
      pending_.clear();
      pending_pos_ = 0;
      if (eof_ || !FLAC__stream_decoder_process_single(decoder_)) break;
      auto state = FLAC__stream_decoder_get_state(decoder_);
      if (state == FLAC__STREAM_DECODER_END_OF_STREAM) { eof_ = true; break; }
    }
    return produced;
  }

  bool seek(uint64_t frame) override {
    if (!FLAC__stream_decoder_seek_absolute(decoder_, frame)) return false;
    pending_.clear();
    pending_pos_ = 0;
    eof_ = false;
    return true;
  }

  int channels() const override { return channels_; }
  int sample_rate() const override { return sample_rate_; }
  int bits_per_sample() const override { return bits_per_sample_; }
  uint64_t total_frames() const override { return total_frames_; }

 private:
  static FLAC__StreamDecoderWriteStatus write_cb(const FLAC__StreamDecoder*,
                                                  const FLAC__Frame* frame,
                                                  const FLAC__int32* const buffer[],
                                                  void* client) {
    auto* self = static_cast<FlacReader*>(client);
    size_t n = frame->header.blocksize;
    size_t base = self->pending_.size();
    self->pending_.resize(base + n * self->channels_);
    int shift = 32 - self->bits_per_sample_;
    for (size_t i = 0; i < n; ++i) {
      for (int c = 0; c < self->channels_; ++c) {
        self->pending_[base + i * self->channels_ + c] = buffer[c][i] << shift;
      }
    }
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
  }

  static void metadata_cb(const FLAC__StreamDecoder*, const FLAC__StreamMetadata* meta, void* client) {
    auto* self = static_cast<FlacReader*>(client);
    if (meta->type == FLAC__METADATA_TYPE_STREAMINFO) {
      self->channels_ = meta->data.stream_info.channels;
      self->sample_rate_ = meta->data.stream_info.sample_rate;
      self->bits_per_sample_ = meta->data.stream_info.bits_per_sample;
      self->total_frames_ = meta->data.stream_info.total_samples;
    }
  }

  static void error_cb(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus, void*) {}

  std::string path_;
  FLAC__StreamDecoder* decoder_ = nullptr;
  int channels_ = 0, sample_rate_ = 0, bits_per_sample_ = 0;
  uint64_t total_frames_ = 0;
  std::vector<int32_t> pending_;
  size_t pending_pos_ = 0;
  bool eof_ = false;
};

// --- MP3 reader -----------------------------------------------------------
// minimp3_ex decodifica l'intero file in un colpo solo (mp3dec_ex_open):
// per la durata tipica di un mp3 va benissimo e semplifica moltissimo
// seek()/read_samples() rispetto a uno streaming decoder incrementale.

class Mp3Reader : public AudioReader {
 public:
  explicit Mp3Reader(std::string path) : path_(std::move(path)) {}
  ~Mp3Reader() override { mp3dec_ex_close(&dec_); }

  bool init() override {
    if (mp3dec_ex_open(&dec_, path_.c_str(), MP3D_SEEK_TO_SAMPLE) != 0) return false;
    channels_ = dec_.info.channels;
    sample_rate_ = dec_.info.hz;
    bits_per_sample_ = 16;  // minimp3 decodifica sempre a 16 bit
    if (channels_ <= 0) return false;
    total_frames_ = dec_.samples / static_cast<uint64_t>(channels_);
    return total_frames_ > 0;
  }

  size_t read_samples(int32_t* out, size_t num_frames) override {
    std::vector<mp3d_sample_t> buf(num_frames * channels_);
    size_t got_samples = mp3dec_ex_read(&dec_, buf.data(), buf.size());
    size_t got_frames = got_samples / static_cast<size_t>(channels_);
    for (size_t i = 0; i < got_frames * static_cast<size_t>(channels_); ++i) {
      out[i] = static_cast<int32_t>(buf[i]) << 16;  // 16 -> 32 bit, coerente col resto della pipeline
    }
    return got_frames;
  }

  bool seek(uint64_t frame) override {
    return mp3dec_ex_seek(&dec_, frame * static_cast<uint64_t>(channels_)) == 0;
  }

  int channels() const override { return channels_; }
  int sample_rate() const override { return sample_rate_; }
  int bits_per_sample() const override { return bits_per_sample_; }
  uint64_t total_frames() const override { return total_frames_; }

 private:
  std::string path_;
  mp3dec_ex_t dec_{};
  int channels_ = 0, sample_rate_ = 0, bits_per_sample_ = 0;
  uint64_t total_frames_ = 0;
};

bool has_extension(const std::string& filename, const char* ext) {
  size_t len = std::strlen(ext);
  return filename.size() >= len && filename.compare(filename.size() - len, len, ext) == 0;
}

}  // namespace

std::unique_ptr<AudioReader> create_reader(const std::string& filename) {
  std::unique_ptr<AudioReader> reader;
  if (has_extension(filename, ".flac")) {
    reader = std::make_unique<FlacReader>(filename);
  } else if (has_extension(filename, ".mp3")) {
    reader = std::make_unique<Mp3Reader>(filename);
  } else {
    reader = std::make_unique<WavReader>(filename);
  }
  if (!reader->init()) return nullptr;
  return reader;
}

}  // namespace recorder
