# Frequently Asked Questions

## General

**What is Seagull?**
A Windows media player and downloader. It plays your local files and online video through one player, and can stream or download from any site `yt-dlp` supports.

**What sites can it play or download from?**
Anything `yt-dlp` supports - hundreds of video and live-streaming sites. If a link works in `yt-dlp`, it generally works in Seagull. Site support comes from `yt-dlp`, which Seagull downloads and keeps updated.

**Does Seagull phone home or track me?**
No analytics or accounts. It makes network requests only for what you ask it to do (resolving/streaming/downloading the links you give it, search queries you type) and to fetch or update its external tools at startup. That tool check can be turned off (see below).

## Installing & launching

**It won't start - Windows says a DLL is missing (e.g. `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll`, `MSVCP140.dll`).**
Seagull is built with the Microsoft Visual C++ runtime, which isn't present on every Windows install. Run the included **`vc_redist.x64.exe`** (it sits next to `Seagull.exe`), then launch Seagull again. It's a one-time, Microsoft-signed install; a reboot may be requested.

**Nothing happens when I double-click it.**
Usually the missing-runtime case above (a missing runtime can fail silently) - run `vc_redist.x64.exe` first. Otherwise, make sure the whole folder is intact: `Seagull.exe` needs the Qt and VLC DLLs and the `plugins/` folder beside it, so run it in place rather than moving the exe out on its own.

**"Windows protected your PC" / SmartScreen blocks it.**
Seagull isn't code-signed yet, so SmartScreen warns on first launch. Click **More info → Run anyway**. You can cross-check what you downloaded against the checksums on the releases page.

**My antivirus flagged Seagull or something in `Tools/`.**
This is a common false positive for media downloaders: `yt-dlp.exe` and `ffmpeg.exe` are legitimate, widely used tools that some scanners flag by heuristics. They're downloaded from their official sources and verified by SHA-256. If your AV quarantines them, restore them and add an exclusion for Seagull's folder, or the tool downloads (and updates) will keep failing.

## Playback

**The video won't play / shows a black screen.**
Most playback runs through libVLC. Try a different quality from the gear menu, and make sure the VLC DLLs and the `plugins/` folder are next to the executable (the build copies them automatically). For online links, a stale CDN URL is re-resolved automatically once; if it still fails you'll get a "stream failed - press replay" message.

**Playback of some videos stutters (AV1).**
AV1 is decoded in software (dav1d) on purpose - VLC 3's AV1 hardware path is unreliable. Very high-resolution AV1 on a slow machine can still struggle; pick a lower quality or a non-AV1 format from the gear menu.

**How do I change quality?**
Hover the video, open the gear (quality) menu in the controls bar, and pick a stream. Your default stream/download quality is set in **Settings → Download & Streaming**.

**Is there an equalizer?**
Yes - **Settings → Audio** has a 10-band graphic equalizer that applies in real time, and a floating EQ button appears by the tab bar while anything plays to jump straight to it. It's set separately for Audio and Video: switch between them with the pill at the top (it follows whatever's playing until you pick one yourself), choose a stock preset or build and save your own, and use the power button to bypass or enable the EQ for that type. Your bands, presets, and on/off choices are remembered per type. The EQ runs in Seagull's own audio pipeline with a limiter behind it, so boosted bands play louder without ever clipping or distorting.

**What is the Normalization button next to the EQ power button?**
Per-type loudness control: it gently lifts quiet material toward a comfortable level and hard-limits peaks just under full scale, so quiet tracks are audible and loud ones never clip. It's independent of the EQ (it protects even with the EQ off), is remembered separately for Audio and Video, and is on by default. Your volume control is applied after it, so it always wins.

**Does Seagull remember where I stopped watching?**
Yes - partly watched videos and local files resume from their last position when you play them again, and each site's home page can show a **Continue Watching** view of what you haven't finished. Items watched to the end drop off automatically. Turn it off with **Settings → General → Remember playback position**; the per-site Continue Watching rows have their own toggles under **Settings → Search**.

**What is the pop-out player?**
The button beside fullscreen detaches the video into its own window so you can keep watching while you use the rest of Seagull (or other apps). Playback continues across the move. **Closing the pop-out window stops playback** and returns the player to the main window.

## Search, Shorts & queue

**How does Shorts mode work?**
The Videos/Shorts pill on the Search tab switches the source. In Shorts, results come from a short-form vertical feed; a playing short loops at the end, and scrolling the mouse wheel over the video moves to the next/previous short, fetching more as you go.

**What is the home feed?**
When you have favourited channels, the Search tab can open on a personalized feed of their newest videos instead of a blank page. In **Settings → Search** you can drag favourites into priority order, set how many of them feed it and how many videos each one contributes, choose whether the feed mixes by recency or keeps your order, and optionally let scrolling pull deeper into each channel. It works per site - each supported site keeps its own favourites and its own home feed.

**How do I favourite a channel?**
Click the star on a result card (or a channel page). Favourites drive the home feed and are saved per site next to your config, so they persist between sessions.

**Can I see a video's comments?**
Yes - when an online video has comments, a **Comments** tab appears next to the player, with collapsible reply threads you can expand and fold like on the source site. The first batch loads quietly in the background so it's usually ready the moment you open the tab, and scrolling to the bottom loads more.

