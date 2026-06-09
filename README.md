# Seagull

A Qt6/C++20 media player and downloader for Windows. Seagull plays local files and online video in one unified player, streams or downloads from any site `yt-dlp` supports, and ships with a built-in search, queue, and file library.

It uses **yt-dlp** for stream/download resolution, the **VLC SDK** (libVLC) for playback, and **ffmpeg** for stream processing - all bundled, with the tools kept current by an in-app auto-updater.


## Features

- **Unified player** - local files and online streams play through the same libVLC-backed player, with overlay controls, a quality selector, poster/replay, and fullscreen.
- **Streaming & downloading** - paste a link to stream or download from any `yt-dlp`-supported site (YouTube, Twitch, and many others). Adaptive video+audio are merged transparently.
- **Live stream support** - live sources play with a seekable DVR window and a `● LIVE` indicator instead of a fixed timeline.
- **Search tab** - search YouTube and browse results as cards (thumbnail, title, channel/length/views) with one-click **Play**, **Queue**, or **Download**.
- **Queue tab** - paste-and-preview a URL, stream it, download it, or build a play/download queue.
- **Library tab** - a folder tree + file browser with a sortable file table (name/size/type/date), plus a details panel showing cover art and media metadata.
- **Theming** - Seagull, Dark, and Light themes applied across the whole UI, overlays included.
- **Reorderable tabs** and a download indicator on the Library tab while downloads run.

## Screenshots

**Library**

<p>
  <img src="Seagull/Previews/Library.png" width="49%" />
  <img src="Seagull/Previews/Library1.png" width="49%" />
</p>

**Queue**

<p>
  <img src="Seagull/Previews/Queue.png" width="49%" />
  <img src="Seagull/Previews/Queue1.png" width="49%" />
</p>

**Search**

<p>
  <img src="Seagull/Previews/Search.png" width="70%" />
</p>


## Usage

- **Play or download a link** - open the **Queue** tab, paste a video URL, then use the preview to **Stream** it in the player or **Download** it. Paste more links to build a play/download queue.
- **Search** - open the **Search** tab, type a query in the lower bar, and press **Go** or Enter. Each result card has **Play**, **Queue**, and **Download**.
- **Browse your files** - the **Library** tab is a file browser; double-click a media file to play it, and click a column header to sort.
- **Player controls** - hover the video for the controls bar; pick a quality from the gear menu, toggle fullscreen, or use the title bar's info/share buttons.
- **Settings** - choose your theme, default download/stream quality and format, and your home/download folders. The **Info** page shows this readme, the disclaimer, and the license in-app.

## Built with

- **C++20** / **Qt 6.11** (Widgets, Multimedia, Svg)
- **libVLC** (VLC SDK) for playback
- **yt-dlp**, **ffmpeg**, **deno** as bundled external tools
- **CMake** + **MSVC 2022**

## Build

**Requirements:** Qt 6.11.1 (MSVC 2022 64-bit), MSVC 2022, CMake 3.20+

```powershell
# From the repo root
cmake -S Seagull -B Seagull/out/build/x64-Debug -G "Visual Studio 17 2022" -A x64
cmake --build Seagull/out/build/x64-Debug --config Debug
```

Or open the repo in Visual Studio and use its built-in CMake integration (`CMakeSettings.json` is preconfigured for `x64-Debug`).

The build copies the Qt runtime (`windeployqt`), the VLC DLLs + plugins, and the `Tools/` folder into the output directory automatically.

**Paths:**
- Qt: `C:/Qt/6.11.1/msvc2022_64`
- VLC SDK: `Seagull/sdk/` (headers, libs, plugins)
- Build output: `Seagull/out/build/x64-Debug/`

## Architecture

Seagull is layered so each piece has it's own jobs:

```
Seagull (orchestrator)
 ├─ MainWindow      window shell: chrome, tabs logic, video/tabs splitter
 ├─ VideoPlayer     the playback feature widget (overlays, OSD, quality)
 │   └─ PlaybackEngine   wraps libVLC (neutral transport API, no vlcpp leaks)
 ├─ Tabs            Library · Queue · Search · Settings
 └─ Backend workers SgYtDlp (download/resolve) · SgSearch (discovery) · SgUpdater (tool updates)
```

- `Seagull` creates everything and wires the signals/slots between the UI and backend workers.
- `MainWindow` is a pure shell - no playback logic.
- `VideoPlayer` owns the render surface and overlays; `PlaybackEngine` is the only code that touches VLC.
- Backend classes are `QObject`s wrapping the `yt-dlp` process and libVLC; several `SgYtDlp` instances run in parallel so long jobs never block each other.



## External tools

Bundled in `Seagull/Tools/` (copied to the build output):

- `yt-dlp.exe` - stream resolution and downloads
- `ffmpeg.exe` / `ffprobe.exe` - stream processing and media metadata
- `deno.exe` - used by some yt-dlp extractors

On startup Seagull checks for newer versions of these and updates them in place (with SHA-256 verification).

## License

Licensed under the **GNU GPL v3** - see `LICENSE.txt`.

Seagull is provided as-is - how you use it is your own responsibility. See `DISCLAIMER.md`.

Seagull is built with Qt, libVLC, yt-dlp, FFmpeg, and Deno, each under its own license - see `THIRD_PARTY_NOTICES.md`.
