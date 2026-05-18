# recorder

Multi-channel USB audio recorder. Records from USB audio interfaces via PortAudio and encodes to FLAC. Includes a GUI mode with record/pause/stop controls.

## Features

- Records multi-channel audio (1-32 channels) from USB audio interfaces
- FLAC encoding (16 or 24-bit, up to 192kHz)
- Time-based file splitting (default: 30 minutes)
- GUI mode with ImGui (record/pause/stop, live elapsed time, overrun count)
- Auto-detects USB audio interfaces (prefers `hw:` devices on Linux)
- Auto-detects mounted USB disks for output
- Never overwrites existing files (auto-increments filenames)
- Hot-plugging: GUI detects USB audio devices and disks inserted/removed while idle

## Building

### macOS

```bash
brew install cmake portaudio flac
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Linux (Raspberry Pi / Debian)

```bash
sudo apt install build-essential cmake libportaudio19-dev libflac-dev
# For GUI support:
sudo apt install libgl-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxkbcommon-dev
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Without GUI (headless)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_GUI=OFF
cmake --build build
```

## Usage

```
recorder [options]

Options:
  -l, --list-devices         List available input devices and exit
  -d, --device <index>       Input device index (default: USB interface, or first available)
  -c, --channels <n>         Number of channels, 1-32 (default: device max)
  -r, --rate <hz>            Sample rate in Hz (default: 48000)
  -b, --bits <n>             Bit depth: 16 or 24 (default: 24)
  -o, --output <file>        Output FLAC filename (default: <usb disk>/recording.flac)
  -t, --duration <seconds>   Recording duration, 0 = until Ctrl+C (default: 0)
  -s, --split <minutes>      Split into new file every N minutes (default: 30)
  -g, --gui                  Launch with graphical interface
  -h, --help                 Show help
```

### Examples

```bash
# List devices
recorder --list-devices

# Record 8 channels from device 3
recorder -d 3 -c 8 -r 48000 -b 24 -o session.flac

# Record for 60 seconds
recorder -t 60

# GUI mode
recorder --gui

# GUI with 10-minute splits
recorder --gui -s 10

# Just run with defaults (auto-selects USB interface, full channel count,
# writes to USB disk if mounted, splits every 30 minutes)
recorder
```

## Architecture

```
USB Interface -> PortAudio (paInt32) -> RT Callback -> SPSC Ring Buffer -> Writer Thread -> libFLAC -> .flac
```

The PortAudio callback runs on a real-time thread and writes to a lock-free single-producer single-consumer ring buffer. A separate writer thread reads from the buffer and encodes to FLAC. This keeps the real-time path free of allocations, locks, and I/O.
