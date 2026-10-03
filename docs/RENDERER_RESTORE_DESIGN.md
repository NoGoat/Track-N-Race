# Renderer Restore Design

## Purpose

Replace the Electron main-process resume cache with an engine-driven rebuild
when the window becomes visible again.

While the renderer is hidden (minimized, occluded, or in a background tab),
main stops forwarding rows and keeps a bounded "resume cache" instead. That
cache keeps only chart rows from the last chart window, and it drops everything
else. One visible result: a formation lap or Safety Car that ends while the app
is minimized leaves its banner on screen. The ending `SCAR` event is never
delivered, and `useRaceBanners` trusts the last `SCAR` event over
`session.safety_car_status`. The Session page's events list has the same gap.

The resume cache was added before the engine kept live history. Since
`LiveHistoryStore` (commit `4065fa6`), the engine holds the whole live session
in lap/family segments, including race events. A recording already holds the
whole playback session. Main's copy is redundant, it doesn't cover everything,
and it costs memory: up to 600 s of packed rows plus 32 MiB of JSON while hidden.

The new model has one source of truth:

- While hidden, nothing is cached in main. Rows for the renderer are dropped.
- On restore, the engine works out what the renderer missed and sends one
  restore payload, built from its own history (live) or from the recording
  (playback).
- The renderer installs that payload as a replacement of the missed time range.
  It doesn't replay the payload as a live stream.

## Current behaviour (to be removed)

| Layer | Piece | Behaviour |
| --- | --- | --- |
| Renderer | `main.tsx` `visibilitychange` | Calls `playerBridge.setPageVisible()` |
| Main | `setRendererVisible()` | Closes or opens the forwarding gate, clears or sends the resume cache |
| Main | `hiddenBinary`, `hiddenJson`, `hiddenJsonSeeds`, `cacheResumeJson()`, `trimResumeCache()`, `sendResumeCache()`, `clearResumeCache()`, `resumeWindowMs`, `MAX_RESUME_JSON_CHARS` | Bounded chart window, filtered to status, damage, telemetry, motion and motion_ex |
| Main | `forwardBinary()` hidden branch, `releaseSeekForwarding()` hidden branch | Push into the resume cache |
| Main | `binaryRows.ts` `chartHistoryRecords()` | Only used to fill the resume cache |
| Main | Diagnostics `resumeCache` / `hidden_resume` | Report the cache |
| Preload | `telemetryBridge.onResume`, `telemetry-resume` channel | Delivery |
| Renderer | `telemetryBridge.onResume(...)` in `telemetryStore.ts` | Appends the cache through `handleMsg`/`appendPlaybackPatch`, then in playback calls `requestVisibleWindowHistory(true)` and an All Laps gap `getWindowData` |

Problems this design fixes:

1. **Race events are never cached.** In live mode nothing backfills them after
   a restore, so the Session page has a gap and the safety-car banner can stay
   on screen.
2. **Old rows are lost.** The cache only covers the last chart window (15–600 s).
   Hidden time before that window is lost in live mode.
3. **Edge-encoded patches need special handling.** The `hiddenJsonSeeds` logic
   for V6 patches exists only because the cache is a partial copy.
4. **Rows are restored twice in playback.** It applies the cache and then
   re-reads the same window from the file.
5. **Non-V6 playback can keep a gap.** The playback refetch installs as
   `'prefix'`, which cannot fill a gap inside the window. The cache only hid
   this when the hidden time was shorter than the window.
6. **Flashbacks and session changes while hidden.** These replay through
   `handleMsg` in the wrong order relative to the cached rows. It works by
   accident or not at all.

## New model

```text
renderer visibilitychange
   └─ main.setRendererVisible(v)
        ├─ gate closed/opened (unchanged, but no caching)
        └─ engine.setHostVisible(v)
              hide:  remember hiddenSince, reset rewindFloor, remember sessionUid
              show:  compute restore ranges → history worker / reader
                     → onSeekFlush(..., restore = true, ...)
                     → main broadcasts playback_seek_flush_bin
                     → renderer processRestoreFlush() → replaceRange install
```

