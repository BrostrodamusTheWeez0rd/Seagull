# Seagull

A Qt6/C++20 media player and downloader for Windows. Uses yt-dlp for streaming/downloading online video, VLC SDK for local media playback, and ffmpeg for stream processing.

## Build

**Requirements:** Qt 6.11.1 (MSVC 2022 64-bit), MSVC 2022, CMake 3.20+

```powershell
# Configure and build (from repo root)
cd C:\Users\Ryan\source\repos\Seagull
cmake -S Seagull -B Seagull/out/build/x64-Debug -G "Visual Studio 17 2022" -A x64
cmake --build Seagull/out/build/x64-Debug --config Debug
```

Or open the repo in Visual Studio and use the built-in CMake integration (CMakeSettings.json is already configured for x64-Debug).

**Qt path:** `C:/Qt/6.11.1/msvc2022_64`  
**VLC SDK:** `Seagull/sdk/` (headers in `include/vlc/`, libs in `lib/`, plugins in `plugins/`)  
**Build output:** `Seagull/out/build/x64-Debug/`

Post-build steps (run automatically via CMake):
- `windeployqt` copies Qt runtime DLLs
- VLC DLLs (`libvlc.dll`, `libvlccore.dll`) and plugins folder are copied to output
- `Tools/` folder (contains yt-dlp, ffmpeg, deno) is copied to output

## Architecture

`Seagull` (in `Seagull.cpp/.h`) is the top-level orchestrator — it owns all modules and wires up signals/slots.

### Modules (`Seagull/Modules/`)

**UI (`Modules/UI/`)**
- `MainWindow` — main window, hosts the tab widget and the media player
- `Library` — local file browser/player tab
- `Downloads` — download queue and online video tab
- `Search` — search tab
- `Settings` — settings tab
- `Widgets/PlayerControls` — playback controls bar
- `Widgets/PlayerTitleBar` — title bar overlay for the player

**Backend (`Modules/Backend/`)**
- `SgYtDlp` — Qt wrapper around the yt-dlp process. Handles downloading, metadata fetching, stream URL resolution, quality probing, and auto-updating yt-dlp/ffmpeg/deno

### Workers

The orchestrator creates four `SgYtDlp` worker instances to avoid process contention:
- `downloaderWorker` — handles file downloads
- `resolverWorker` — resolves stream URLs
- `prefetcherWorker` — prefetches next item
- `playerWorker` — dedicated to the player's probe/stream-url traffic
- `updaterWorker` — runs on its own `QThread`; checks/updates yt-dlp, ffmpeg, deno at startup

### Key signals

| Signal | From | To | Purpose |
|--------|------|----|---------|
| `playMediaRequested` | Library | MainWindow | Play a local file |
| `playMediaRequested` | Downloads | MainWindow | Play online video |
| `mediaEnded` | MainWindow | Seagull | Auto-advance to next track |
| `skipRequested` | MainWindow | Seagull | Skip forward/back |
| `probeQualitiesRequested` | MainWindow | playerWorker | Get available stream qualities |
| `streamUrlRequested` | MainWindow | playerWorker | Resolve stream URL for a format |

## External Tools

Located in `Seagull/Tools/` (copied to build output):
- `yt-dlp.exe` — video download/stream resolution
- `ffmpeg.exe` — stream processing
- `deno.exe` — used by some yt-dlp scripts

`SgYtDlp::checkForYtDlpUpdate()` chains version checks and auto-downloads updates for all three.

## Code Style

- C++20, Qt6 signals/slots with lambdas
- `Qt::QueuedConnection` for all cross-thread signal connections
- Workers have no parent when moved to a thread (`new SgYtDlp(nullptr)`)
- Module classes inherit from `QWidget`; backend classes inherit from `QObject`
