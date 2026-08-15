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
// Restituisce il primo disco USB scrivibile trovato (comportamento
// storico, usato quando ne e' presente uno solo).
std::string find_usb_disk();
// Restituisce TUTTI i dischi USB scrivibili trovati, stessa logica di
// find_usb_disk() ma senza fermarsi al primo. Usata per il selettore
// disco quando ne sono collegati piu' di uno contemporaneamente.
std::vector<std::string> scan_usb_disks();
std::string unique_filename(const std::string& path);

}  // namespace recorder
