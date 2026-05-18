#include "device_list.h"

#include <portaudio.h>

#include <cstdio>

namespace recorder {

void print_input_devices() {
  Pa_Initialize();

  int count = Pa_GetDeviceCount();
  if (count <= 0) {
    std::fprintf(stderr, "No audio devices found.\n");
    Pa_Terminate();
    return;
  }

  std::printf("%-5s  %-50s  %8s  %10s\n", "Index", "Name", "Channels", "Rate (Hz)");
  std::printf("%-5s  %-50s  %8s  %10s\n", "-----", "--------------------------------------------------",
              "--------", "----------");

  for (int i = 0; i < count; ++i) {
    const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
    if (!info || info->maxInputChannels <= 0) continue;

    std::printf("%-5d  %-50.50s  %8d  %10.0f\n", i, info->name, info->maxInputChannels,
                info->defaultSampleRate);
  }

  Pa_Terminate();
}

}  // namespace recorder
