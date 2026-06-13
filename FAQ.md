# Frequently Asked Questions

## General

**What is Seagull?**
A Windows media player and downloader. It plays your local files and online video through one player, and can stream or download from any site `yt-dlp` supports.

**What sites can it play or download from?**
Anything `yt-dlp` supports - hundreds of video and live-streaming sites. If a link works in `yt-dlp`, it generally works in Seagull. Site support comes from `yt-dlp`, which Seagull downloads and keeps updated.

**Does Seagull phone home or track me?**
No analytics or accounts. It makes network requests only for what you ask it to do (resolving/streaming/downloading the links you give it, search queries you type) and to fetch or update its external tools at startup. That tool check can be turned off (see below).

## Playback

**The video won't play / shows a black screen.**
Most playback runs through libVLC. Try a different quality from the gear menu, and make sure the VLC DLLs and the `plugins/` folder are next to the executable (the build copies them automatically). For online links, a stale CDN URL is re-resolved automatically once; if it still fails you'll get a "stream failed - press replay" message.

**Playback of some videos stutters (AV1).**
AV1 is decoded in software (dav1d) on purpose - VLC 3's AV1 hardware path is unreliable. Very high-resolution AV1 on a slow machine can still struggle; pick a lower quality or a non-AV1 format from the gear menu.

**How do I change quality?**
Hover the video, open the gear (quality) menu in the controls bar, and pick a stream. Your default stream/download quality is set in **Settings → Download & Streaming**.

**What is the pop-out player?**
The button beside fullscreen detaches the video into its own window so you can keep watching while you use the rest of Seagull (or other apps). Playback continues across the move. **Closing the pop-out window stops playback** and returns the player to the main window.

## Search, Shorts & queue

**How does Shorts mode work?**
The Videos/Shorts pill on the Search tab switches the source. In Shorts, results come from a short-form vertical feed; a playing short loops at the end, and scrolling the mouse wheel over the video moves to the next/previous short, fetching more as you go.

**Can I clear my search history?**
Yes - **Settings → Search** has "Clear History Now" and an option to auto-clear on exit. History is stored next to `config.ini`.

**Why can't I mix local files and online links in the queue?**
A queue holds one kind at a time (all local or all online) so playback behaves predictably. Adding the other kind prompts you to clear the queue first.

**What are playlists?**
You can save the current queue as a playlist (`.sgpl` file) and replay it later from the Library tab's Playlists view.

## Recording & clipping

**How do I record a live stream?**
While a live stream is playing, press the Record button in the controls bar. Recording runs in parallel with playback and finalizes when you stop it or the stream ends.

**How do I clip part of a VOD or local file?**
Press Record once to mark the start (the button pulses), then again to mark the end. The selected range is saved in the background and flashes in the Library when done.

**Where do recordings and downloads go?**
Set your folders in **Settings → Folders & Recording**. Downloads use a dedicated downloads folder; recordings/clips use the recording folder. You can also unify the media folders (Videos/Audio/Images) into one.

## Tabs & layout

**How do the tabs work?**
Drag to reorder, click the small × to close (the floating `+` reopens closed tabs), or drag a tab off the bar to float it in its own window - drag it back over the strip to dock it. At least one tab always stays open.

**Where did the tabs go / how do I get the video bigger?**
Click the divider between the video and the tabs, or the chevron at the bottom of the video, to collapse the tabs and let the video fill. Click again to bring them back. The split position is remembered.

**The search/library results look too small or too large.**
Change **Settings → Display → Card size** (Small … Extra Large, or Custom). Cards grow to fill the row from your chosen size.

## Tools & updates

**Why is it updating something at startup?**
Seagull relies on `yt-dlp`, `ffmpeg`/`ffprobe`, and `deno`, and checks for newer versions on launch (these tools change often, especially yt-dlp). Updates are downloaded and verified by SHA-256 before replacing the old files.

**Can I disable auto-update?**
Yes - turn off **General → Auto-update** in Settings. With it off, the startup dialog asks before checking, and asks again before installing anything.

**ffmpeg takes a while to update.**
ffmpeg is by far the largest tool (~100 MB) and comes from a slower host than the others, so its update can take noticeably longer. This only happens when an update is actually available.

**First run asks me to set up folders / download tools.**
That's the one-time setup dialog. It confirms your media folders and downloads any tool that's missing. It reappears only if a tool exe goes missing.

**Does Seagull need internet on first run?**
Yes. The external tools are not bundled; Seagull downloads `yt-dlp`, `ffmpeg`, and the helper runtime during first-run setup. After that, local playback works offline; online features naturally still need a connection.

## Building

**How do I build it?**
See the Build section of `README.md`. In short: Qt 6.11.1 (MSVC 2022 64-bit) + CMake 3.20+, then configure/build, or open the repo in Visual Studio (CMake integration is preconfigured).

**Why does a black console window appear?**
That's the Debug build's console. The **Release** configuration builds as a GUI app with no console window.
