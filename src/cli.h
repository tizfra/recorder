#pragma once

#include <optional>
#include <string>
#include <vector>

namespace recorder {

struct Config {
  int device_index = -1;
  int channels = 2;
  int sample_rate = 48000;
  int bit_depth = 24;
  std::string output_file = "recording.wav";
  std::string output_file_base;
  double duration_seconds = 0.0;
  double split_seconds = 1800.0;
  bool list_devices = false;
  bool gui = false;
  // Indici 0-based (all'interno dei primi `channels` canali dello stream
  // aperto su PortAudio) dei canali da scrivere effettivamente nel file
  // di output. Vuoto (default) = registra tutti i `channels` canali —
  // comportamento identico a prima dell'introduzione di questo campo,
  // sia da GUI che da riga di comando. Messo in fondo allo struct per
  // non alterare l'ordine dei campi esistenti (nel caso cli.cpp o altro
  // codice costruisca Config con inizializzazione posizionale).
  std::vector<int> record_channels;
};

std::optional<Config> parse_args(int argc, char* argv[]);

}  // namespace recorder
