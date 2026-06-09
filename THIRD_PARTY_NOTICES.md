# Third-Party Notices

Seagull is built with and distributes the following third-party components. Each is the property of its respective authors and is provided under its own license. The relevant license text accompanies each component (or is available at the links below).

Seagull itself is licensed under the GNU GPL v3 (see `LICENSE.txt`).

---

## Qt 6

- **Used for:** application framework and UI (Widgets, Multimedia, Svg).
- **License:** GNU Lesser General Public License v3 (LGPL-3.0).
- **Notes:** Qt is used as dynamically linked libraries (the DLLs deployed alongside the executable), which may be replaced by the user with compatible versions.
- **Project:** https://www.qt.io
- **License text:** https://www.gnu.org/licenses/lgpl-3.0.html

## libVLC (VLC media player SDK)

- **Used for:** media playback.
- **License:** GNU Lesser General Public License v2.1 or later (LGPL-2.1-or-later) for libVLC and its core; some bundled plugins are under the GNU GPL.
- **Notes:** Used as dynamically linked libraries with their plugins; replaceable by the user.
- **Project:** https://www.videolan.org/vlc/
- **License text:** https://www.gnu.org/licenses/lgpl-2.1.html

## yt-dlp

- **Used for:** resolving and downloading online media.
- **License:** The Unlicense (public domain).
- **Notes:** Bundled as a standalone executable and invoked as a separate process.
- **Project:** https://github.com/yt-dlp/yt-dlp
- **License text:** https://github.com/yt-dlp/yt-dlp/blob/master/LICENSE

## FFmpeg

- **Used for:** stream processing and media metadata (ffmpeg / ffprobe).
- **License:** GNU General Public License v3 (GPL-3.0) — the bundled "essentials" build from gyan.dev is a GPL build.
- **Notes:** Bundled as standalone executables and invoked as separate processes.
- **Project:** https://ffmpeg.org · Windows builds: https://www.gyan.dev/ffmpeg/builds/
- **License text:** https://www.gnu.org/licenses/gpl-3.0.html

## Deno

- **Used for:** running scripts required by some yt-dlp extractors.
- **License:** MIT License.
- **Notes:** Bundled as a standalone executable and invoked as a separate process.
- **Project:** https://github.com/denoland/deno
- **License text:** https://github.com/denoland/deno/blob/main/LICENSE.md

---