The engine owns the restore because it is the only layer that sees every
rewind, seek and session change while the renderer is hidden. Neither main nor
the renderer has to guess the hidden time range.

### Engine: tracking the hidden period

New `Engine::setHostVisible(bool visible)`, exposed through the addon as
`setHostVisible`. It defaults to visible, so Qt, the minimal frontend and paired
phones are unaffected.

State, protected by `mutex_`:

| Field | Set on hide | Updated while hidden | Used on show |
| --- | --- | --- | --- |
| `hostHidden_` | `true` | — | Restore is issued only on a hidden→visible edge |
| `hiddenSince_` | `liveSessionTime_` (live) or `currentTime_` (playback) | — | Start of the gap |
| `hiddenRewindFloor_` | `+inf` | `min(floor, target)` in `rewindLiveTimeline()` and when an authoritative playback seek is applied | Start of the gap if time went backwards |
| `hiddenSessionUid_` | `liveLapHistorySessionUid_` (the header `m_sessionUID` of the last live packet) | — | Detects a new session while hidden |

`restoreFrom = min(hiddenSince_, hiddenRewindFloor_)`.

### Session identity (verified against the UDP specs)

Every packet's `PacketHeader` carries `uint64 m_sessionUID` ("Unique identifier
for the session"). The definition is the same in all three supported specs:

- F1 24 v27.2x (`references/f1-24-v27.2x.md`, `PacketHeader`)
- F1 25 v3 (`references/f1-25-v3.md`, `PacketHeader`)
- F1 25 2026 Season Pack, document version 1.2 (`references/f1-25-2026-season-8.md`, `PacketHeader`)

The header is packed, little-endian, and laid out the same way in all three:

```text
uint16 m_packetFormat   @0
uint8  m_gameYear       @2
uint8  m_gameMajor      @3
uint8  m_gameMinor      @4
uint8  m_packetVersion  @5
uint8  m_packetId       @6
uint64 m_sessionUID     @7
float  m_sessionTime    @15
uint32 m_frameIdentifier @23
```

This matches `Parser.cpp`: `ReadUInt64(data, 7)`, `ReadFloat(data, 15)`,
`ReadUInt32(data, 23)`.

Other identity fields the specs offer, and why they aren't used here:

| Field / event | Spec text | Use for restore |
| --- | --- | --- |
| `m_sessionUID` (header, every packet) | "Unique identifier for the session" | **Primary signal.** Every packet carries it, so it can't be missed while hidden |
| `m_sessionLinkIdentifier` (PacketSessionData, also season and weekend variants) | "Identifier for session - persists across saves" | Not used. It's meant to stay the same across save/load of the same session, and it's only on the Session packet |
| `SSTA` / `SEND` events | "Sent when the session starts / ends" | Not used as the detector. These are one-shot events and could be dropped by UDP. They are still restored as ordinary events in the event range |
| `m_overallFrameIdentifier` (header) | "doesn't go back after flashbacks" | Not used as a session signal. It tells a flashback (`m_frameIdentifier` goes back) apart from forward time, but the native rewind path already handles flashbacks |

**Not specified by the spec:** whether restarting a session (the same track
and session type) produces a new `m_sessionUID`. Strategy already assumes it
does: `StrategyProcessor::ingest` says the "header UID identifies a session
exactly (including restarts at the same track and type)". That is a code-level
assumption, not spec text. Confirm it with one capture of a session restart
before relying on it for restarts. Changes of track or session type produce a
new session, and the UID check covers those whichever way restarts behave.

**Implementation.** The engine already updates `liveLapHistorySessionUid_` from
`r.sessionUid` for every live datagram (`Engine.cpp`, before the row loop).

- On hide, copy it into `hiddenSessionUid_`.
- On show, `sessionChanged = (format seen) && liveLapHistorySessionUid_ != hiddenSessionUid_`.
- The UID never reaches the renderer: `session_uid` isn't serialized into the
  session JSON. So the engine has to decide, and the payload carries the result.

