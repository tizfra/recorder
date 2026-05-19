# recorder

Multi-channel USB audio recorder. Records from USB audio interfaces via PortAudio to WAV or FLAC files. Includes a GUI with record/pause/stop controls and per-channel VU meters.

## Features

- Records multi-channel audio (1-32 channels) from USB audio interfaces
- WAV (default) and FLAC output, selected by file extension
- 16 or 24-bit, up to 192kHz
- Time-based file splitting (default: 30 minutes)
- File size cap at 3.9GB (safe for FAT32 USB drives)
- Never overwrites existing files (auto-increments filenames)
- GUI with record/pause/stop buttons and vertical per-channel VU meters
- Live level metering before recording starts
- Auto-detects USB audio interfaces (prefers `hw:` devices on Linux)
- Auto-detects mounted USB disks for output path
- Hot-plugging: GUI detects USB audio devices and disks inserted/removed while idle
- FLAC with >8 channels automatically splits into groups of 8 (WAV has no channel limit)

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
  -o, --output <file>        Output filename, .wav or .flac (default: <usb disk>/recording.wav)
  -t, --duration <seconds>   Recording duration, 0 = until Ctrl+C (default: 0)
  -s, --split <minutes>      Split into new file every N minutes (default: 30)
  -g, --gui                  Launch with graphical interface
  -h, --help                 Show help
```

### Examples

```bash
# List devices
recorder --list-devices

# Record with all defaults (WAV, USB disk, 30-minute splits)
recorder

# 8 channels from device 3
recorder -d 3 -c 8 -o session.wav

# 2 channels, 60 seconds
recorder -c 2 -t 60 -o take.wav

# FLAC output (selected by extension)
recorder -o session.flac

# GUI mode
recorder --gui

# GUI with 10-minute splits
recorder --gui -s 10
```

## Autostart on Raspberry Pi

### 1. Enable desktop autologin

```bash
sudo raspi-config
```

Go to **System Options** > **Boot / Auto Login** > **Desktop Autologin**.

### 2. Install xdpyinfo (if not already present)

```bash
sudo apt install x11-utils
```

### 3. Create autostart entry

```bash
mkdir -p ~/.config/autostart

cat > ~/.config/autostart/recorder.desktop << 'EOF'
[Desktop Entry]
Type=Application
Name=Audio Recorder
Exec=/home/br/src/recorder/start.sh
EOF
```

The included `start.sh` waits for the X server to be ready before launching the GUI, so the window appears reliably on every boot.

## Architecture

```
USB Interface -> PortAudio (paInt32) -> RT Callback -> SPSC Ring Buffer -> Writer Thread -> WAV/FLAC -> disk
```

The PortAudio callback runs on a real-time thread and writes to a lock-free single-producer single-consumer ring buffer. A separate writer thread reads from the buffer and encodes to WAV or FLAC. This keeps the real-time path free of allocations, locks, and I/O.

The GUI renders at ~20fps using ImGui + GLFW + OpenGL to keep CPU usage low on the Pi. An AudioMonitor provides live VU metering when not recording; during recording, levels come from the main PortAudio callback. Files are capped at 3.9GB and automatically split to stay within FAT32 limits.
