# Read Path Optimization Design

Status: implemented in the working tree on 2026-11-10, not committed. The
Electron TypeScript typechecks and the changed C++ compiles syntax-only. The
library changes ran in an out-of-tree test build on Linux. No app has been
built or run with these changes; [What to test](#what-to-test) lists what that
leaves open.

## Purpose

Each frontend reads telemetry from libtnrp and keeps some of it. A read of
that path in all four frontends found the same samples held more than once,
and copies and conversions made on the way that nothing needed. This work
removes the ones that could be removed without changing what a chart shows.

Nothing here reduces chart data. Every sample that was drawn before is still
held and drawn.

| Change | Where | Effect |
|---|---|---|
| [A](#a-recorded-chunks-leave-memory) | libtnrp | While recording, committed chunks live in the file, not also in memory |
| [B](#b-whole-laps-are-not-fetched-twice) | Electron renderer | A live lap received whole is not fetched again from the engine |
| [Lap caches](#analysis-lap-cache) | Electron renderer | The Analysis lap cache is bounded and shares laps the store holds |
| [Driver cache](#driver-history-cache) | Electron renderer | The driver history cache has a byte limit |
| [Transit copies](#transit-copies) | Electron main and renderer | Three copies of binary payloads removed |
| [Qt rows](#qt) | Qt | Row type read once; seek flush JSON not copied |
| [Android attach](#android) | Android | Engine threads attach to the VM once |
| [Minimal](#minimal-recorder) | Minimal | Subscribes to the one row family it reads |
| [Retention flag](#retention-flag) | libtnrp | `Config::retainLiveSession`, on by default |

## What was found

The live session is the largest duplicate. On the desktop hosts the engine
keeps it for every car (`TnrdWriter::setRetainSession`), and the frontend keeps
the player's part again, decoded: Electron in the renderer's column tables,
Qt in `SessionData::laps`. Both copies serve a purpose, so neither was simply
deleted. Change A shrinks the engine's side. Change B removes a third copy
Electron made of two laps.

The rest were smaller: lap caches that overlapped, buffers copied to cross a
boundary that did not need a copy, and rows scanned or parsed more often than
once.

Two findings from the first read were wrong or overstated, and are recorded
here so they are not chased again:

- **Android paired rows parsed twice.** Not so. The desktop sends each row as
  a JSON string inside the frame, so `PairedTelemetryClient.handleText` never
  takes its `toString()` branch.
- **Android copies a column on every read.** Overstated. The renderer reads a
  `stableSlice`, one copy per redraw.

## A. Recorded chunks leave memory

File: `protocol_parser_library/src/tnrd/TNRD_V6.cpp`. The storage model this
changes is in [LIVE_TELEMETRY_MEMORY_DESIGN.md](LIVE_TELEMETRY_MEMORY_DESIGN.md),
which is updated to match.

### Before

A retained session kept every committed chunk's compressed frame in
`memoryPayloads`, indexed by `V6ChunkInfo::offset`. When a recording file was
attached, the same frame was also appended to the file. The session grew about
1 MB a minute in memory for the length of the race, recording or not.

### After

A frame that an attached file holds is not kept in memory as well.

- `memoryBacking` runs parallel to `memoryPayloads`. An entry names the file
  (`AttachedFile::id`) and the frame's offset in it. The matching
  `memoryPayloads` slot is null.
- `payloadOf(chunk)` returns a frame from memory, or reads it from the file
  that holds it. Every reader of committed frames goes through it:
  `decodeCommitted`, `image` and `writeFileBacklog`.
- A session that is not recording keeps every frame in memory, as before.

| Event | What happens |
|---|---|
| Chunk committed while a file is attached | Written to each file that takes the driver. The first one that also flushes becomes the frame's holder. If none does, memory keeps it |
| File attached part-way (`attachFile`) | The backlog is written and flushed, then `adoptPlaced` releases those frames to the new file |
| File detached (`detachFile`, recording off, session end) | `reclaimFrom` reads the file's frames back into memory first. The session carries on in memory at its old size |
| File fails (`fileFailed`) | The same reclaim, through a fresh read handle if its own is closed. A frame that cannot be read is lost to the session |
| Session finished (`finish`) | No reclaim (`reclaimOnDetach` off): nothing reads the session afterwards |
| Memory image requested (`image`) | Frames a file holds are read for that image alone and freed with it |
| Deep flashback (`uncommit`, `rewriteFile`) | Reopened laps are decoded from the file. The file is rewritten beside the original, reading kept frames from the original through `rewriteSource`. Frames take their new offsets once the new file is in place |

With two files attached (all drivers, and driver only), the first that takes a
driver's chunk holds it. A driver-only file alone releases the player's frames
and leaves the other cars' in memory.

### Costs and risks

- **Live reads touch the disk.** A chart backfill, a lap request or a host
  restore asks for an image of the player's chart types, and `image` reads
  those frames from the file each time. Before, it shared pointers. The file
  was just written, so the OS cache should serve it, but this is unmeasured.
- **Track-map matching** asks for every car's position, aero and lap-timing
  chunks, at most every 30 seconds while a map is unmatched. It is the
  heaviest read.
- **A failed file can lose live history.** If a recording file fails and its
  frames cannot be read back, those laps are gone from the live session.
  Before, memory always had them.
- **One flush per chunk.** A frame is only released once its file has flushed
  it, so commits are no longer left in the stdio buffer.
- **No switch.** There is no setting to keep the old behaviour.

### Flashbacks

The rewrite path runs only when a flashback lands at or before the commit
point, which trails live by the 30-second write delay (`WRITE_DELAY`). The
game's flashback reaches about 15 seconds and cannot be chained, so in real
use it stays inside the laps still being built and never rewrites. The path is
kept correct and is covered by the library tests, but it is not a risk to
plan testing around.

## B. Whole laps are not fetched twice

File: `electron-frontend/src/renderer/src/stores/telemetryStore.ts`.

### Before

On every lap change the renderer asked the engine for the Previous lap, and
for Fastest when it changed (`requestStoredLiveLaps`). The answer was decoded
into its own storage (`decodeStoredLap`). The tables meanwhile kept the same
lap's rows as a rewind cushion (`trimLiveWorkingSet` keeps the current lap and
two completed ones). The fetch exists because a lap received while the window
was hidden has holes.

### After

A lap the window received whole keeps a snapshot cut from the tables. The
snapshot is made of views, so it shares the tables' chunks, and no fetch is
made.

A lap is whole for a family when:

- its first row was seen: the lap number went up by one and the lap rows on
  either side of the line are under one second apart
  (`WHOLE_LAP_MAX_ROW_GAP_S`);
- the family was in `historyRowMask` from then until the lap ended
  (`liveLapWholeMask` is and-ed with every new mask);
- nothing dropped rows on the way. A hide, a rewind, a host restore and a
  session reset each call `invalidateWholeLiveLap`.

A snapshot is used in place of the engine's copy only when it covers every
family the charts want now, and lap rows are among them
(`coversWantedFamilies`). Anything else is fetched as before. That includes
switching to a page that needs a family the snapshot lacks.

The snapshot is cut one second after the lap line
(`WHOLE_LAP_SETTLE_MS`, `scheduleWholeLapSnapshot`). Hot rows travel on
another IPC channel, so the last rows of a lap can arrive just after the lap
row that ends it. Until then Previous shows the stand-in it always showed.
`wholeLapData` keeps the row before the lap for status and damage, as the
published lap slices do.

The rewind cushion is unchanged, so behaviour at lap lines and across time
windows is as before.

### Costs and risks

- Previous updates once about a second into the new lap, where before it
  updated when the engine's copy arrived.
- On a page whose charts do not want lap rows, a lap is never taken as whole
  and is fetched as before. Sector splits are still learned from the stand-in
  at the lap line.
- The whole-lap rules are deliberately conservative. A case they miss shows
  up as a Previous or Fastest lap with missing samples.

## Analysis lap cache

File: `electron-frontend/src/renderer/src/components/AnalyzeScreen.tsx`.

`analysisLapCache` held every comparison lap fetched until the file changed.
The store's `playbackLapDataCache` separately held up to six laps of the
playback driver, read by the same engine call.

- `withAnalysisLap` limits `analysisLapCache` to six laps
  (`MAX_ANALYSIS_LAP_CACHE`). The laps on screen are never dropped, and a
  dropped lap can be requested again.
- A comparison lap for the playback driver from the primary file is taken
  from the store's cache when it is there with the needed row types. The
  object is shared, not fetched or copied.

## Driver history cache

File: `telemetryStore.ts`. `allLapsDriverCache` keeps full All Laps history
for recently viewed playback drivers so switching back is immediate. It was
limited to three drivers. It is now also limited to 64 MB of column chunks
(`MAX_CACHED_DRIVER_BYTES`); the newest driver always stays. With long races
this may keep one or two drivers where it kept three. The figure is a first
guess.

## Transit copies

| Path | Before | After |
|---|---|---|
| Hot rows, `forwardBinary` in `bridgeManager.ts` | `Buffer.from(batch)` on every batch, read only by the logging branch | The copy is made inside the logging branch |
| Seek-forward queue, `bufferSeekBinary` | Each batch copied before queueing | Queued as received; each batch is already its own Buffer from the addon |
| History payload to the decode worker, `historyDecodeClient.ts` | `bytes.slice()`, then transfer the copy | A payload that owns its whole buffer is transferred as it is. The worker returns it with a failure so `decodeV6History` can fall back in-thread |

The worker change alters one failure case. If the worker itself dies during a
decode, the payload is gone and that decode fails. Before, the caller still
had its copy and decoded in-thread.

## Qt

- **Row type read once.** `MainWindow::onEngineRow` searched each row several
  times, once per kind of row it handles before parsing. `engineRowType` reads the type once and the
  handlers are chosen from it. The per-row copy made when a burst is split
  stays, because the seek replay buffer stores rows.
- **Seek flush JSON not copied.** `EngineSeekFlush::coldJson` is the moved
  `std::string`. `TnrdPlayer::decodeHistory` reads it in place.

## Android

`AndroidSink` in `native_bridge.cpp` attached the calling thread to the VM and
detached it around every row. An engine thread now attaches once and
`ThreadAttachment`, a `thread_local`, detaches it when the thread exits. Rows
are still one JNI call each.

## Minimal recorder

`MinimalController` parsed every row the engine produced to find session
rows. It now calls `setDataRequirements(1u << 5, 0, 0.0f)` after constructing
the engine, so only session rows (row id 5) and control rows reach it.
Recording ignores the mask.

## Retention flag

`Config::retainLiveSession`, on by default, separates keeping the live session
in memory from `binaryPlayback`. With it off a `binaryPlayback` host has no
`LiveV6Store`: no chart backfill or host restore, no `liveGetLapData`, no deep
Strategy rollback and no live track-map matching or generation.

No host sets it. It exists so Android can drop the retained session with one
line once that is decided.

## Not done

| Item | Why |
|---|---|
| Lap data as columns instead of JSON (`playerGetAnalysisLapData`, `playerPhoneLapDataJson`) | Touches every reader generation, the addon, IPC, Qt, JNI and the pair protocol, and needs a decision on positions and lap edges |
| V6 patch merging in the library | Changes what every host receives in playback. Qt's `PlaybackPatchMerger` still parses, merges, re-serialises and parses each patch |
| Chunk columns at their recorded width | Needs the V6 history payload's per-field widths checked first |
| Charts reading stored samples instead of a copy | Qt's series vectors and Electron's chart pages still duplicate what is on screen |
| One lap cache on Android | The graph and Analysis controllers keep laps in different shapes |
| Control rows on their own sink callback | Changes `Sink` for every host for a small gain |
| Hot rows decoded straight into the tables | `handleMsg` does more than store the row |
| Android: whether to keep the live session | A decision, not a change. `androidConfig()` turns it on; [LIVE_TELEMETRY_MEMORY_DESIGN.md](LIVE_TELEMETRY_MEMORY_DESIGN.md) says Android does not |

## What has been checked

| Check | Result |
|---|---|
| `npm run typecheck` (main and renderer) | Pass |
| Syntax-only compile of `TNRD_V6.cpp`, `Engine.cpp`, `MinimalController.cpp`, `MainWindow.cpp`, `TnrdPlayer.cpp` | Pass |
| Syntax-only compile of `native_bridge.cpp` against the desktop JDK header | One error on the existing `AttachCurrentThread` line, where the JDK and NDK signatures differ. Needs an NDK compile to settle |
| libtnrp regression tests, out-of-tree build on Linux | 10 of 12 pass, including `tnrd_v6_recording_scopes`, `tnrd_v6_recovery` and `strategy_rollback` |
| `tnrd_v4_roundtrip`, `tnrd_v5_roundtrip` | Segfault at startup. They segfault the same way on the staged code without these changes. Not investigated |
| Scratch test for change A, four cases | Pass |
| The V6 tests and the scratch test under AddressSanitizer and UndefinedBehaviorSanitizer | Pass, no reports |

The scratch test compared a session with no file against one recording the
same rows:

- **From the start:** committed frames held in memory were 0 bytes while
  recording and 5,145 bytes with no file. Images were identical, twice in a
  row.
- **Driver-only file, then all-drivers file, attached part-way:** held bytes
  went 5,145, then 2,579, then 0. Images matched at each step.
- **Flashback into committed laps, twice, with both files:** the same laps
  were taken back, and images matched after each rewind and after the replay.
- **Detach:** held bytes returned to 5,145 and images matched.

The test is not in the repo. It reuses the row builders from
`tests/TnrdV6RecordingScopes.cpp` and would sit beside it as
`tests/TnrdV6FileBacked.cpp`.

## What to test

Nothing below has been run. The data source for all of it is the São Paulo
capture replayed at 1x, as described in
[QT_MEMORY_INVESTIGATION.md](QT_MEMORY_INVESTIGATION.md#running-the-udp-replay).

### Change A, on Electron and Qt, with recording on

- [ ] **Memory over a long session.** With the memory log on, the writer's
  `memoryChunkBytes` should stay near zero while recording, where it grew
  about 1 MB a minute. Compare process private memory against a run on the
  old build.
- [ ] **Chart backfill.** Switch between chart pages and enter All Laps late
  in a session. Charts should fill as before, with no visible stall.
- [ ] **Previous and Fastest laps** on a lap the window missed (minimize
  across a lap line). The engine's copy should still arrive.
- [ ] **Minimize and restore** for two minutes. Charts should be continuous
  afterwards.
- [ ] **Recording switched on part-way.** The file should start from the
  session's beginning and open in playback.
- [ ] **Recording switched off part-way.** Live history should keep working.
  Expect memory to rise by the session's size at that moment.
- [ ] **Session end with both files** (all drivers and driver only), then the
  keep/discard choice. Both files should open before the choice is made.
- [ ] **Open the active recording in playback** mid-session.
- [ ] **Strategy** after a flashback, and the Strategy page opened late in a
  race.
- [ ] **Modded maps on** with an unmatched track: watch for a hitch every
  30 seconds while it tries to match.
- [ ] **A recording file that fails.** Fill the disk or pull the drive
  mid-session. The session should carry on and report the file error. Check
  what live history survives.
- [ ] **Windows.** Everything above ran only on Linux. The file is read and
  written through one handle.

### Change B, on Electron, live

- [ ] **Previous lap in Current-lap mode.** It should appear at the lap line
  and settle about a second later with no missing samples at either end.
- [ ] **Fastest lap** set on a whole lap, then several laps later, after the
  tables have trimmed past it.
- [ ] **Page switch mid-lap** to a page that charts another family. The lap
  that was in progress should be fetched from the engine, and its Previous
  view should be complete on the new page.
- [ ] **Hide during a lap,** then show. That lap and the one in progress
  should come from the engine.
- [ ] **Flashback.** Previous should be correct after a rewind across a lap
  line.
- [ ] **Out-lap to flying lap in qualifying,** and the first lap after
  connecting mid-lap. Neither should be taken as whole.
- [ ] **Sector splits and the lap delta** against Previous and Fastest.
- [ ] **All Laps mode,** the same checks.
- [ ] **Renderer memory.** Live lap snapshots in the memory log should no
  longer add storage of their own in the common case.

### Electron, playback

- [ ] **Seek and All Laps** on a V6 recording, including a full-race payload.
  This exercises the transferred worker payload.
- [ ] **Driver switch and switch back** in All Laps. The driver cache should
  still make the return immediate for at least the most recent driver.
- [ ] **Analysis.** Compare the current lap against another lap of the same
  driver, another driver, and a second file. Step through more than six
  comparison laps and return to an earlier one.
- [ ] **Scrub repeatedly** while playing, which exercises the seek-forward
  queue.

### Qt

- [ ] **Live and playback smoke test** on every page.
- [ ] **Rows handled before parsing:** a recording error, the recording
  finished prompt, modded map changes, the lap-times dialog and Trends lap
  measures.
- [ ] **Seek on a V4 or V5 recording,** which is the path that reads the cold
  JSON.

### Android

- [ ] **NDK build** of `native_bridge.cpp`.
- [ ] **Live from the game and recording playback,** including starting and
  stopping the source several times. Engine threads now stay attached until
  they exit and rely on `ThreadAttachment` to detach them; if that did not
  run, stopping the source is where it would crash.
- [ ] **Paired mode,** which does not use the sink, as a control.

### Minimal recorder

- [ ] **The session card** still shows circuit and session, and a recording
  still starts and holds every row.

## Files

| Area | Files |
|---|---|
| A | `protocol_parser_library/src/tnrd/TNRD_V6.cpp`, `docs/LIVE_TELEMETRY_MEMORY_DESIGN.md` |
| B, driver cache | `electron-frontend/src/renderer/src/stores/telemetryStore.ts` |
| Analysis lap cache | `electron-frontend/src/renderer/src/components/AnalyzeScreen.tsx` |
| Transit copies | `electron-frontend/src/main/bridgeManager.ts`, `renderer/src/lib/historyDecodeClient.ts`, `lib/historyDecode.worker.ts`, `lib/columnStore.ts` |
| Qt | `qt_frontend/src/MainWindow.cpp`, `EngineSink.h`, `TnrdPlayer.cpp` |
| Android | `android_frontend/app/src/main/cpp/native_bridge.cpp` |
| Minimal | `minimal_frontend/src/MinimalController.cpp` |
| Retention flag | `protocol_parser_library/include/tnrp/Config.h`, `src/Engine.cpp` |