When `sessionChanged`:

- Both ranges start at `0`. The restore covers the whole new session, which is
  the only session the renderer should have.
- A new live session currently reaches `LiveHistoryStore` as a rewind to near
  zero (`timelineTime < liveSessionTime_ - 0.2f` → `rewindLiveTimeline`), so
  old-session rows after that point are already gone. Old rows at or before
  the new session's first timestamp could survive.
  - Resetting the live store on a UID change, as the playback load path already
    does with `liveHistory_->reset()`, closes that edge.
  - This belongs with this change, because a restore after a session change
    must not return rows from the previous session.

### Engine: what is requested on show

Two ranges go into **one** payload, so the renderer installs and publishes once:

| Range | Families | From | Through |
| --- | --- | --- | --- |
| Charts | `hostConsumerHistoryMask_` | `max(windowFrom, restoreFrom)` | now |
| Events (live only) | `1 << 6` (race_event) | `restoreFrom` | now |

`windowFrom` uses the same formula as the live backfill in
`setDataRequirements`:

- window `< 0` (All Laps or Stint Laps): `0`
- window `== 0` (current lap): `liveLapStart_`
- otherwise: `now - window`

Taking the `max` with `restoreFrom` keeps a short minimize cheap. All Laps only
decompresses the hidden laps instead of the whole session.

Events ignore the chart window. In current-lap mode, events from earlier laps
of the hidden period still have to come back. This is the case from the bug
report: formation lap, then lap 7 in Current mode.

"Now" is `liveSessionTime_` (live) or `currentTime_` (playback), read under the
mutex at request time. It becomes the payload's `restoreThrough`.

Live:

- Extend `LiveHistoryStore` with a multi-range request, for example
  `requestRanges(std::vector<{uint32_t mask; float from;}>, float through, cb)`.
  It returns one `LiveHistoryBackfill`.
  - Race events are a small JSON family, so the extra range costs very little.
  - The chart range reuses the existing segment and decompression path on the
    history worker.
- Like the existing `setDataRequirements` backfill, drop the callback if a newer
  requirements request or restore has replaced it. Use a restore generation
  counter instead of `latestRequirementsRequestId_` alone, so a requirements
  change during the restore doesn't drop it unless that change is itself a
  backfill.

Playback:

- Use the existing reader path (`prepareV6HistoryReadLocked` /
  `reader_.seekFlush`) for the chart range only.
- Playback events already come from `playbackEvents`, which the renderer holds
  in full from load time and slices by playhead. Don't request them.
- This replaces the renderer's `requestVisibleWindowHistory(true)` and its All
  Laps gap `getWindowData` call in `onResume`.

Also on show, emit current-state rows for the host's stream-only families:
session, timing, participants, tyre sets, strategy and so on. Use the existing
`restore` / `liveLatestRows_` (live) or `latestOfTypesTagged` (playback)
mechanism from `setDataRequirements`, so headers and cards are correct
immediately instead of waiting for the next packet.

### Engine → main: payload

Reuse `onSeekFlush` and `playback_seek_flush_bin`, and add these fields:

| Field | Meaning |
| --- | --- |
| `restore: true` | Install with replace-range semantics. Not a seek; doesn't move the playhead |
| `restoreFrom` | Chart range start (`historyStart` keeps its current meaning for coverage markers) |
| `restoreEventsFrom` | Event range start (live only; absent in playback) |
| `restoreThrough` | End of both ranges |
| `sessionChanged` | The session UID differs from `hiddenSessionUid_` |
| `currentLapStart`, `lapNum` | As today: `liveLapStart_` / `liveLapNum_` or the reader's `currentLapAt` |

`authoritativeSeek` is `false`, so main's supersede check
(`requestId <= latestSeekRequestId`) and the renderer's seek gating continue to
treat it as an additive response.

### Main

- Delete the resume cache and everything that only exists for it (see the table
  above).
- `setRendererVisible(v)`:
  - Keep the gate and the edge detection.
  - Call `engine?.setHostVisible(v)` after updating `rendererVisible`.
  - Keep `broadcast(lastStatusRow)` on show.
