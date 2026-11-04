# InstantReplay

> **Disclaimer:** This project is **100% vibecoded** for personal use and sharing with friends. Use at your own risk!

Ultra-lightweight (~0.1% CPU) Windows screen replay recorder powered by **DXGI Desktop Duplication** and **NVIDIA NVENC SDK v13** (AV1 / HEVC / H.264) with active-window process-loopback audio capture and one-click clip sharing.

## Features
- **Zero-Copy GPU Pipeline**: Captures desktop textures via `IDXGIOutputDuplication` and feeds them directly to `nvEncodeAPI64.dll` in VRAM without CPU readback.
- **AV1 / HEVC / H.264 Hardware Encoding**: Defaults to 9th-gen NVENC AV1 (Preset P5 High Quality, 1s GOP) with automatic fallback to H.264.
- **Active-Window Audio Isolation**: Uses Windows Process Loopback (`VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK`) + Media Foundation AAC-LC to record only the foreground application's audio tree (or full desktop audio / muted).
- **RAM Ring Buffer**: Continuously maintains the last *N* seconds (default 30s) of encoded packets in memory; pressing `Alt+F10` muxes a faststart (`moov`-first) `.mp4` on a lowest-priority background thread.
- **Bitrate Sweep Debug Mode**: Simultaneously encodes multiple bitrates (e.g., `4, 8, 14, 24 Mbps`) from the same captured GPU frame so a single hotkey press saves side-by-side comparison files.
- **Portable Single-File Binary**: Statically linked (`InstantReplay.exe`, ~350 KB) with zero external DLL dependencies. Auto-generates `config.ini` next to itself and falls back to `./captures/` if the configured drive is absent.
- **Self-Hosted Share Server (`server/`)**: Zero-dependency Python HTTP server + `Dockerfile` / `docker-compose.yml` for NAS deployment with Discord OpenGraph video embeds and HTTP Range scrubbing.

## Quick Start (Portable)
1. Download `InstantReplay.exe` from Releases and place it in any folder.
2. Run `InstantReplay.exe` — a green icon appears in the Windows system tray.
3. Press **`Alt+F10`** at any time to save the last 30 seconds.
4. Right-click the tray icon to switch monitors, change audio source, toggle **Start with Windows**, enable **Bitrate Sweep Mode**, or upload clips.

## Building from Source (MSYS2 Clang64 + Ninja)
```powershell
$env:PATH = "C:\msys64\clang64\bin;C:\msys64\mingw64\bin;" + $env:PATH
cmake -B build -G Ninja -DCMAKE_CXX_COMPILER=C:/msys64/clang64/bin/clang++.exe
cmake --build build
```
