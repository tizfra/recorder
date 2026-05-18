#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>

#include "cli.h"
#include "device_list.h"
#include "recorder.h"

static std::atomic<bool> g_running{true};

static void signal_handler(int) { g_running.store(false, std::memory_order_relaxed); }

int main(int argc, char* argv[]) {
  auto cfg = recorder::parse_args(argc, argv);
  if (!cfg) return 1;

  if (cfg->list_devices) {
    recorder::print_input_devices();
    return 0;
  }

  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  auto stats = recorder::run_recording(*cfg, g_running);

  double duration = static_cast<double>(stats.frames_recorded) / cfg->sample_rate;
  std::cerr << "\nDone. " << stats.frames_recorded << " frames (" << duration << "s)";
  if (stats.files_written > 1) {
    std::cerr << " across " << stats.files_written << " files";
  }
  std::cerr << "\n";
  if (stats.overruns > 0) {
    std::cerr << "Warning: " << stats.overruns << " frames dropped (buffer overrun)\n";
  }

  return 0;
}