- `forwardBinary()` / the JSON callback / `releaseSeekForwarding()`: when
  hidden, drop the rows. No caching.
- The `onSeekFlush` handler stays as it is, minus `clearResumeCache()`.
  `broadcast()` ignores the visibility gate, so a restore that finishes while
  the window is hidden again is still delivered. That's harmless because the
  next show issues a new restore.

### Renderer

Delete `telemetryBridge.onResume(...)`, the preload channel and the type.

In `processPlaybackSeekFlush`, branch early on `payload.restore === true` into a
dedicated `processRestoreFlush`. It shares the decode code (V6 columnar or
packed + cold JSON) but not the seek-specific steps:

1. **Don't clear tables, and don't bump `seekTimelineGeneration`.** Capture the
   current generation and abandon the restore if an authoritative seek starts
   during the decode, because the seek supersedes it.
2. **Session change.** If `sessionChanged`, call `resetSession()` before
   installing, then install the payload as the whole history for every family
   (`from = -Infinity`).
3. **Charts.** For every family in `rowTypeMask`, **including families whose
   incoming table is empty**, call
   `installHistory(table, incoming, 'replaceRange', MAX_ROWS, { from: restoreFrom, through: restoreThrough })`.
   - Processing empty families matters because a rewind while hidden can leave
     no rows in the range. The stale rows still have to be deleted.
4. **Events (live).** Rebuild `raceEventsArr`:
   - held events with `session_time < restoreEventsFrom`
   - then the incoming events
   - then held or streamed events with `session_time > restoreThrough`
   - all through `mergeRaceEventHistory` (removes duplicates, merges RTMT/PENA-16
     retirements, sorts, caps at `MAX_RACE_EVENTS`).
   - **Don't** call `raceEventListeners`. Transient banners are for live
     moments, not catch-up.
   - The persistent safety-car banner then corrects itself, because
     `latestSafetyCarEvent` now sees the terminal `SCAR`.
5. **Lap state.**
   - Set `lapStartTime` / `lapNum` from the payload.
   - In live All Laps with the lap bit present, rebuild
     `allLapsLapBoundaries` with `reconstructLapBoundaries` (the existing
     post-install code already does this).
   - Rebuild `liveLapBoundaries` from the lap table for the current-lap and
     window modes as well, because a lap rollover while hidden left them stale.
6. **Derived state.**
   - Run the existing tail of `processPlaybackSeekFlush`:
     `findCurrentStintStart`, latest status, damage and lap, `recompute`,
     `markHistoryCoverage`.
   - Recompute `fuelMaxReceived` from the installed status range.
   - If the fastest lap may have changed while hidden, call
     `liveGetFastestLap` again in live mode.
7. **No `requestVisibleWindowHistory()` follow-up.** The restore already covers
   the visible window.

### Column store: `replaceRange` install mode

Add `InstallMode` `'replaceRange'` to `installHistory` in `lib/columnStore.ts`.
It needs a `{ from, through }` argument.

```text
result = held[t < from] ++ incoming[from ≤ t ≤ through] ++ held[t > through]
```

- Clamp incoming rows to `[from, through]`, so a segment boundary that returns
  slightly earlier rows can't put rows out of order.
- Keep `held[t > through]`. These are rows streamed live after the gate opened,
  which arrive before the restore payload.
- Use the payload's `through`, not the incoming table's last time. With
  `'authoritative'`, an empty or short incoming table would keep stale held rows.
- Keep V6 patch tables time-ordered. Incoming rows inside the range are already
  complete rows (the reader resolves patches), so no overlay merge is needed.
  - Confirm this for live packed rows. The `'overlay'` path exists because
    sparse V6 backfills carry only some fields.
  - If a playback restore can return partial patches, merge the patch onto the
    preceding held row at `from` in the same way.

## Edge cases

