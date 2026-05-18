#pragma once

#include <atomic>
#include <cstdint>

#include "cli.h"

namespace recorder {

struct RecordingStats {
  uint64_t frames_recorded = 0;
  uint64_t overruns = 0;
  int files_written = 0;
};

RecordingStats run_recording(const Config& config, std::atomic<bool>& running);

}  // namespace recorder
