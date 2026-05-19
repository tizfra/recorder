#pragma once

#include <FLAC/stream_decoder.h>

#include <cstdint>
#include <string>
#include <vector>

struct FlacData {
  int channels = 0;
  int sample_rate = 0;
  int bits_per_sample = 0;
  std::vector<int32_t> samples;

  size_t num_frames() const { return channels > 0 ? samples.size() / channels : 0; }
};

namespace detail {

inline FLAC__StreamDecoderWriteStatus flac_write_cb(const FLAC__StreamDecoder*,
                                                     const FLAC__Frame* frame,
                                                     const FLAC__int32* const buffer[],
                                                     void* client_data) {
  auto* data = static_cast<FlacData*>(client_data);
  unsigned channels = frame->header.channels;
  unsigned frames = frame->header.blocksize;

  for (unsigned f = 0; f < frames; ++f) {
    for (unsigned c = 0; c < channels; ++c) {
      data->samples.push_back(buffer[c][f]);
    }
  }
  return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

inline void flac_metadata_cb(const FLAC__StreamDecoder*, const FLAC__StreamMetadata* metadata,
                              void* client_data) {
  if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
    auto* data = static_cast<FlacData*>(client_data);
    data->channels = metadata->data.stream_info.channels;
    data->sample_rate = metadata->data.stream_info.sample_rate;
    data->bits_per_sample = metadata->data.stream_info.bits_per_sample;
  }
}

inline void flac_error_cb(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus, void*) {}

}  // namespace detail

inline FlacData read_flac(const std::string& path) {
  FlacData data;

  FLAC__StreamDecoder* decoder = FLAC__stream_decoder_new();
  if (!decoder) return data;

  auto status = FLAC__stream_decoder_init_file(decoder, path.c_str(), detail::flac_write_cb,
                                                detail::flac_metadata_cb, detail::flac_error_cb,
                                                &data);
  if (status == FLAC__STREAM_DECODER_INIT_STATUS_OK) {
    FLAC__stream_decoder_process_until_end_of_stream(decoder);
  }

  FLAC__stream_decoder_finish(decoder);
  FLAC__stream_decoder_delete(decoder);
  return data;
}