| Case | Handling |
| --- | --- |
| Formation lap or SC ends while hidden (the original bug) | The event range brings back the terminal `SCAR`, and the banner clears |
| Flashback while hidden | `hiddenRewindFloor_` moves `restoreFrom` back to the target, and replace-range deletes the stale rows and events after it |
| Several flashbacks | The floor is the minimum of all targets |
| New session while hidden | The header `m_sessionUID` differs from `hiddenSessionUid_`, so `sessionChanged` causes `resetSession()` and a full install from `0`. Restarts depend on the unconfirmed UID-on-restart behaviour (see *Session identity*) |
| `SEND` while hidden | Covered by the event range. It must not call `resetSession()` through `handleMsg`, because restore never replays through it |
| Hidden for a whole race in All Laps | Chart range = hidden laps only. That costs at most the same as entering All Laps today |
| Restore in flight, then hidden again | The new hide resets tracking. The late payload is still valid history and installs normally. The next show issues a new restore from the new `hiddenSince_` |
| Playback seek (for example from a paired phone) while hidden | The authoritative flush is broadcast as today. The floor makes the restore cover the time after the seek |
| Rows in flight around the gate flip | Rows with `t ≤ through` are replaced by identical rows from the payload. Rows with `t > through` are kept |
| `config_.binaryPlayback` is off | The live store isn't populated. Confirm whether Electron always enables it. If not, restore can only re-emit latest rows, so document that limitation or require the flag |
| Paired Android displays | Untouched. Host visibility affects only the host consumer and the restore payload. Phones keep their own requirements and stream |

## Cost

- **Main memory while hidden:** goes from up to 600 s of packed rows plus
  32 MiB of JSON to **zero**.
- **Restore latency:** one history-worker round trip. A short window or the
  current lap is usually uncompressed and returns quickly. Long All Laps
  hides decompress only the hidden laps.
  - Charts show pre-hide data for that round trip instead of jumping straight
    to the cached window.
- **No IPC burst:** one payload, decoded cooperatively (`yieldToMainThread`),
  published once.

## Files

| Area | File |
| --- | --- |
| Engine | `protocol_parser_library/include/tnrp/Engine.h`, `protocol_parser_library/src/Engine.cpp` (`setHostVisible`, hidden tracking, restore request, rewind floor) |
| Live store | `protocol_parser_library/src/LiveHistoryStore.h/.cpp` (multi-range request) |
| Addon | `electron-frontend/node_addon/addon.cpp` (`setHostVisible`, new `onSeekFlush` arguments) |
| Main | `electron-frontend/src/main/bridgeManager.ts` (remove cache, call engine, payload fields), `electron-frontend/src/main/binaryRows.ts` (remove `chartHistoryRecords` if unused), `electron-frontend/src/main/diagnostics.ts` (remove resume fields) |
| Preload | `electron-frontend/src/preload/index.ts` (remove `onResume`) |
| Renderer | `electron-frontend/src/renderer/src/stores/telemetryStore.ts` (remove `onResume`, add `processRestoreFlush`), `lib/columnStore.ts` (`replaceRange`), `types.ts` (payload fields, remove resume types) |

## Verification

Manual, in the Electron app:

1. **Live: formation lap.** Minimize during the formation lap and restore a few
   laps in.
   - The banner is cleared.
   - The Session page lists the formation lap's `SCAR` events.
   - Charts have no gap in Current mode, 30 s mode or All Laps mode.
2. **Live: Safety Car or VSC.** It deploys and ends while hidden. The banner is
   not shown after restore, and no transient banners replay.
3. **Live: flashback while hidden.** After restore, charts and events after the
   flashback target match a session where the window was never hidden.
4. **Live: session change while hidden.** For example, quit to the menu and
   start a new session. After restore, the old session's state is gone.
5. **Live: long hide in All Laps.** All laps are present, and the restore time
   is logged with `playback-debug`.
6. **Playback.** Repeat 1–3 with a V6 recording and a V5 recording, so the
   non-V6 gap from problem 5 is checked as well.
7. **Diagnostics.** Main retention diagnostics show no hidden buffers.
   `[playback-debug]` logs one restore request and one install for each show.

