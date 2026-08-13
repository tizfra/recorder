# recorder

Multi-channel USB audio player and recorder. Records from USB audio interfaces via PortAudio to WAV or FLAC files. Plays back recorded tracks and MP3s too (e.g. as background music). Includes a touchscreen GUI with record/pause/stop controls, per-channel VU meters, and a remote control web page.

## Features

### Recording
- Records multi-channel audio (1-32 channels) from USB audio interfaces
- Select exactly which channels get written to the output file (default: all channels), including non-contiguous selections — GUI only, checkbox grid under "Recording Channels"
- WAV (default) and FLAC output, selected by file extension
- 16 or 24-bit, up to 192kHz
- FLAC with >8 channels automatically splits into groups of max 8 channels per file (WAV has no channel limit); filenames include the channel range whenever the recording isn't "all channels" (e.g. `_ch01-08`, or `_ch01-02_09-10` for non-contiguous selections)
- Time-based file splitting (default: 30 minutes), file size cap at 3.9GB (safe for FAT32 USB drives)
- Never overwrites existing files (auto-increments filenames)
- Live level metering before recording starts; VU meters always show all device channels, independent of which ones are actually selected for recording
- Auto-detects USB audio interfaces (prefers `hw:` devices on Linux) and mounted USB disks for output path
- Hot-plugging: GUI detects USB audio devices and disks inserted/removed while idle

### Playback
- WAV, FLAC, and MP3, with automatic linear resampling if a file's sample rate doesn't match the audio device
- Folder browser: navigate subfolders, natural/numeric filename sorting (`take2` before `take10`)
- **Group split files**: optionally reunites multi-file FLAC splits (e.g. a 32-channel recording saved as 4 files of 8 channels each) into a single synchronized playback entry — plays all files together, sample-accurate, as if it were one recording. Works with any number of files, not just 4. Toggle via a checkbox in the GUI or the web remote.
- Play an entire folder sequentially with auto-advance; Prev/Next track buttons
- Route playback output to a specific channel pair on the interface (e.g. channels 5-6), independent of the file's own channel count
- Playback volume control (dB), applied live without interrupting playback, default -5dB to tame "hot" MP3 masters
- Immediate Pause/Stop (no audible tail from buffered audio)

### GUI
- Borderless window sized to the screen (not exclusive fullscreen — more reliable minimize/restore under labwc), no title bar
- CPU temperature and RAM usage shown live
- Convenient on-screen buttons for Desktop (minimize), Quit, and Shutdown — available in both Record and Playback screens, with confirmation dialogs
- Safe USB eject: unmounts and powers off the drive, with an honest status message if the power-off step fails (data is still safe either way — see note below)

