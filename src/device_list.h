#pragma once

#include <optional>
#include <string>
#include <vector>

namespace recorder {

struct DeviceInfo {
  int index;
  int max_input_channels;
  std::string name;
};

void print_input_devices();
std::optional<DeviceInfo> find_preferred_device();
std::optional<DeviceInfo> get_device_info(int index);
std::vector<DeviceInfo> scan_input_devices();
bool is_usb_device(const std::string& name);

}  // namespace recorder