## Implementation status

Implemented; not yet built or tested.

| Area | Where |
| --- | --- |
| Restore callback | `Sink::onRestoreFlush` + `Sink::RestoreFlushInfo` (`include/tnrp/Sink.h`). The default is a no-op, so Qt is unaffected |
| Multi-range read | `LiveHistoryStore::requestRanges` (with an `onStale` callback) |
| Engine | `Engine::setHostVisible`, `noteHostTimelineMovedLocked`, `issueLiveRestoreLocked`, `runPlaybackRestore`, `hostLatestRowsLocked`, `resetLiveSessionHistoryLocked` |
| Addon | `setHostVisible(visible, sequence)` runs on an `EngineCallWorker`. Restores share the seek-flush TSFN, with an 11th `restore` argument |
| Main | Resume cache, `chartHistoryRecords` and the resume diagnostics removed. `setRendererVisible` calls the engine |
| Renderer | `onResume` removed. `processPlaybackSeekFlush` handles `payload.restore`, and `applyRestoredLapState` rebuilds lap state. `installHistory` has a `'replaceRange'` mode |

Differences from the design above:

- **Order of current-state rows.** The engine emits them *after* the restore
  flush, not before. The addon holds JSON rows behind an in-flight flush, so a
  session-changed reset can't wipe them.
  - Chart-history families (telemetry, status, damage, lap, motion,
    motion_ex) are left out of that replay. An older latest row of one of them
    would look like a timeline reversal and truncate newer rows.
- **Stale restores.** A live restore that a rewind or session reset makes
  stale is reissued from the moved-back start instead of being dropped.
  - Hiding again before a live restore is delivered folds its start and its
    session change into the new hidden period.
- **Session change.** On a header `m_sessionUID` change the live history store
  is reset. A new session's times restart near zero, and
  `LiveHistoryStore::rewind` can't trim the old session for that, because no
  old lap starts early enough.
- **Lap times after a gap.** `onLap` now files `last_lap_ms` under
  `lap_num - 1` rather than the previous lap the renderer saw. The two are
  only different after a hidden gap.
- **Fastest lap.** When the lap number changed while hidden (or the session
  changed), the restore clears the live fastest lap and asks the engine for it
  straight away (`getLiveFastestLap`).
- **Starting state for status and damage.** Lap and window charts read their
  start value from the newest row *before* the start. Without one in the
  payload, that row was the last one held before hiding, so ERS and tyre wear
  began at their pre-hide values (100% and 100/100/99/99 after a formation
  lap).
  - **Live:** `RangeSpec::seedMask` makes the history store add the newest
    status and damage row strictly before the chart range, with its real time.
  - **V1–V5 playback:** the restore adds the same rows from
    `latestOfTypesTagged`. V1–V5 status is cut strictly at the range start, and
    their damage is resampled from it.
  - **V6:** the reader already seeds each field at the range start.
  - **Renderer:** the restore inserts a starting row only when it's newer than
    every held row before the range. V1–V5 damage starting rows carry the time
    of their last change, and must not be spliced into real history.
- **Zero session UID.** Packets with `m_sessionUID == 0` are ignored for
  session identity. The São Paulo 2026 capture has two before the session and
  several runs after `SEND`, alternating with the real UID. Treating them as a
  new session would have wiped live history at the end of every race.
  - A host hidden before any session was seen still reports `sessionChanged`
    on show. That is harmless, because there is nothing to reset.

Known limits:

- **Current-lap mode** restores only the current lap, which matches the
  existing live backfill. A previous lap that was partly hidden stays partial
  for comparisons until it leaves the working set.
- **`config_.binaryPlayback` off:** nothing is restored. Electron always sets
  it (`bridgeManager.ts` engine config: `binaryPlayback: true`), so this only
  matters for hosts that never call `setHostVisible`. The minimal frontend sets
  it to `false`, and Qt sets it to `true`.
- **Restarts:** `m_sessionUID` behaviour on a session restart still needs one
  capture (see *Session identity*).