**How do I track my downloads?**
Downloads started from search result cards line up in the **Downloads** tab: live progress with speed and ETA (a slim progress bar also runs along the tab's header), plus restart, cancel, remove, and open-folder actions per item. The list persists between sessions, and restarting an item fetches it fresh from its source page. Downloads started from the Queue tab show their progress right there in the Queue instead.

**Can I clear my search history?**
Yes - **Settings → Search** has "Clear History Now" and an option to auto-clear on exit. History is stored next to `config.ini`.

**Why can't I mix local files and online links in the queue?**
A queue holds one kind at a time (all local or all online) so playback behaves predictably. Adding the other kind prompts you to clear the queue first.

**Can I shuffle playback?**
Yes - while media plays, an autoplay toggle appears beside the tab bar, and with autoplay on a shuffle toggle appears next to it. Turn it on to play through the current list in random order; auto-advance and skip then follow the shuffled order. Both are remembered separately for each kind of content.

**What are playlists?**
You can save the current queue as a playlist (`.sgpl` file) and replay it later from the Library tab's Playlists view.

## Recording & clipping

**How do I record a live stream?**
While a live stream is playing, press the Record button in the controls bar. Recording runs in parallel with playback and finalizes when you stop it or the stream ends.

**How do I clip part of a VOD or local file?**
Press Record once to mark the start (the button pulses), then again to mark the end. The selected range is saved in the background and flashes in the Library when done.

**Where do recordings and downloads go?**
Set your folders in **Settings → Folders & Recording**. Downloads use a dedicated downloads folder; recordings/clips use the recording folder. You can also unify the media folders (Videos/Audio/Images) into one.

**Can downloads sort themselves by type?**
Yes - **Smart sort** (Settings → Download & Streaming) saves each download into its media type's folder automatically: audio into Audio, video into Video, and so on. Turn it off to send everything to a single Downloads folder.

**Do audio downloads include album art?**
Yes. When you download audio, Seagull embeds the video's thumbnail as cover art and writes basic tags (title, artist), so the file shows up properly in music players. Formats that can't hold a cover image (raw AAC, WAV) just skip it. The artwork is embedded with AtomicParsley, one of the helper tools Seagull fetches and keeps updated for you.

## Tabs & layout

**How do the tabs work?**
Drag to reorder, click the small × to close (the floating `+` reopens closed tabs), or drag a tab off the bar to float it in its own window - drag it back over the strip to dock it. At least one tab always stays open.

**Where did the tabs go / how do I get the video bigger?**
Click the divider between the video and the tabs, or the chevron at the bottom of the video, to collapse the tabs and let the video fill. Click again to bring them back. The split position is remembered.

**The search/library results look too small or too large.**
Change **Settings → Display → Card size** (Small … Extra Large, or Custom). Cards grow to fill the row from your chosen size.

**Can I make the seek bar bigger?**
Yes - **Settings → Display → Progress bar size** (Small, Medium, Large) sets the width of the player's seek bar. Larger gives finer scrubbing.

## Tools & updates

**Why is it updating something at startup?**
Seagull relies on `yt-dlp`, `ffmpeg`/`ffprobe`, `deno`, and `AtomicParsley` (which lets yt-dlp embed cover art into MP3/M4A audio), and checks for newer versions on launch (these tools change often, especially yt-dlp). Updates are downloaded and verified by SHA-256 before replacing the old files.

**Can I disable auto-update?**
Yes - turn off **General → Auto-update** in Settings. With it off, the startup dialog asks before checking, and asks again before installing anything.

**ffmpeg takes a while to update.**
ffmpeg is by far the largest tool (~100 MB) and comes from a slower host than the others, so its update can take noticeably longer. This only happens when an update is actually available.

**What is the Defender exclusion option?**
**Settings → General** has a button to add Seagull's folder to Windows Defender's exclusion list (and remove it again). Defender re-scanning the tool exes on every launch can slow a cold start; excluding the folder avoids that. The button reads **Add** or **Remove** depending on the current state, and changing it needs an elevation prompt. It's optional - Seagull works fine either way.

**First run asks me to set up folders / download tools.**
That's the one-time setup dialog. It confirms your media folders and downloads any tool that's missing. It reappears only if a tool exe goes missing.

**Does Seagull need internet on first run?**
Yes. The external tools are not bundled; Seagull downloads `yt-dlp`, `ffmpeg`, `deno`, and `AtomicParsley` during first-run setup. After that, local playback works offline; online features naturally still need a connection.

## Reporting a problem

**Something's misbehaving - how do I capture a log?**
Type `SEALOG` into the Queue tab's link bar and press Enter; a confirmation tells you verbose logging is active. From then on Seagull writes everything it does (tool commands and output, player and worker activity, warnings) to `Config/seagull-log.txt` next to the app. The setting survives restarts, so a startup problem is captured from the very first moment of the next launch. Reproduce the issue, then send that file along with your report. Type `SEALOG` again to turn it off; logging is off by default and the log never leaves your machine unless you share it.

## Building

**How do I build it?**
See the Build section of `README.md`. In short: Qt 6.11.1 (MSVC 2022 64-bit) + CMake 3.20+, then configure/build, or open the repo in Visual Studio (CMake integration is preconfigured).

**Why does a black console window appear?**
That's the Debug build's console. The **Release** configuration builds as a GUI app with no console window.
