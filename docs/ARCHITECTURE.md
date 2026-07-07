# Seagull — Architecture Reference

The engineering reference for the Seagull media player: the startup sequence, the threading
model, the playback pipeline, per-module behaviour, the yt-dlp fleet, and everything the app
persists. Read the chapter for the area you're touching before working in it.

Contents:
1. [Overview and layers](#1-overview-and-layers)
2. [Startup sequence](#2-startup-sequence)
3. [Threading model](#3-threading-model)
4. [The playback pipeline](#4-the-playback-pipeline)
5. [UI module reference](#5-ui-module-reference)
6. [Backend module reference](#6-backend-module-reference)
7. [The yt-dlp fleet and anti-bot strategy](#7-the-yt-dlp-fleet-and-anti-bot-strategy)
8. [Persistence inventory](#8-persistence-inventory)
9. [Key signals](#9-key-signals)
10. [External tools and updates](#10-external-tools-and-updates)
11. [Build](#11-build)

---

## 1. Overview and layers

`Seagull` (in `Seagull.cpp/.h`) is the top-level orchestrator — it owns the `MainWindow`, the
`VideoPlayer`, the tab modules, and the backend workers, and wires up all the signals/slots
between them. Modules never talk to each other directly; everything routes through the
orchestrator's connections.

The player is layered: **`Seagull` orchestrates → `MainWindow` is a window shell →
`VideoPlayer` is the playback feature widget → `PlaybackEngine` wraps VLC.** `MainWindow`
holds no playback/VLC logic; the orchestrator creates the `VideoPlayer` and hands it to the
shell via `mainWindow->setVideoPlayer(player)`. `PlaybackEngine` is the only code that
includes a VLC header.

Canonical tabs, in registration order: **Library · File Explorer · Downloads · Queue ·
Search · Settings**. The equalizer is not a tab — it's the Settings sidebar's **Audio** page
(`Settings::addAudioPage(eqModule)`), reached from Settings or the floating EQ button.
Search and File Explorer are duplicable (multi-instance); Description and Comments are
dynamic tabs that come and go with the playing video.

Ownership rules:
- The orchestrator owns every backend worker and passes bare pointers into module
  constructors (e.g. `Queue(downloaderWorker, resolverWorker, prefetcherWorker)`).
- Module classes inherit `QWidget`; backend classes inherit `QObject`.
- Workers that move to a thread are constructed with no parent (`new SgYtDlp(nullptr)`).
- Singletons (`SgWatchHistory`, `SgDownloadHistory`, `SgFavorites` ×3, `SgLog`) are accessed
  via `instance()` and load their store lazily on first touch.

## 2. Startup sequence

Everything happens inside `Seagull::run()`, in this order. The main window stays hidden
until step 6 — every earlier dialog runs windowless. This is safe because the player no
longer queues a deferred `winId()`/VLC hookup at construction (the old
`QTimer::singleShot(0)` was a landmine: it could fire inside a modal's nested event loop,
realize the native windows under the modal block, and leave the app input-dead).

1. **Favourites preload.** `SgFavorites::instance()` and `phInstance()` are constructed
   eagerly so their JSON loads on the main thread before any `VideoCard` exists; the other
   stores (Chaturbate, SoundCloud, Twitch) construct lazily, in practice touched by the
   Search/Settings constructors while wiring signals, well before any card exists.
2. **Terms of Use** (first run only, `Setup/TermsAccepted` unset): a modal QDialog renders
   the bundled `DISCLAIMER.md`. Decline — or closing, or Escape — returns `false` from
   `run()` and `main()` exits.
3. **Update gate.** The two-stage `UpdateDialog` runs only when ALL of these hold:
   `General/AutoUpdate` on, not within the hourly cooldown (`Updates/LastChecked`,
   3600 s), not the launch right after a self-update (`Updates/JustSelfUpdated`, one-shot),
   not the launch right after first-run setup (`Updates/SkipNextStartupCheck`, one-shot),
   and not a first run (`SetupDialog::isNeeded()`). `LastChecked` is stamped even if the
   user declines the ask stage, so a quick relaunch doesn't re-nag. The dialog always opens
   on a "Check for updates?" ask — nothing touches the network before a Yes. Stage 1 checks
   Seagull itself (`SgAppUpdate`); accepting an app update shows the window, starts the
   self-update (download → stage → helper swap → relaunch), and skips the tool stage — the
   fresh launch handles tools. Stage 2 checks the four external tools (`SgUpdater`).
4. **First-run setup** (when `Setup/Completed` is unset or a tool exe is missing):
   `SetupDialog` confirms the folders and downloads missing tools — it IS the tool stage on
   first run, which is why the update modal is suppressed there. On completion it arms
   `Updates/SkipNextStartupCheck` so the next launch stays quiet too, and the Library
   rescans (it had listed the pre-setup default folders).
5. **Window reveal.** `mainWindow->show()`, then `videoPlayer->rebindOutputWindow()` binds
   VLC's output to the render frame's now-real HWND, and `SgMediaControls::attachToWindow`
   binds SMTC to the window HWND (SMTC is per-HWND for desktop apps).
6. **One-time Defender offer** (only for users updating from builds that predate the
   first-run checkbox; `Setup/DefenderExclusionOffered` gates it forever after). The add
   verifies what actually persisted — if Tamper Protection blocked it, the user is told how
   to finish by hand instead of thinking it worked.
7. **Tab restore.** Duplicate tabs reopen from `Tabs/ExtraTabs`, then
   `applyStoredTabOrder()` (from `Tabs/Order`) and `restoreActiveTab()`.
8. **`finishStartupUpdates()`** releases the thumbnail ffmpeg holds (held since
   construction so a tool swap can't race a running grab) and shuts the updater thread down
   — the startup flow is the updater's whole job; later manual checks recreate it via
   `ensureUpdater()`.
9. **`searchModule->warmHomeFeed()`** starts filling the primary Search tab's home feed in
   the background, now that yt-dlp.exe is stable.

## 3. Threading model

| Thread | Lives there | Notes |
|--------|-------------|-------|
| GUI thread | Everything not listed below: all widgets, the orchestrator, six of the seven `SgYtDlp` workers (they're async via `QProcess`, so they don't block), `SgSearch`, `SgRecorder`, `SgHlsProxy`, `SgMetaCache`, `SgMediaControls`, the singletons | |
| `updaterThread` | `SgUpdater` | Slow blocking work (fetches, hashing, unzip). Shut down right after startup (`shutdownUpdater`); recreated on demand |
| `commentsThread` | `commentsWorker` (`SgYtDlp`) | The comment fetch + JSON parse never touch the GUI thread; results marshal back queued |
| `SeagullAudioOut` (`QThread::HighPriority`) | `AudioSinkWorker` + its `QAudioSink` | The sink pulls PCM on this thread, so audio survives GUI freezes (e.g. the Windows modal move/resize loop). All DSP (`SgEq`, `SgDynamics`) and the visualizer analysis run in the pull |
| VLC's internal threads | decode, demux, and the amem audio callback | `onAudioData` pushes into the `AudioFifo` from VLC's audio thread; VLC events are marshalled to the engine's thread via queued `QMetaObject::invokeMethod` |
| Child processes | 7 × yt-dlp, ffmpeg (thumbnailer, recorder, clipper) | One process per worker at a time, by design |

Rules: `Qt::QueuedConnection` for every cross-thread signal connection. Calls INTO a
threaded worker are functor `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` so they
execute (and their objects get created) with the right thread affinity. Lock-free atomics
for the DSP enable flags; a short mutex for EQ coefficient swaps; the `AudioFifo` is
mutex-guarded.

## 4. The playback pipeline

### VLC instance and load paths

One `VLC::Instance` for the app's lifetime, created with: mouse/keyboard events off,
`network-caching=300`, `file-caching=5000`, `http-reconnect`, and a Chrome desktop
user-agent (the same UA yt-dlp signs CDN URLs with, so the CDN doesn't throttle us).
The `VLC::MediaPlayer` is rebuilt only by the audio-tap OFF path (see below).

Per-media options, added in `loadLocalFile` / `loadStream`:
- `:codec=dav1d,any` — always. VLC 3's AV1-over-D3D11VA frame pool is broken (constant
  stutter, pause/unpause hitches); dav1d claims ONLY AV1, every other codec falls through
  to `any` and keeps hardware decoding. Never use `avcodec-hw=none`.
- `:start-time=<s>` — resume (watch history) and the clip-reload path.
- Local files: `:file-caching=1000` overrides the instance's 5000 — an oversized local
  cache lets the decoder front-run the output clock on a restart and pads silence at the
  head of a replay.
- Streams: `:network-caching=300`, `:http-reconnect=true`, `:adaptive-logic=highest`, and
  `:http-referrer=<page URL>` when the resolver supplied one (hotlink-protected CDNs
  reject requests without it; signed googlevideo URLs ignore it).

**Matched A/V merging** (sites that serve separate video and audio):
- DASH / fragmented-mp4 pairs (YouTube): `:demux=avformat` + `:input-slave=<audio URL>`.
- HLS pairs (e.g. Chaturbate): avformat mangles HLS segment URLs into UNC paths on
  Windows, and VLC won't pull audio from an input-slave `.m3u8` — so the engine
  synthesises a tiny local **master playlist** (`writeHlsMaster`, in `%TEMP%`) tying the
  video chunklist to the audio chunklist via an `EXT-X-MEDIA` group, and VLC's native
  adaptive demuxer combines them. The site's own master is single-use (yt-dlp burns its
  token during extraction), which is why we build our own.

**Replay:** local files rebuild a fresh `VLC::Media` (re-arming a played-out one leaves
VLC's restart clock thinking playback advanced, padding audible silence at the head);
streams re-arm `m_lastMedia`. `releaseMedia()` is the full teardown — after it, a stray
`play()` (space bar, stale controls) is a no-op because `play()` requires loaded media.

### The audio tap

ALL audio-bearing media (audio *and* video; photos excepted) routes through the tap. The
untapped/VLC-native path below survives only as the fallback when the output device can't
be opened, and for stills.

```
VLC decode (its audio thread)
  └─ amem callback (S16N, 44.1 kHz stereo) ──push──▶ AudioFifo (mutex ring, ~4 s cap)
                                                          │ pop (sink's pull thread)
                                                          ▼
                        FifoDevice::readData  — S16 → float scratch (we own the buffer)
                                                          │
                                                          │──▶ analyzeForVisualizer
                                                          │    (raw S16 at the pull instant,
                                                          │     BEFORE the EQ/limiter flatten it)
                                              SgEq   (10-band biquad EQ, may overshoot 0 dBFS)
                                                          │
                                              SgDynamics (loudness normaliser → look-ahead
                                                          brickwall limiter, ceiling −1 dBFS)
                                                          │
                                              float → S16 (clamped quantise, can't clip)
                                                          │
                              QAudioSink (pull mode, ~100 ms buffer) ──▶ device
```

Load-bearing details, each one earned the hard way:
- **Never pull float from VLC's amem** — confirmed dead end on this setup (it screams).
  All float exists only in scratch buffers we generate from VLC's clean S16.
- **Why our own EQ:** libVLC's EQ runs inside VLC and hard-clips its own boost at VLC's
  S16 output, upstream of anything we could limit, and libVLC won't run a filter after its
  EQ (it overrides `audio-filter`). Doing EQ → limiter → quantise ourselves means boosts
  can never clip.
- **FIFO cap ~4 s** of stereo S16 (~1.4 MB). It must comfortably exceed VLC's decode-ahead
  burst at track start; the old ~1 s cap overflowed and evicted unplayed samples — an
  audible skip right as a track began. The sink plays at hardware rate, so headroom adds
  no latency.
- **Prime gate:** until VLC has buffered a ~150 ms cushion, the device hands the sink
  silence without consuming, so the first real samples play from a filled FIFO instead of
  being spliced with zero-padding (that splice is the start-of-track click). Self-re-arms
  whenever the FIFO runs fully dry, and the dry case also resets `SgEq`/`SgDynamics` so
  filter ringdown and gain rides never bleed across tracks.
- **Denormal flush (FTZ+DAZ)** is armed per pull: the IIR feedback decays toward zero and
  denormal arithmetic stalls the CPU ~100×, which starves the pull thread and crackles.
- **Pause** suspends the sink without draining the FIFO — sound stops the instant VLC
  pauses (instead of playing out VLC's ~1 s decode-ahead) and resume picks up the buffered
  samples seamlessly.
- **Volume/mute apply at the sink** while tapped (VLC's fader otherwise) — always AFTER
  the EQ/limiter chain, so the user's volume wins.
- **A/V sync:** VLC thinks amem consumes instantly, so a tapped video would lead the
  buffered audio. `setTapAudioDelayMs` (negative = advance audio) cancels it; VLC clears
  audio delay per media change, so the engine re-asserts it from `onPlaying`. Currently
  tuned to 0 (`kVideoTapAvSyncMs` in `VideoPlayer.cpp`).
- **Tap OFF rebuilds the player** — libVLC can't cleanly detach a player from amem
  callbacks, so `createPlayer()` runs again and the caller rebinds the video HWND.
- The visualizer analysis (RMS level, 3-band spectrum via cascaded one-pole splits, beat
  onsets via a decaying energy average, each normalised to its own recent peak then
  upward-expanded so dense material still moves on screen) runs on the pull thread against
  the RAW pre-DSP S16 at the moment it's pulled — same timing as what's heard, but ahead of
  the normaliser/limiter, whose whole job is to flatten exactly the dynamics the visuals
  need — and marshals its emits to the GUI thread, so a UI freeze pauses the visuals but
  never the audio.

### Equalizer and normalization state

`EQ` (the Settings Audio page) edits and persists per-kind state under `Eq/`; the player
applies it on play (`applyEqualizerForCurrentKind` / `applyNormalizationForCurrentKind`,
which read config directly so a track gets its kind's curve even if the EQ page was never
opened); the orchestrator forwards live edits only when the playing kind matches the edited
type (`mediaKindChanged` keeps the page's pill following playback).

In the engine, `setEqualizer`/`disableEqualizer` fan out by path: tapped → push
gains/preamp/enable to the `AudioSinkWorker` (queued; `SgEq` is unity at flat, no makeup)
and keep libVLC's EQ OFF (it would clip upstream of our limiter); untapped → libVLC's EQ
with the `kEqMakeupDb` (+10 dB) preamp folded in to cancel its fixed insertion loss. EQ
state is re-applied after every `setMedia` and after a player rebuild, so it survives
replays, quality switches, and tap toggles.

Normalization: with everything tapped, both kinds now toggle the sink's `SgDynamics` live
(atomic enable). The legacy untapped-video path — libVLC's compressor filter configured as
a brickwall-ish limiter, bound as media options at load — remains for the fallback case
only (`addVideoNormalizationOptions`, gated `!m_tapOn`). Default is ON for both kinds
(`Eq/<kind>/NormEnabled`).

### Media kinds, resume, shorts

`MediaKind` (Video / Audio / Photo) drives the surface and chrome: audio keeps a cover-art
poster (or the visualizer overlay) up full-time and swaps fullscreen for a visualizer
button; photo skips VLC entirely (still image + fading side arrows + optional slideshow
timer). Kind is derived per file/stream and announced via `mediaKindChanged`.

Resume: `saveWatchProgress` snapshots position into `SgWatchHistory` on every transition
away from the media (pause, stop, EOF, close, new media); EOF records "completed". When
media starts, a saved position is staged in `m_pendingResumeMs` and consumed into the
load's `:start-time`. Keyed by page URL (streams) or file path (local); photos, live
streams, and AUDIO never record or resume (songs always start at the top and never appear
in Continue Watching — the resume-staging call sites are also gated on kind, which covers
positions saved before audio was excluded). Gated by `Playback/RememberPosition` (default on).

Shorts mode (`setShortsMode`): the short loops at EOF when autoplay is off (feed style) or
auto-advances seamlessly when autoplay is on; wheel deltas over the video accumulate to one
notch and emit `shortsScrolled(±1)`, rate-limited so a flick advances exactly one short.

### Failure paths

A stream that fails to open re-resolves itself ONCE (`streamUrlRequested` with
`freshResolve=true`, bypassing the meta cache — the usual cause is an expired CDN URL),
bounded by a retry timer; a second failure pins "stream failed — replay" in the ended
state. Live-stream recording auto-stops on stop/close/new-media/end.

## 5. UI module reference

### MainWindow — the shell

Owns the chrome, the vertical `QSplitter` (video over tabs), and the tab widget. Only
window-level concerns live here: fullscreen, pop-out hosting, and forwarding move/resize to
the player's overlay repositioning.

- **Tabs.** Pages are wrapped in `QScrollArea`s. Closable (min. one stays open); closing
  removes the page from the tab widget but the module widget stays alive and working.
  The floating "+" after the last tab reopens closed tabs at their canonical spot and
  offers "New Search / File Explorer tab" (duplicables). Tabs tear off by dragging off the
  bar into a `FloatingTab` window; dragging that window over the strip (or closing it)
  docks it back — a floating tab is never lost. Dynamic tabs (Description, Comments) sit at
  the end, are never persisted, never in the "+" menu. Persisted: `Tabs/Closed`,
  `Tabs/Order`, `Tabs/ExtraTabs` (duplicates), `Tabs/ActiveLabel`+`ActiveOrdinal`.
- **Tab-header indicators.** `setTabBusy` shows the animated seagull spinner in place of a
  tab's close ×; `setTabProgress` draws a thin determinate bar along the bottom of a tab's
  header (the Downloads tab while a download runs).
- **Corner cluster**, floating right of the "+": per-context **autoplay** and **shuffle**
  toggles plus a slideshow-interval spin in photo mode (`setPlaybackContext` namespaces
  them per content type, e.g. `search.youtube` vs `library.video`); a floating **EQ**
  button while audio/video plays (`eqRequested` → orchestrator calls `showTab(settings)` +
  `Settings::showAudioPage`); a floating **Share** button while an online video plays.
- **Tabs-pane toggle.** Two triggers, same `toggleTabsCollapsed()`: a clean click on the
  splitter handle (press+release with no move past the drag threshold — real drags are
  untouched; the filter never swallows events), and the circular chevron overlay near the
  bottom of the video (owned by `VideoPlayer`, `tabsToggleRequested`). The shell pushes
  pane state back (`syncTabsPaneState` → `setTabsPaneOpen`) so the chevron points the way
  the pane will move. Works in fullscreen (handle width goes 0 only in fullscreen with the
  pane down); starting shorts viewing force-drops the pane. Split ratio persists
  (`Display/VideoSplitRatio`).
- **Media keys.** An app-wide event filter routes transport keys (space, arrows,
  comma/period, M, F, Escape) player-first whenever media is active and the app is focused,
  bowing out for text-entry widgets, modals, and popups (`handleMediaKey`). A wheel over an
  unfocused combo/spin inside a scroll area is redirected to the scroll area, so scrolling
  a settings page can't accidentally change a value.
- **Pop-out.** `togglePlayerPopout` reparents the whole player into a `PlayerWindow`;
  closing it re-docks (playback never stops from the move itself).

### VideoPlayer — the playback feature

Owns the `PlaybackEngine`, the render frame, and all the overlays. The VLC HWND paints over
child widgets, so every overlay (controls pill, title banner, poster, chevron, photo
arrows, visualizer) is a **top-level tool window** glued to the video rect —
`repositionOverlays`/`reownOverlays` keep them attached across moves, fullscreen, and
pop-out, and exposure probes (`overlaySurfaceExposedAt`) stop them stacking above dialogs
or other apps' windows.

- Poster overlay: stream thumbnail or local frame-grab/cover art (via the player's own
  `SgThumbnailer`), shown while fetching, when paused, and at EOF; click-through so
  play/pause still works. Replay from the ended state reloads cleanly (see engine).
- Quality: the probe fills the gear menu (`handleAvailableQualities`); switching requests a
  fresh stream URL for the format id and reloads at the current position
  (`savedStreamTimestamp`).
- Recording button dispatches by source: live → parallel ffmpeg capture; VOD/local → first
  press marks the clip start (button pulses), second press saves [start, end] in the
  background. Banner notices confirm saves; the Library tab flashes when the file lands.
- Announces `commentsAvailable(pageUrl, count)` once per URL from the probe's
  `comment_count`; `videoInfoChanged` drives the dynamic Description tab and Share button.
- Pop-out support: `rebindOutputWindow` re-attaches VLC to the recreated HWND (Qt recreates
  it when a widget changes top-level windows); `hardStop` is the pop-out close teardown.

### MediaLibrary — the Library tab

Card grid of the saved-media folders, one type at a time: the floating pill
(Videos/Audio/Images/Recordings/Playlists) literally selects which `SgPaths` folder is
listed (not a filter). Pill auto-hides while scrolled; grid builds incrementally on an idle
timer (`buildBusyChanged` lets the visualizer suspend its 60 fps render timer during the
burst). Top-right chips: magnifier (per-type title search), sort (name/date, persisted
`Library/SortMode`), and the trash toggle arming **delete mode** — cards become
multi-select toggles, selections keyed by absolute path so they survive type switches, and
confirm sends everything picked to the Recycle Bin. Playback sessions snapshot the ordered
list at play time, so auto-advance/shuffle walk what the user pressed play on even while
they browse another type. Playlist cards (`.sgpl`) route to the Queue.

### FileExplorer — the file manager tab

Folder tree + sortable file table + details panel + clipboard ops. Files drag onto tree
folders to move; context menu adds to the Queue's local queue. Address bar and rename use
the shared spell checker.

### Downloads — the DownloadManager tab

Persistent download list backed by `SgDownloadHistory`. Owns the ad-hoc download FIFO
(moved out of the orchestrator) and drives `downloadWorker` strictly one-at-a-time. Records
are keyed by a unique per-download id, NOT the page URL — the same link downloaded as video
and again as audio is two separate rows. `enqueue` captures the Settings download shape
(`Download/Type` + `Download/Format`) onto the record; the only dedup rule is that an
identical url+type+format is ignored while one is already Queued/Downloading. Rows
(`Widgets/DownloadRow`: thumbnail, title, then an info line with status, file type — real
extension once the file exists, else the queued format — yt-dlp's own speed/ETA while
downloading, and the queued/finished date-time pinned right) rebuild on structural changes;
live progress goes straight into the active row. Restart re-queues the SAME record (id
kept, timestamps/path reset). Loading a pre-id-keying history migrates in place: ids are
synthesized, and kind/fmt are derived from the saved file's extension so old rows show a
type too (an old record with no file recorded stays typeless — nothing to derive from).
Row titles are `Widgets/MarqueeLabel`s — the shared elide-at-rest / marquee-on-hover title
label also used by the video cards (two-line elide collapsing to a one-line marquee on
card hover) and the player's title pill. Long titles never overflow or widen a layout:
they end in an ellipsis until hovered, then scroll to the end, hold, snap back, repeat.
Restart re-runs yt-dlp against the stored **page URL** (never a CDN URL — those expire),
cancel marks Canceled, finished rows offer open-folder. `activity(busy, percent)` drives
the tab-header progress bar. ALL downloads route here — Search-card Download buttons and
every Queue-tab download action ("Download now", "Download Queue", the context menu's
Download) emit a `downloadRequested(pageUrl, title, thumbUrl)` that the orchestrator wires
to `enqueue`. The Queue no longer downloads on `downloaderWorker` at all (that worker keeps
metadata/stream-URL/playlist fetches), so it has no inline progress bar or per-row progress
column; "Download Queue" hands the whole table over and clears it.

### Queue — preview, stream, download, queue

Paste-a-URL preview (debounced metadata fetch → hero thumbnail + stats), then Stream /
Download / Add to Queue. All download actions hand off to the Downloads tab (see above);
the queue table shows Title + Status only. The queue holds ONE locality at a time (local or online, never
mixed — crossing the line pops a clear-first modal); a local queue relabels "Stream Queue"
to "Play Queue" and hides "Download Queue". Playlist URLs offer queueing all entries;
titles resolve in the background one row at a time; the CDN prefetcher resolves the next
queued item ahead of play, validated by YouTube's `?expire=` token where present, with
failed prefetches remembered so a bot-block doesn't retry-storm. "Create Playlist" writes
the queue as `.sgpl` JSON (`{version,name,type,created,entries[{target,title}]}`) to
`SgPaths::playlistFolder()`; `loadPlaylistFile` replaces (confirmed) and optionally
auto-plays. Typing `SEALOG` in the URL bar toggles verbose logging (modal confirms
Active/Deactivated).

### Search — discovery, channels, home feed

Browser-style chrome: back / forward / refresh / Home, a site dropdown
(YouTube / PornHub / Chaturbate / SoundCloud / Twitch — typed shorthands like "sc"/"tw"
work), and an editable query combo (arrow drops the per-site history; typing filters via a
`MatchContains` completer; Enter's returnPressed+textActivated twin-fire is collapsed by an
end-stamped timestamp, robust across the duplicate-site modal's nested event loop).

Two site shapes: **video sites** (YouTube, PornHub, SoundCloud — results are playable
items, channels/artists have listing pages) and **live sites** (Chaturbate, Twitch —
results ARE the live channels, `SearchResult::isLive` puts a red LIVE badge + viewer count
on the card, clicking a channel name gets a clean "no video list" message instead of a
doomed yt-dlp run). SoundCloud plays as audio kind, so the EQ/limiter, visualizer,
artwork embedding, and resume all apply to it.

- **Results feed.** Cards dedupe by URL, page in batches (cap 240), sortable
  (Relevance = arrival order / name / date), title-filterable via the magnifier chip.
  Playing a result makes the results the active feed: skip buttons walk it
  (`playAdjacentResult`), advancing past the loaded tail auto-fetches and resumes.
- **Videos/Shorts pill** (search/channel views only) is a source switch, not a filter:
  Shorts re-runs the query through YouTube's internal search API (yt-dlp can't parse shorts
  renderers). A playing short gets feed behaviour (loop/advance + wheel), and the
  orchestrator prefetches the next two shorts' CDNs (see §7).
- **Channels.** `channel:` prefix searches channels (internal API; yt-dlp is unreliable
  for channel results); channel cards and clickable uploader names open the channel's
  videos page in the same grid with a header banner (avatar decoded via ffmpeg when it's
  WebP), navigated with back/forward (typed `NavEntry{Query|Channel}` history).
- **Favourites.** A star on cards/channel pages writes to the site's `SgFavorites` store;
  the star chip toggles a favourites-only view.
- **Home feed.** With favourites present, the landing is a personalized feed of their
  newest uploads: built sequentially (one light request per channel, once per session,
  rate-limit sensitive), cached for instant back/Home, rebuilt on site switch. Settings
  control per site: ranked drag-order picker, channel count, videos per channel, randomize
  (recency mix vs favourites order), and lazy load (scroll pulls deeper per channel, capped
  at 60/channel, warned on enable). YouTube, PornHub, and SoundCloud get the full set;
  the live sites keep fixed list order (randomize would break live-first). Chaturbate's
  feed is the favourited rooms themselves; Twitch's is the favourited channels enriched by
  ONE batched GQL live-status request (`users(logins:)`), live channels first with stream
  titles, degrading to plain favourite cards if the lookup fails. The primary tab warms
  its feed at startup (`warmHomeFeed`).
- **Continue Watching pill** on home landings when `SgWatchHistory` has resumable items
  for the site (per-site toggle), rendering history entries as cards.
- **Duplicate-site guard.** Before a user search, if a sibling Search tab targets the same
  site, warn once per tab/session (permanent opt-out `Search/WarnDuplicateSite`) — parallel
  tabs hammering one site look like bot traffic.

### Settings

Sidebar pages: General / Display / **Audio** (the hosted EQ widget) / Download & Streaming /
Folders & Recording / Search / Info. Notables: auto-update, remember-playback-position,
Defender exclusion (elevated add/remove with verified state), desktop + Start-menu shortcut
toggles (General); appearance filter (Light/Dark) + theme, card size, seek-bar width,
visualizer picker/behaviour/flock-cap (Display); type/format/quality for downloads and
recording, cookies browser with enable-warning and cookie-data wipe, smart sort (Download &
Streaming); the folder rows with the unify-media switch (Folders); result limit, history
clearing, and the per-site home-feed section (Search). Saves are suppressed while loading;
theme/card-width/seek-width only re-apply when actually changed (a theme apply re-polishes
every widget).

### EQ — the Audio page

Video/Audio pill (auto-follows the playing kind until a manual click pins it; re-arms each
time the page shows), preset dropdown (stock per type + custom + "Add custom preset…"),
10 band sliders + preamp with a live dB callout, EQ power button, and the Normalization
power button. Edits persist debounced under `Eq/`; live edits emit only for the matching
playing kind. Bands map 1:1 onto `SgEq` (same ISO centres as libVLC, so old presets carry).

### SetupDialog / UpdateDialog

Covered in §2 and §10. Both run before the window exists; UpdateDialog has no close button
and swallows Escape while busy so a tool is never replaced while something might spawn it.

## 6. Backend module reference

- `SgYtDlp` — see §7.
- `SgOptions` — builds yt-dlp's argument list from config: output path (smart-sort routes
  per media type), format/quality, browser cookies. Audio downloads append
  `--embed-metadata`, plus `--embed-thumbnail --convert-thumbnails jpg` (ffmpeg embeds for
  MP3/Opus/FLAC, AtomicParsley for M4A/MP4; jpg because YouTube thumbnails are WebP) —
  except for WAV/AAC, which have no cover-art container: yt-dlp's embed step would
  hard-fail the whole download (non-zero exit despite a completed file, orphaned jpg
  beside it), so those skip the thumbnail args entirely.
- `SgFormat` — `StreamOption` + the format-selection policy shared by resolve/probe.
- `SgSearch` — discovery worker (`Site` enum: YouTube / PornHub / Chaturbate / SoundCloud /
  Twitch). yt-dlp for normal search (`ytsearch:` / `scsearch:` — SoundCloud rides the same
  flat `-J` path as YouTube) and channel/artist listings (`/videos`, `/tracks`); YouTube's
  internal `youtubei/v1/search` API over QNAM for shorts search, channel search
  (`params:"EgIQAg=="`), and home-feed channel shorts (search-by-name; yt-dlp can't list a
  `/shorts` tab), paging via continuation tokens accumulated per query. Twitch goes through
  the public GQL endpoint (`gql.twitch.tv` with the anonymous web Client-Id, the same
  approach streamlink uses): `searchFor` for channel search, one batched `users(logins:)`
  for the home feed's live statuses; GQL `errors[]` are logged so a schema drift shows up
  in the SEALOG. PornHub/Chaturbate parse their HTML listings. `fetchChannelVideos` guards
  the live sites (no video list to browse).
- `SgFavorites` — five contained stores: `instance()` → `Config/favorites.json` (YouTube,
  avatars via yt-dlp), `phInstance()` → `ph_favorites.json`, `cbInstance()` →
  `cb_favorites.json`, `scInstance()` → `sc_favorites.json` (avatars via yt-dlp, like
  YouTube), `twInstance()` → `tw_favorites.json`. `forUrl()` routes a channel URL to its
  store. Keyed by channelUrl; loads once, writes back immediately.
- `SgWatchHistory` — resume + Continue Watching store (`Config/watch_history.json`).
  Near-complete ⇒ completed (no resume, hidden from Continue Watching); `resumePosition`
  returns −1 unless past the minimum threshold and outside the end guard; per-site queries
  feed the home views; capped, most-recent-first.
- `SgDownloadHistory` — the Downloads tab's store (`Config/download_history.json`): status
  lifecycle (Queued/Downloading/Completed/Failed/Canceled), resolved output path,
  `clearFinished` keeps pending work. Keyed by page URL.
- `SgPaths` — save-folder resolver (`Paths/*`). `DownloadFolder` (default: the user's
  Downloads) is deliberately outside the unify system. The four media folders fall back
  explicit key → legacy `DownloadFolder` → the user's Windows folders. `UnifyMedia` +
  `UnifiedFolder` collapse them (accessors take `honourUnify=false` for raw values).
  `PlaylistFolder` (default `Documents/Playlists`) skips the legacy fallback. Also owns
  `configDir()`/`configFile()` — the app-local `Config/` folder everything persists into.
- `SgThumbnailer` — one-ffmpeg-at-a-time FIFO: video frame grabs + audio cover art, cached
  in `cache/thumbs/`; `decodeViaFfmpeg` handles WebP (no `qwebp` plugin in this build);
  `setHeld` parks the queue during startup tool updates.
- `PlaybackEngine` / `SgEq` / `SgDynamics` — see §4.
- `SgRecorder` — live capture: parallel `ffmpeg -c copy` against the exact resolved (and
  Twitch ad-stripped) URLs feeding VLC, with the page URL as Referer; VOD/local clips cut
  with ffmpeg directly against the already-resolved stream (no re-resolve), falling back
  to a fresh yt-dlp full download + trim only when no resolved stream is on hand. Output
  lands in the recording folder; format per `Recording/*`.
- `SgHlsProxy` — localhost proxy that rewrites Twitch's live HLS manifest, dropping
  server-side-stitched ad segments before VLC sees them (a UA spoof alone did NOT work).
  Shared by the five resolve-capable workers; the player and recorder both consume the
  proxied URL.
- `SgMetaCache` — the shared `-J` cache; see §7.
- `SgMediaControls` — SMTC via C++/WinRT: now-playing card (title/artist/artwork), media
  keys and overlay buttons routed back to the player, timeline pushed by a timer. Update()
  while Closed blanks the card — state order matters. Also hosts the Defender-exclusion
  and shortcut (.lnk) helpers Settings calls.
- `SgAppUpdate` — GitHub Releases check (beta channel) + self-update download/stage;
  a helper swaps files after exit (robocopy preserving `Config/` and `Tools/`) and
  relaunches with `Updates/JustSelfUpdated` armed.
- `SgUpdater` — see §10.
- `SgSpellCheck` — Windows OS spell checker over COM, shared app-wide, consumed through
  `Widgets/SpellCheckLineEdit` (Search query, File Explorer address/rename, Library search).
- `SgLog` — the SEALOG verbose sink; see §8. `SgYtDlp::logLine` mirrors every command and
  output line into it; a Qt message handler captures qDebug/qWarning/qCritical; every write
  is mutex-guarded and flushed so a crash leaves a complete log.

## 7. The yt-dlp fleet and anti-bot strategy

Seven `SgYtDlp` instances, each running ONE `QProcess` job at a time, so long jobs never
contend:

| Worker | Job |
|--------|-----|
| `downloaderWorker` | Queue-tab metadata previews / playlist-entry fetches |
| `resolverWorker` | Stream URL / queue title resolution |
| `prefetcherWorker` | Next queue item's CDN prefetch |
| `playerWorker` | The player's probe + stream-url traffic |
| `downloadWorker` | ALL downloads (Search cards + Queue tab), pumped one-at-a-time by the Downloads tab |
| `commentsWorker` | Paginated comments fetch — on its own thread |
| `shortsPrefetcher` | The next TWO shorts' CDN resolves, ahead of the scroll |

Anti-bot/throttle strategy, spread across the fleet (these look arbitrary individually —
they're one policy):
- **Shared `-J` meta cache** (`SgMetaCache`, injected into all but the comments worker):
  per-URL, 30-minute TTL, live streams never cached. The resolver, prefetcher, and player
  asking about the same video is answered locally — a duplicate extraction burst is a
  known bot trigger. `freshResolve=true` bypasses it for the stale-CDN retry.
- **Strictly sequential fetching** everywhere: one download at a time, home-feed channels
  one at a time, shorts prefetches one at a time — parallel yt-dlp launches are a bot tell.
- **Jitter:** each shorts-prefetch launch sits behind a small randomized debounce so
  background requests don't march at a fixed metronome. Forward-only (back-scroll resolves
  on demand); a watchdog abandons a resolve that reports nothing (a no-format result emits
  neither `streamUrlReady` nor `finished` and would otherwise latch the pipeline).
- **Cookies are opt-in** (`Streaming/CookiesBrowser`, warned on enable, wipeable);
  `impersonate-chrome` for non-YouTube sites; the engine presents the same Chrome UA the
  URLs were signed with.
- **Failure memory:** the Queue skips URLs a prefetch failed on rather than retry-storming.
- **One debounced modal:** every worker's bot-check / HTTP-429 detection funnels into
  `extractionBlocked(kind, detail)` → `onExtractionBlocked`, which shows a single warning
  and stays quiet through a cooldown (several workers can trip at once and retries recur).
- **Duplicate-site tab warning** in Search (see §5).

Comments have no offset in yt-dlp, so "load more" re-requests a larger window and the
orchestrator de-dupes by comment id; replies arrive inline (each carries a `parent` id) and
are grouped client-side, so a thread toggle is a re-render, not a fetch.

## 8. Persistence inventory

Everything lives in the app-local **`Config/`** folder (`SgPaths::configDir()`), so the
install is self-contained and survives the self-update's robocopy swap.

**Files:**

| File | Owner | Contents |
|------|-------|----------|
| `Config/config.ini` | everyone via `QSettings` | All settings (sections below) |
| `Config/favorites.json` | `SgFavorites::instance()` | YouTube favourites |
| `Config/ph_favorites.json` | `phInstance()` | PornHub favourites |
| `Config/cb_favorites.json` | `cbInstance()` | Chaturbate favourites |
| `Config/sc_favorites.json` | `scInstance()` | SoundCloud favourites |
| `Config/tw_favorites.json` | `twInstance()` | Twitch favourites |
| `Config/watch_history.json` | `SgWatchHistory` | Resume positions / Continue Watching |
| `Config/download_history.json` | `SgDownloadHistory` | Downloads-tab records |
| `Config/seagull-log.txt` | `SgLog` | SEALOG verbose log (append, session-bannered) |
| per-site search history `.txt` | `Search` | One query per line, next to config.ini |
| `cache/thumbs/` | `SgThumbnailer` | Disk thumbnail cache |
| `<PlaylistFolder>/*.sgpl` | `Queue` | Saved queue playlists (JSON) |
| `%TEMP%/seagull_hls_*.m3u8` | `PlaybackEngine` | Synthesized HLS masters (removed on teardown) |

**config.ini sections** (representative keys; dynamic families noted):

| Section | Keys |
|---------|------|
| `Setup/` | `TermsAccepted`, `Completed`, `DefenderExcluded`, `DefenderExclusionOffered` |
| `General/` | `AutoUpdate` |
| `Updates/` | `LastChecked`, `JustSelfUpdated` (one-shot), `SkipNextStartupCheck` (one-shot) |
| `Display/` | `Theme`, `CardWidth`, `SeekBarSize`, `VideoSplitRatio` |
| `Audio/` | `Volume`, `Muted` |
| `Eq/` | per-type `Audio/`+`Video/` subgroups: `Enabled`, `Gains`, `Preamp`, `NormEnabled`; custom preset arrays `AudioCustom`/`VideoCustom` |
| `Playback/` | `RememberPosition` |
| `Player/` | `Autoplay/<contextKey>`, `Shuffle/<contextKey>`, `SlideshowInterval` (per-content-type toggles) |
| `Download/` | `Type`, `Format`, `Quality` |
| `Streaming/` | `Quality`, `CookiesBrowser`, `RecordFormat` |
| `Recording/` | `Type`, `Format` |
| `Paths/` | `HomeFolder`, `DownloadFolder`, `VideoFolder`, `AudioFolder`, `PhotoFolder`, `RecordingFolder`, `PlaylistFolder`, `UnifyMedia`, `UnifiedFolder`, `SmartSort` |
| `Library/` | `SortMode` |
| `Search/` | `ResultLimit`, `SortMode`, `ClearHistoryOnExit`, `WarnDuplicateSite`, `CookiesWarningAck`; per-site families `HomeChannels<Site>`, `HomeAmount<Site>`, `HomeVideosPerChannel<Site>`, `HomeRandomize<Site>`, `HomeLazyLoad<Site>`, `ShowContinueWatching<Site>` |
| `Tabs/` | `Closed`, `Order`, `ExtraTabs`, `ActiveLabel`, `ActiveOrdinal` |
| `FileExplorer/` | `AddressHistory` |
| `Visualizer/` | `Type`, `Active`, `Behavior`, `MaxGulls`, `KillOnEnd`, `LighthouseBeats` (beats per lighthouse flash, Night type only) |
| `Logging/` | `Verbose` (the SEALOG persist) |

## 9. Key signals

| Signal | From | To | Purpose |
|--------|------|----|---------|
| `playMediaRequested` | MediaLibrary / FileExplorer | VideoPlayer | Play a local file |
| `playMediaRequested` | Queue / Search | VideoPlayer | Play online video (page + optional CDN URLs) |
| `mediaEnded` | VideoPlayer | Seagull | Auto-advance (queue outranks the grids) |
| `skipRequested` | VideoPlayer | Seagull (`skipActive`) | Skip forward/back, shared with SMTC next/prev |
| `probeQualitiesRequested` / `streamUrlRequested` | VideoPlayer | playerWorker | Quality probe / format resolve (`freshResolve` bypasses the cache) |
| `fullscreenToggleRequested` / `popOutRequested` | VideoPlayer | MainWindow | Window-level actions |
| `downloadRequested` | Search / Queue | DownloadManager | Queue a download (page URL + title + thumb) |
| `activity` | DownloadManager | MainWindow `setTabProgress` | Downloads tab-header progress |
| `downloadProgress` / `downloadDestination` | SgYtDlp | DownloadManager | Per-row live progress / final file path |
| `mediaKindChanged` | VideoPlayer | Seagull → EQ | Audio page pill follows playback |
| `eqChanged` / `eqEnabledChanged` / `normalizationChanged` | EQ | Seagull → VideoPlayer | Live edits, gated to the matching playing kind |
| `eqRequested` | MainWindow | Seagull | Floating EQ button → Settings Audio page |
| `shortsScrolled` | VideoPlayer | Seagull → Search | Wheel over a short advances the feed |
| `commentsAvailable` | VideoPlayer | Seagull | Open the dynamic Comments tab + preload |
| `extractionBlocked` | any SgYtDlp | Seagull | Debounced bot/throttle warning modal |
| `smtcStateChanged` / `smtcMetadata` / `smtcArtwork` | VideoPlayer | SgMediaControls | Mirror playback to the OS controls |

## 10. External tools and updates

`Seagull/Tools/` (copied to the build output): `yt-dlp.exe` (resolve/download),
`ffmpeg.exe`/`ffprobe.exe` (streams, thumbnails, recording, clipping), `deno.exe` (some
yt-dlp extractors), `AtomicParsley.exe` (M4A/MP4 cover-art embedding).

`SgUpdater` (own thread, startup-only — see §2/§3) checks all four inside the modal
`UpdateDialog`. yt-dlp, Deno, and ffmpeg track their latest upstream releases with SHA-256
verification; AtomicParsley publishes no per-asset hash and ships rarely, so it's pinned to
a known build (version + SHA-256 in `SgUpdater.cpp`) and verified against that pin. ffmpeg
is by far the largest (~100 MB) and comes from a slower host. Nothing checks or installs
without an explicit user Yes.

Self-update (`SgAppUpdate`): download → stage → helper swaps the app directory after exit
(robocopy, preserving `Config/` and `Tools/`) → relaunch with the startup check suppressed
once.

## 11. Build

**Requirements:** Qt 6.11.1 (MSVC 2022 64-bit), MSVC 2022, CMake 3.20+

```powershell
# Configure and build (from repo root)
cd C:\Users\Ryan\source\repos\Seagull
cmake -S Seagull -B Seagull/out/build/x64-Debug -G "Visual Studio 17 2022" -A x64
cmake --build Seagull/out/build/x64-Debug --config Debug
```

Or open the repo in Visual Studio and use the built-in CMake integration
(CMakeSettings.json is configured for x64-Debug and x64-Release). The Release config links
`/SUBSYSTEM:WINDOWS` (`/ENTRY:mainCRTStartup`) so there's no console window; Debug keeps
the console. The exe icon comes from `app.rc` (`Assets/Icon.ico`). Versioning:
`project(Seagull VERSION x.y.z)` in `CMakeLists.txt` is the single source of truth, plus
`SEAGULL_VERSION_SUFFIX` (currently `-beta`).

**Qt path:** `C:/Qt/6.11.1/msvc2022_64`
**VLC SDK:** `Seagull/sdk/` (headers in `include/vlc/`, libs in `lib/`, plugins in `plugins/`)
**Build output:** `Seagull/out/build/x64-Debug/`

Post-build steps (run automatically via CMake): `windeployqt` copies the Qt runtime; the
VLC DLLs (`libvlc.dll`, `libvlccore.dll`) and plugins folder are copied; `Tools/` is copied.