### Remote control
- Built-in web server (port 8080); the address to connect to is shown live in the GUI and printed at startup
- Mirrors nearly everything the local GUI can do: record/pause/stop, channel selection state, playback (including grouped multi-file playback), Prev/Next, folder navigation, volume, "Group split files" toggle, Record/Playback mode switch, CPU temp/RAM
- Highlights the track actually playing (not just what you've tapped locally)

## Building

### Linux (Raspberry Pi / Debian)

Raspberry Pi OS Bookworm and later use **labwc**, a Wayland compositor, as the default window manager — not X11. The build needs GLFW's native Wayland backend for reliable window behavior (fullscreen/focus issues are common when running GLFW through XWayland under labwc):

```bash
sudo apt install build-essential cmake portaudio19-dev libflac-dev
# GUI dependencies (X11 backend, kept for portability):
sudo apt install libgl-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxkbcommon-dev
# Native Wayland backend (required for reliable behavior under labwc):
sudo apt install libwayland-dev wayland-protocols

mkdir -p build && cd build
cmake ..
cmake --build . -j4
```

GLFW, Dear ImGui, minimp3 (MP3 decoding), and cpp-httplib (remote control server) are downloaded and built automatically by CMake (`FetchContent`) — no need to install them separately.

> Rebuilding after an earlier build that predates Wayland support: delete the `build` directory first (`rm -rf build`) — GLFW's Wayland option is a CMake cache variable and won't otherwise pick up the change.

### Headless build (no GUI)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_GUI=OFF
cmake --build build
```

## One-time setup on the Pi

### 1. Passwordless shutdown

The Shutdown button runs `sudo systemctl poweroff`. To allow that without a password prompt:

```bash
sudo visudo
```

Add at the end (replace `YOUR_USERNAME`):

```
YOUR_USERNAME ALL=(ALL) NOPASSWD: /usr/bin/systemctl poweroff
```

### 2. Remove the window title bar (labwc)

On Wayland, disabling window decorations needs a rule in labwc's own config — the app requesting it isn't enough by itself:

```bash
mkdir -p ~/.config/labwc
cp /etc/xdg/labwc/rc.xml ~/.config/labwc/rc.xml
```

Make sure `~/.config/labwc/rc.xml` contains, inside `<windowRules>...</windowRules>`:

```xml
<windowRule identifier="audio-recorder" serverDecoration="no" />
```

(`audio-recorder` is the app_id the app sets explicitly when running on Wayland.) Restart the Pi or the graphical session to apply.

### 3. Autostart on boot

```bash
mkdir -p ~/.config/labwc
cp /etc/xdg/labwc/autostart ~/.config/labwc/autostart
```

Add at the end of `~/.config/labwc/autostart`:

```bash
/path/to/recorder/start.sh &
```

`start.sh` waits for the Wayland compositor's socket to be ready before launching the GUI, so the window appears reliably on every boot.

> The system taskbar (`wf-panel-pi`) is left running by default — it's what the Desktop button switches you to, useful for WiFi, USB disk management, and copy/paste. If you'd rather not have a taskbar at all, it can be disabled from the same `autostart` file, but then the Desktop button has nowhere to send you.

## Usage

### CLI

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

> Selecting a specific subset of channels to record (as opposed to a plain channel count) is GUI-only for now, not exposed as a CLI flag.

```bash
recorder --list-devices                # list devices
recorder                               # defaults: WAV, USB disk, 30-minute splits
recorder -d 3 -c 8 -o session.wav      # 8 channels from device 3
recorder -c 2 -t 60 -o take.wav        # 2 channels, 60 seconds
recorder -o session.flac               # FLAC output (selected by extension)
recorder --gui                         # GUI mode
recorder --gui -s 10                   # GUI with 10-minute splits
```

### Remote control

Open the address shown in the GUI (or printed at startup) from a phone or tablet's browser, on the same network. No login/pairing needed — treat the network as trusted, since anyone on it can control the app.

## Architecture

```
USB Interface -> PortAudio (paInt32) -> RT Callback -> SPSC Ring Buffer -> Writer Thread -> WAV/FLAC -> disk
```

The PortAudio input callback runs on a real-time thread and writes to a lock-free single-producer single-consumer ring buffer. A separate writer thread reads from the buffer, applies the recording channel selection (if any), and encodes to WAV or FLAC. This keeps the real-time path free of allocations, locks, and I/O.

Playback mirrors this: one or more files (grouped for synchronized multi-file playback) are read in lockstep by a reader thread into a combined ring buffer, resampled on the fly if needed (single-file only), and the PortAudio output callback consumes it, applying channel routing and volume gain in real time. Pausing goes silent immediately without draining the buffer, so resuming continues exactly where it left off.

The GUI renders at ~20fps using ImGui + GLFW (native Wayland backend) + OpenGL. An `AudioMonitor` provides live VU metering when not recording; during recording, levels come from the main PortAudio callback and always cover every device channel, regardless of the recording channel selection.

The remote control web server runs on its own thread and talks to the GUI thread only through a command queue and a published status snapshot — it never touches PortAudio/Recorder/AudioPlayer directly, so there's no cross-thread audio API access.

## Known limitations

- USB eject: the app unmounts the drive and attempts a full USB power-off (`udisksctl power-off`). If power-off fails, the app tells you explicitly rather than reporting a false success — the data is safe either way (the filesystem was cleanly unmounted), but the drive may not be electrically "safe to unplug" until it succeeds. Some Linux desktop environments show a generic "device removed" notification for *any* USB disconnect (clean or not) — if you see this immediately when pressing Eject rather than after physically removing the drive, it's very likely that generic notification, not a real problem.
- Grouped multi-file playback requires all files in the group to share the same sample rate; automatic resampling is not supported when playing multiple files together (only for single files, e.g. MP3s at a different rate than the device).
- The remote control web page has no authentication — anyone on the same network can control the app.
