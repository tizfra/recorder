# recorder

Multi-channel USB audio player and recorder. Records from USB audio interfaces via PortAudio to WAV or FLAC files. Plays recorded track and mp3s tracks too (for example as background music)
Includes a GUI with record/pause/stop controls and per-channel VU meters and a remote control thru web page.

## Features

- Records multi-channel audio (1-32 channels) from USB audio interfaces
- Select exactly which channels get written to the output file (default: all channels), including non-contiguous selections — GUI only, checkbox grid under "Recording Channels"
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
- Convenient on-software button for shutdown Raspberry, the app and return to desktop
- Playback of WAV, FLAC, and MP3 files (MP3 automatically resampled if its sample rate doesn't match the audio device)
- Folder browser for playback: navigate subfolders, not just a flat file list
- Play an entire folder sequentially with auto-advance to the next track
- Route playback output to a specific channel pair on the interface (e.g. channels 5-6), independent of the file's own channel count
- Playback volume control (dB), applied live without interrupting playback, default -5dB to tame "hot" MP3 masters
- You can control all the software with your any browser simpli connecting at the address write on the software

## Building

### Linux (Raspberry Pi / Debian)

Raspberry Pi OS Bookworm and later use **labwc**, a Wayland compositor, as the default window manager. The build requires GLFW's native Wayland backend (not just X11/XWayland) for reliable fullscreen and window-focus behavior under labwc:

```bash
sudo apt install build-essential cmake portaudio19-dev libflac-dev
# For GUI support:
sudo apt install libgl-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxkbcommon-dev
# For native Wayland support (required on Raspberry Pi OS Bookworm+/labwc):
sudo apt install libwayland-dev wayland-protocols
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

> If you're rebuilding after an earlier build predating Wayland support, delete the `build` directory first (`rm -rf build`) — GLFW's Wayland option is a CMake cache variable and won't pick up otherwise.

### To make the shutdown button work

The shutdown button runs `sudo systemctl poweroff`. To allow that without a password prompt:

```bash
sudo visudo
#YOUR_USERNAME
ALL=(ALL) NOPASSWD: /usr/bin/systemctl poweroff
```

### To remove the window title bar (labwc)

On Wayland, disabling window decorations needs a rule in labwc's own config, not just an app-side request:

```bash
mkdir -p ~/.config/labwc
cp /etc/xdg/labwc/rc.xml ~/.config/labwc/rc.xml
```

Make sure `~/.config/labwc/rc.xml` contains, inside `<windowRules>...</windowRules>`:

```xml
<windowRule identifier="audio-recorder" serverDecoration="no" />
```

(`audio-recorder` is the app_id the app sets explicitly on Wayland.) Restart the Pi or graphical session to apply.

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

> Note: selecting a specific subset of channels to record (as opposed to a plain channel count) is currently GUI-only, not exposed as a CLI flag.

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

## Remote control

The GUI starts a small web server on port 8080. The address to connect to is shown directly in the GUI, under the Record/Playback toggle, and printed to the console on startup. Open that address from a phone or tablet on the same network to control recording, playback, folder navigation, volume, and playlists remotely.

## Autostart on Raspberry Pi

### 1. Enable desktop autologin

```bash
sudo raspi-config
```

Go to **System Options** > **Boot / Auto Login** > **Desktop Autologin**.

### 2. Create the autostart entry

Raspberry Pi OS Bookworm+ uses labwc, which reads its own autostart script rather than (or in addition to) the XDG `~/.config/autostart/*.desktop` mechanism. Create a per-user copy so it survives OS updates:

```bash
mkdir -p ~/.config/labwc
cp /etc/xdg/labwc/autostart ~/.config/labwc/autostart
```

Add the app at the end of `~/.config/labwc/autostart`:

```bash
/home/br/src/recorder/start.sh &
```

The included `start.sh` waits for the Wayland compositor (labwc) to be ready before launching the GUI, so the window appears reliably on every boot — no X server or xdpyinfo involved, since this is a Wayland session, not X11.

> The system taskbar (`wf-panel-pi`) is left running by default — handy for WiFi, USB disk management, and copy/paste while the app is minimized via its Desktop button. If you want a fully dedicated kiosk screen instead, `wf-panel-pi` can be disabled from `~/.config/labwc/autostart` too, but note the app's Desktop button then has no taskbar to return from.

## Architecture

```
USB Interface -> PortAudio (paInt32) -> RT Callback -> SPSC Ring Buffer -> Writer Thread -> WAV/FLAC -> disk
```

The PortAudio callback runs on a real-time thread and writes to a lock-free single-producer single-consumer ring buffer. A separate writer thread reads from the buffer and encodes to WAV or FLAC, applying the recording channel selection (if any) before writing. This keeps the real-time path free of allocations, locks, and I/O.

Playback follows a mirrored design: a reader thread decodes WAV/FLAC/MP3 into a ring buffer (resampling on the fly if needed), and the PortAudio output callback consumes it, applying channel routing and volume gain in real time.

The GUI renders at ~20fps using ImGui + GLFW (native Wayland backend) + OpenGL to keep CPU usage low on the Pi. An AudioMonitor provides live VU metering when not recording; during recording, levels come from the main PortAudio callback. Files are capped at 3.9GB and automatically split to stay within FAT32 limits.

The remote control web server runs on its own thread and communicates with the GUI thread only through a command queue and a published status snapshot — it never touches PortAudio/Recorder/AudioPlayer directly, avoiding any cross-thread audio API calls.
