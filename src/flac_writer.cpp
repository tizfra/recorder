#include "flac_writer.h"

#include <iostream>

namespace recorder {

FlacWriter::FlacWriter(const std::string& filename, int channels, int sample_rate,
                       int bits_per_sample)
    : _filename(filename),
      _channels(channels),
      _sample_rate(sample_rate),
      _bits_per_sample(bits_per_sample) {}

FlacWriter::~FlacWriter() {
  finalize();
  if (_encoder) {
    FLAC__stream_encoder_delete(_encoder);
  }
}

bool FlacWriter::init() {
  _encoder = FLAC__stream_encoder_new();
  if (!_encoder) {
    std::cerr << "Error: failed to create FLAC encoder\n";
    return false;
  }

  FLAC__stream_encoder_set_channels(_encoder, _channels);
  FLAC__stream_encoder_set_sample_rate(_encoder, _sample_rate);
  FLAC__stream_encoder_set_bits_per_sample(_encoder, _bits_per_sample);
  FLAC__stream_encoder_set_compression_level(_encoder, 5);
  FLAC__stream_encoder_set_verify(_encoder, false);

  auto status =
      FLAC__stream_encoder_init_file(_encoder, _filename.c_str(), nullptr, nullptr);
  if (status != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
    std::cerr << "Error: FLAC encoder init failed: "
              << FLAC__StreamEncoderInitStatusString[status] << "\n";
    return false;
  }

  return true;
}

bool FlacWriter::write_samples(const int32_t* interleaved, size_t num_frames) {
  if (!_encoder || _finalized) return false;

  FLAC__bool ok = FLAC__stream_encoder_process_interleaved(_encoder, interleaved,
                                                           static_cast<unsigned>(num_frames));
  if (!ok) {
    auto state = FLAC__stream_encoder_get_state(_encoder);
    std::cerr << "Error: FLAC write failed: " << FLAC__StreamEncoderStateString[state] << "\n";
    return false;
  }

  _total_frames += num_frames;
  return true;
}

bool FlacWriter::finalize() {
  if (!_encoder || _finalized) return true;
  _finalized = true;

  FLAC__bool ok = FLAC__stream_encoder_finish(_encoder);
  if (!ok) {
    auto state = FLAC__stream_encoder_get_state(_encoder);
    std::cerr << "Warning: FLAC finalize issue: " << FLAC__StreamEncoderStateString[state] << "\n";
    return false;
  }
  return true;
}

}  // namespace recorder
