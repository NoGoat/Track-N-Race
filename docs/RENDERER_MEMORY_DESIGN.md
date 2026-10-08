# Renderer Memory Design

Status: implemented in the working tree on 2026-10-08, not committed. Electron
frontend only. The last section looks at what would carry over to Qt.

## Purpose

A 103-minute live session (`ram_usage (2).log`) ended at about 1.2 GB of
private memory, with the Electron renderer stuck near 575 MB. The renderer's
own estimate said it held about 20 MB of telemetry. This work found where the
memory actually went and removed it without changing anything the user sees.

| Measured at lap 11, 1x replay | Before | After |
|---|---|---|
| Renderer allocation rate, React Scan on | 270 MB/s | 56 MB/s |
| Renderer allocation rate, React Scan off | not measured | 18–25 MB/s |
| Renderer private, Current lap, before GC, React Scan on | 597 MB | 369 MB |
| Renderer private, Current lap, before GC, React Scan off | not measured | 239 MB |
| Column tables, All Laps | 52.9 MB, in doubling steps | 19.1 MB, smooth |
| Column tables, Current lap (working set) | about 8.8 MB | 4.4–5.8 MB |
| 55-minute race history (telemetry, status, damage, lap) | 79.8 MB, before slack | 37.5 MB allocated |

There were four stages:

| Stage | What | Effect |
|---|---|---|
| A | Stop rebuilding the lap-progress map on every render | Allocation 270 → 56 MB/s (React Scan on) |
| B | Measure honestly: renderer heap in the RAM log, real storage bytes | The numbers below are trustworthy |
| Fix | Charts missed restored history after a minimize | Correctness, found while measuring |
| C | Chunked Float32 column storage, decode in a worker, binary live laps | Tables about 2.8× smaller |

## How it was measured

Every number in this document comes from the same setup, so the stages can be
compared:

- **Data:** `tools/TNR stuff/udp sender and recorder/play_udp.py` replays the
  São Paulo capture at 1x. Never use `--xs` (16x); the app can't keep up.
  Commands, port check and how to stop leftover senders are in
  [QT_MEMORY_INVESTIGATION.md](QT_MEMORY_INVESTIGATION.md#running-the-udp-replay);
  they're the same for Electron.
- **Launch:** from the agent shell, with
  - `ELECTRON_RUN_AS_NODE` unset,
  - `--remote-debugging-port=9222`, and
  - `--disable-features=CalculateNativeWinOcclusion --disable-backgrounding-occluded-windows`.

  Without the last two, a window behind another counts as hidden, main stops
  forwarding rows, and the run measures nothing.
- **Probe:** a script on the DevTools protocol. Every 20 s it:
  - samples `Runtime.getHeapUsage` and each process's private memory
  - logs `document.visibilityState`
  - switches to the next page

  It also minimizes once for two minutes (through the app's own control) and
  restores with Win32 `ShowWindowAsync(hwnd, SW_SHOWNOACTIVATE)`. It takes heap
  snapshots mid-run and at the end, after a forced GC. A 45-second
  `HeapProfiler` sampling profile, with collected objects included, gives the
  allocation rate by function.
- **Snapshot analysis:** totals by node type and by class. Each typed-array
  backing store is traced up to the object that owns it, and each `Storage`
  object's retaining path is classified.
- **Pitfalls:**
  - Kill every `electron.exe` and every `play_udp.py` (each sender is two
    processes) before relaunching. A leftover app turns the launch into a
    "second-instance request". A leftover sender interleaves two timelines.
  - `debug.reactScan` in `%APPDATA%/track-n-race/config.json` was on in the
    original log, and it dominates retained memory (below).

## What the measurements showed

### Allocation churn, not retention

At lap 11 in Current-lap mode, the renderer's live JS heap after GC was 69 MB.
Yet the committed V8 heap had grown to 363 MB and renderer private memory to
597 MB. Most of the memory was garbage V8 kept committed to keep up with
allocation: 270 MB/s.

| Function (45 s sample, React Scan on) | Allocated | Share |
|---|---|---|
| `buildLapProgressMapFromPoints` | 8.6 GB | 71% |
| `findSectorSplitsFromProgress` | 0.9 GB | 7% |
| React Scan instrumentation | about 1 GB | about 8% |

**Cause:** `ChartCoordinatesProvider` rebuilt the current lap's distance map on
every render, creating one object per lap-progress point. Its cost grew through
the lap, and the provider re-renders with incoming telemetry. It also rebuilt
the comparison lap's map, which never changes, and Analysis rebuilt a map on
every pointer move.

### React Scan held most of what was retained

React Scan's change records (`changes`, `fiberState`, `fiberProps`,
`nodeInfo`, `groupedFiberRenders`) held old `FrozenView`s, and so old column
storages:

- **Storages:** 46.6 MB of the 62.7 MB of column storage (56 of 79 storages).
- **Objects:** 84% of 474,000 retained plain objects.

The app's own retained telemetry was about 25 MB: tables 11 MB, lap snapshots
5 MB, chart buffers 7 MB, other state 4 MB. React Scan is a debug toggle; no
code change. Turn it off before judging memory.

### The old estimate was wrong

The renderer estimated each table as rows × fields in the newest row × 8 bytes.
That ignored:

- capacity left over from doubling growth
- rows trimmed from the front but still allocated
- columns absent from the newest row
- non-numeric values
- old storages kept alive by views, which were reported as 0 bytes

### All Laps: the tables are the cost

At 17 minutes (lap 11), React Scan off, the All Laps run showed:

| Item | Size |
|---|---|
| Column tables | 52.9 MB, stepping 26 → 36 → 53 MB as capacity doubled |
| Chart buffers | 7.3 MB CPU + 6.0 MB GPU, flat |
| Renderer V8 heap | about 105 MB committed, 46 MB live |
| Renderer private outside V8 and Blink | 140–200 MB, including typed-array backing stores |

The tables grew about 3.4 MB/min, which projects to 340–500 MB for a 100-minute
race. Chart min/max outlines were considered and rejected: charts are flat at
13 MB, and tooltips, cursor sync, trends and Analysis need exact rows anyway.

## A. Lap-progress map

Files: `lib/lapDelta.ts`, `lib/chartCoordinates.tsx`,
`components/charts/AnalyzeTimeChart.tsx`, `components/AnalyzeScreen.tsx`,
`stores/telemetryStore.ts`.

### Map shape

`LapProgressMap` holds typed-array columns instead of an array of point
objects:

```ts
interface LapProgressMap {
  length: number
  time: Float64Array        // session_time
  elapsedMs: Float64Array   // current_lap_ms
  distance: Float64Array    // lap_distance_m
  maxSessionTime: number
  maxDistance: number
}
```

The arrays may be longer than `length` and may be shared with later maps of
the same lap, so only indices below `length` are read.

### Incremental builder

`LapProgressBuilder.update(rows, start, end)` consumes only the rows added
since the previous call, applying exactly the old build rules:

- the origin distance is the smallest distance among rows with
  `current_lap_ms === 0` at or after the start
- a synthetic first point is placed at the lap start
- rows at exactly the lap-start time are skipped
- on a repeated distance, the first arrival wins
- a row with the same time replaces the last point
- fewer than two points means no map

It rebuilds from scratch when:

- the lap's start or end time changes
- the rows shrink (a rewind or trim)
- the first row's time differs, or the last consumed row's time, elapsed or
  distance differs (the rows were rewritten)
- the end time stopped the build and a different view arrives (rows past the
  end aren't fingerprinted, but the split scan reads them)
- a new row would move the origin distance

A map object keeps its identity until its contents change, so memo comparisons
still work. Growing copies the arrays; maps already handed out keep theirs.
Replacing the last point writes in place, which earlier maps of the same lap
can see.

### Sector splits

The old scan created a `point` object per row and passed over any row whose
boundary wasn't on the map yet. The incremental scan has to give the same
answer as a full rescan, and a row skipped now could succeed against a longer
map later. So it records the first such row and its state (entered first
sector, splits found), and the next call resumes from there.

Boundaries already found are re-checked against the current map at the start
of each call; if one is no longer on it, the scan starts over. Distances are
always interpolated on the current map, as a full rescan would.

### Caches and lookups

- **Caches:** `buildLapProgressMap(lap)` and `findSectorSplits(lap)` are cached
  in `WeakMap`s keyed by the immutable `AnalyzeLapData` snapshot. That covers
  the comparison lap, `SessionTimer`, `AnalyzeScreen`, `AnalyzeTimeChart` (per
  pointer move and per lap) and `learnLiveSectorSplits`.
- **Shared lookup:** `interpolateDistanceAtTime(map, t, clampBeforeStart)`
  replaces three copies of the same binary search. `chartCoordinates` clamps
  before the start; Analysis returns NaN or null.

### Checks and results

- **Equivalence:** the old and new code ran over 63,609 real LapData rows from
  the capture (12 laps). The rows were parsed from the UDP file with the same
  offsets as `f1_26.cpp`. The scenarios were:
  - the live current lap after every row
  - finished-lap snapshots, including views running past the lap end
  - injected anomalies: NaN or backwards distances, repeated timestamps and
    distances, late origin rows, missing sector times
  - rewinds and front trims

  379,034 comparisons, all exactly equal.
- **Speed:** 641 µs to 20 µs per render (29 µs after C1's chunk indexing).
- **Live:**
  - allocation 270 → 56 MB/s with React Scan on, and 25 MB/s with it off
  - renderer private at lap 11, before GC, went from 597 to 369 MB with React
    Scan on, and was 239 MB with it off
  - Fastest, Previous, sector lines, titlebar delta and Analysis inspection
    were checked visually

## B. Diagnostics

Files: `preload/index.ts`, `main/diagnostics.ts`, `lib/columnStore.ts`
(`columnStorageBytes`, `countColumnStorages`), `stores/telemetryStore.ts`, and
the RAM viewer in `Track-N-Race-utils/track-n-race-dev-tools` (`ramLog.ts`,
`ramTypes.ts`, `App.tsx`, `RamChart.tsx`).

### `renderer_runtime_memory`

A new RAM-log category next to `main_runtime_memory`, logged once a second
while `debug.memoryLog` is on:

| Field | Source |
|---|---|
| `v8` | `process.getHeapStatistics()` in the preload (Electron reports KB; logged as bytes) |
| `blink` | `process.getBlinkMemoryInfo()` |
| `churn` | `allocated_lower_bound_bytes_per_s`, `reclaimed_bytes_per_s`, `heap_drops` |
| `process_metric` | the renderer's private and working set from `app.getAppMetrics()`, and private minus V8 heap minus Blink |

V8 exposes no allocation counter, so the preload polls the used heap every
100 ms between reports. Growth between polls is a lower bound on allocation,
because collections inside a poll interval hide some of it. Drops are what
collections reclaimed. The poll stops by itself 5 s after reports stop. Main
strips `runtime_memory` from the renderer's retention report into this
category.

### Column storage accounting

`columnStorageBytes(source, seen)` counts the bytes actually allocated: every
column at full capacity, trimmed rows included. Each storage, and after C1
each chunk, is counted once per `seen` set. Holders are charged in a fixed
order, so a later holder only shows storage nothing earlier keeps alive:

1. live tables
2. live lap snapshots
3. published views (shown as "pinned storage")
4. playback lap cache
5. All Laps driver cache

The existing keys keep their names, with accurate values. New keys:
`all_laps_driver_cache` and `column_storage` (`storages`, `bytes`).

Checked against a heap snapshot at lap 3: per table and for the snapshot
views, the log matched within a few percent.

### RAM viewer

Runtime categories are flagged `isRuntime`. Their headline is the committed V8
heap, and they're kept out of "All attributed categories" and "Peak
attributed", because those heaps already contain the attributed data. Heap,
Blink and process figures are components; churn rates are counters. New
renderer components: pinned storage, All Laps driver cache, column storage
total, storage count.

## Fix: charts missed restored history after a minimize

Found during the All Laps measurement: after a minimize, the surface, inner
and brake temperature charts and tyre wear drew a straight line across the
hidden period. The speed chart was fine.

**Cause:**

- **Arrival order:** on show, live rows resume at once and the restore payload
  lands later, in the middle of rows the charts already hold.
- **What the restore signals:** it bumps `analyzeLapRevision`.
  `SpeedRpmTimeChart` rebuilds on that in every mode.
- **Where `TimeChartView` falls short:** it only honours the lap revision in
  distance mode. On a time axis it relies on `historyRevision`, which is the
  constant `'AL'` in All Laps, so it never rebuilt. Its incremental sync only
  appends after its last point.
- **Same gap elsewhere:** Current lap and fixed time windows use the same
  path, and so did sparse V6 page backfills.

The distance-mode gate is deliberate: the lap revision also advances every
lap, and rebuilding a full All Laps buffer every lap would be expensive.

**Fix:** a separate `historyReplacementRevision` in the store, bumped only
where history is installed under rows charts already hold (a restore, a sparse
V6 backfill). `chartCoordinates` adds it to `historyRevision`, which
`TimeChartView`, `SpeedRpmTimeChart` and `GraphTable` already rebuild on. The
condition already existed in commit `c75fe7e`; no earlier change in this work
caused it.

## C1. Chunked, narrow column storage

File: `lib/columnStore.ts`. The public API is unchanged: `ColumnTable`,
`ColumnView`, `installHistory`, `decodeV6History`, `concatAfter`,
`tableFromView`, `tableFromRows`.

### Layout

| | Before | After |
|---|---|---|
| Time | `Float64Array` | `Float64Array` chunks |
| Numbers and booleans | `Float64Array`, one per field | `Float32Array` chunks (NaN = absent, booleans 1/0) |
| Other values | sparse JS array | chunks of plain arrays |
| Growth | double capacity and copy everything | add a chunk |
| Front trim | move `head`; memory freed at the next growth | move `head`; whole chunks behind it released |

- **Chunk size:** 4,096 rows (`CHUNK_BITS = 12`). A row's chunk is `i >> 12`,
  its offset `i & 4095`.
- **Chunk growth:** a chunk grows 256 → 1024 → 4096 rows by copying (at most
  4K rows), or is sized exactly when the row count is known (decoded history,
  `tableFromRows`).
- **Float64 exceptions:** `FLOAT64_FIELDS` keeps a field in Float64. It's
  empty: the game sends single-precision floats, and the integers read through
  `num()` (milliseconds, joules) stay below 2^24, where Float32 still holds
  them exactly. An audit found no `===` comparisons on table values.
- **Lazy columns:** a column gets a chunk only when a value lands in that
  chunk, so a field that appears late costs nothing before then.

16K-row chunks were tried first. All Laps reached 19.1 MB, but Current lap
only got to 6.8 MB. Its roughly 13K-row tables kept up to a full chunk of
trimmed rows plus a large partly used chunk. 4K-row chunks bring it to
4.4–5.8 MB.

### Sharing and ownership

Each column keeps `chunks[]` and `owned[]`. Three invariants make sharing
safe:

1. **Copy on write:** a chunk is owned by one storage, or shared and
   read-only. Writing to a shared chunk copies it first.
2. **Clean past the end:** every slot at or past a storage's `length` reads as
   absent. Chunks shared from another storage lie wholly below `length`, and a
   chunk cut by `length` is copied up to `length` only. Appending a row
   therefore never inherits stale values, for example after a rewind.
3. **Views read their own storage:** a `FrozenView` holds its `Storage` and
   absolute bounds. Newer storages share its chunks rather than copying them,
   so a held view pins only chunks the live table no longer has.

How each operation uses this:

| Operation | Behaviour |
|---|---|
| `append`, `appendPatch` | Write the next row; a full chunk is followed by a fresh one |
| `trimBefore`, `capRows`, `keepLastOnly` | Move `head`. Once it passes a chunk, `releaseHead()` moves the table to `slice(head, length, keepOwnership=true)` with the same row indices, so the live view stays |
| `truncateAt`, `retainRange` | `slice()` shares whole chunks and copies the chunk cut at the end |
| `StorageBuilder.appendRange` | Shares a source chunk when the whole chunk is in range and both sides line up; otherwise copies rows chunk by chunk |
| `StorageBuilder.alignTo(start)` | Starts the result at the source's offset within its chunk, so a first range lines up |
| `installHistory` | `replaceRange` shares the held prefix and copies only the changed middle and the live tail; `prefix` and `authoritative` share the decoded history |

An `appendPatch` at the newest row's timestamp still updates that row in
place, and views of the same storage still see it, as documented before. A
write into a chunk shared with another storage now copies it, so other
storages no longer see the update.

### Column kinds

A column's number or boolean kind is fixed by the storage it was first
written in, and `value()` returns booleans for boolean columns. The old code
had two behaviours, both kept exactly:

- **Direct copies** (rewinds, `retainRange`) create every source column even
  when no rows are copied. `slice()` calls `adoptColumns`.
- **`StorageBuilder.appendRange`** returns early on an empty range without
  creating columns.

The differential harness caught both differences before they shipped.

### Decode

`decodeV6History` merges blocks straight into exactly sized Float32 chunks,
skipping absent values. It no longer builds a full Float64 table first. The
parse step still expands each field to a temporary `Float64Array`; C2 moves
that off the UI thread.

### Cost

Each `num()` and `time()` call now indexes a chunk. The lap-progress benchmark
went from 20 to 29 µs per render, and live allocation and CPU didn't change
measurably.

## C2. History decode in a worker

Files: `lib/historyDecode.worker.ts`, `lib/historyDecodeClient.ts`,
`main.tsx`, `lib/columnStore.ts`.

### Protocol

- **Request:** `decodeV6History` calls the installed decoder. The client posts
  `{ id, bytes }` to a module worker. `bytes` is a copy, transferred, so the
  caller keeps its own.
- **Reply:** the worker runs `decodeV6HistoryChunks`, the same decode, then
  exports each family as chunk arrays. It transfers every chunk's ArrayBuffer
  (`decodedV6HistoryBuffers`) back without copying.
- **Install:** `tablesOfDecoded` wraps the chunks into `Storage` as owned
  chunks.

### Lifetime

- **Started:** on the first request.
- **Stopped:** 10 s after the last reply. Terminating it frees its whole heap,
  so a large decode's temporary memory never stays in the renderer.
- **Build:** loaded with Vite's `?worker&inline`, so it's a `blob:` worker and
  `file://` origin rules don't apply.

### Fallback and cancellation

- **Fallback:** without `Worker`, or if the worker throws, decode runs on the
  UI thread exactly as before, which also reports bad payloads as it always
  did. Installed in `main.tsx` through `setHistoryDecoder`, so Node tools and
  tests that import `columnStore.ts` decode in-thread.
- **Cancellation:** with the worker, `pause` is asked once, when the result
  arrives, and a stale result is dropped. On the UI thread it is still asked
  between slices.

### Observed

A watcher on the DevTools target list saw the worker start for each live-lap
fetch (about once per lap) and for each restore, and stop 10 s later each
time.

## C3. Binary transport for live laps

Files: `protocol_parser_library/include/tnrp/Engine.h`, `src/Engine.cpp`,
`node_addon/addon.cpp`, `main/bridgeManager.ts`, `preload/index.ts`,
`stores/telemetryStore.ts`, `types.ts`, `docs/LIVE_TELEMETRY_MEMORY_DESIGN.md`.

Before, `live_lap_data` and `live_fastest_lap_data` carried their V6H1 payload
as base64 inside a JSON row. The renderer then ran `atob` and copied the
result byte by byte into a `Uint8Array`.

### Engine

```cpp
using LiveLapCallback = std::function<void(std::string headerJson,
                                           std::shared_ptr<std::vector<uint8_t>> columnar)>;
void liveGetFastestLap(uint64_t requestId, LiveLapCallback done);
void liveGetLapData(uint64_t requestId, int lapNum, LiveLapCallback done);
```

The callback runs on the live store's read thread and is never called when
there's no such lap. The header holds `type`, `requestId`, `lapNum`,
`lapTimeMs`, `startSessionTime` and `endSessionTime`. The base64 helper was
removed. Only the Electron addon calls these methods.

### Addon

Each request wraps its JS callback in a `ThreadSafeFunction`, held by a
`shared_ptr` whose deleter calls `Release()`. It is released when the engine
drops the request, answered or not, so a request with no lap doesn't leak it.
The JS side receives `(headerJson, Buffer)`.

### Main, preload and renderer

- **Main:** `forwardLiveLap` sends `live-lap-data` with the header and buffer
  to every window. Like before, it's delivered while hidden: a dropped answer
  would leave the lap marked as requested. The two live-lap checks in the
  hidden-forwarding filter were removed.
- **Renderer:** `onLiveLapData` parses the header, attaches `history` as a
  `Uint8Array`, and calls the existing `live_lap_data` and
  `live_fastest_lap_data` handlers. `LiveFastestLapDataMsg.history` is now
  `Uint8Array`.

## Verification

| Check | Result |
|---|---|
| Phase A equivalence, real lap rows, 5 scenarios | 379,034 comparisons, equal |
| Column store differential, 7 seeds of random sequences | about 1.1 million comparisons, equal |
| Same, with decode through the worker's chunk path | equal |
| Real V6H1 payloads from playback (two 35 MB full-race histories), decoded in-thread and through chunks; installs across two drivers; held snapshots re-checked; truncate | 406.9 million field comparisons, equal |
| `npm run typecheck` (main, preload, renderer) and the viewer's typecheck | pass |
| All Laps live, 10+ laps, two minimize/restore cycles | continuous charts |
| Previous and Fastest on the binary path | render correctly; deltas shown |
| Playback of `udp_capture_sao_paulo_v6_2026-10-07.tnrd` | All Laps after a seek (35 MB payload), driver switch (34.7 MB), switch back through the driver cache matching the earlier screenshot |

The random sequences cover appends (rows and V6 patches, including
`available:false` and same-timestamp patches), caps, trims, `keepLastOnly`,
`truncateAt`, `retainRange`, `clear`, `installHistory` in all four modes,
`concatAfter`, `tableFromView` and `replaceWithView`. Up to 30 older snapshots
are re-checked after every operation, and every read API is compared against
the old values rounded to Float32. The payloads for the real-data check were
captured with a temporary hook on `webContents.send` in main (`--inspect`),
never committed.

The harnesses are temporary scripts in the session scratchpad, not in the
repo. To keep them, they would become unit tests over a reference copy of the
old `columnStore.ts`, plus a small fixture of real lap rows and payloads.

## Known limitations and open items

- **Not compared with the old build:** playback screenshots weren't taken on
  the old build for comparison; the data-level comparisons cover the same
  values.
- **Unopenable recording:**
  `f1_26_6_canadian_grand_prix_race_2026-09-24T16-31-57-729Z.tnrd` fails with
  "V6 recording contains no recoverable data", even after a clean close. The
  message comes from the native V6 reader, which this work didn't touch. Not
  checked against the old build.
- **Unattributed renderer memory:** private minus V8 heap and Blink is still
  about 100–200 MB (compositor, raster, allocator and typed-array backing
  stores). Not attributed further.
- **Remaining allocation:** 18–25 MB/s with React Scan off, mostly React
  rendering (`updateSyncExternalStore`, effects, callbacks) and store
  subscriptions.
- **Phase D, main process, not started:**
  - reuse parser result buffers (24 GB of allocations per 50 minutes in the
    original log)
  - drop committed chunks from memory while a recording file holds them
  - feed the writer structs instead of JSON

  Strategy keeps running in the background regardless.

## Files

| Area | Files |
|---|---|
| A | `renderer/src/lib/lapDelta.ts`, `lib/chartCoordinates.tsx`, `components/charts/AnalyzeTimeChart.tsx`, `components/AnalyzeScreen.tsx`, `stores/telemetryStore.ts` |
| B | `preload/index.ts`, `main/diagnostics.ts`, `lib/columnStore.ts`, `stores/telemetryStore.ts`, and the dev-tools viewer in `Track-N-Race-utils` |
| Fix | `stores/telemetryStore.ts`, `lib/chartCoordinates.tsx` |
| C1 | `lib/columnStore.ts`, `stores/telemetryStore.ts` |
| C2 | `lib/historyDecode.worker.ts` (new), `lib/historyDecodeClient.ts` (new), `main.tsx`, `lib/columnStore.ts` |
| C3 | `tnrp/Engine.h`, `Engine.cpp`, `node_addon/addon.cpp`, `main/bridgeManager.ts`, `preload/index.ts`, `stores/telemetryStore.ts`, `types.ts`, `docs/LIVE_TELEMETRY_MEMORY_DESIGN.md` |

## Porting to Qt

Qt's renderer side is built differently, so most of this work doesn't apply
directly. One idea does, and Qt has an issue of its own.

### How Qt holds telemetry

- **Samples:** fixed float structs per family in `SessionModel.h`
  (`TelSample`, 28 bytes; `TyreSample`, 68 bytes; `StsSample`;
  `DamageSample`; `MotionSample`; `MotionExSample`; `LapProgressSample`), in
  `QVector`s.
- **Rolling buffers:** `SessionData` keeps the whole session in `telBuf`,
  `stsBuf`, `tyreBuf`, `damageBuf`, `motionBuf` and `motionExBuf`, up to 750K
  rows (`kMaxRows`, trimmed with `remove(0, n)` beyond `kMaxRows + 4096`).
- **Laps, a second copy:** every live sample is also appended to `curLap`
  (`onTelemetry`: `telBuf.push_back` and `curLap.tel.push_back`, and the same
  for tyres and the rest). `finalizeCurrentLap` pushes the finished
  `LapBlock` into `laps`, which keeps every lap for the session.
- **Charts:** `TelemetryChart` reads samples and maps time to distance with
  `distanceAtTime`, a binary search on the lap's progress vector.
- **Decode:** history decodes (`TnrdPlayer::decodeHistory`,
  `decodeColumnarHistory`) run on `TnrdPlayer` worker threads.
- **Accounting:** `addSessionRetention` already counts `QVector` capacity, not
  size.

### Each stage against Qt

| Stage | Applies to Qt? | Why |
|---|---|---|
| A, per-render map rebuild | No | Qt never builds a progress map; it binary-searches `LapBlock::progress` directly. `sectorSplits()` allocates a small vector per call, which is cheap |
| B, honest accounting | Mostly done | Qt's estimate already counts capacity. What's missing is the process side: private memory against the estimate, to see unattributed memory |
| Restore gap fix | No | Qt's host is always visible (`setHostVisible` defaults to visible), so it never gets a restore payload |
| C1, Float32 columns | Already | Qt's structs are float |
| C1, lap segmentation and sharing | **Yes, in spirit** | See below |
| C1, no doubling slack | Partly | `QVector` doubles like the old `Float64Array` storage, and `remove(0, n)` copies the remaining rows |
| C2, decode off the UI thread | Already | `TnrdPlayer` decodes on worker lanes. C++ frees memory deterministically, so there's no GC ratchet to avoid |
| C3, binary live laps | No | Qt builds its own laps in-process and never calls `liveGetLapData`. The engine API change doesn't affect Qt |

### What a Qt port would actually be

**Store each sample once.** In live mode Qt holds the session twice: once in
the rolling buffers and once across `laps` plus `curLap`. That costs at least
2× for every family it records both ways (telemetry, tyre, status, damage,
motion and motion-ex). The same idea as C1's sharing applies:

1. **Lap blocks own the data.** Each lap's samples live in exactly one place,
   its `LapBlock`.
2. **Rolling buffers become views.** "Last N seconds" and All Laps read a
   range of lap blocks and binary-search the first. Charts that iterate
   `telBuf` today would iterate across lap blocks, which is a small adapter
   since the blocks are time-ordered.
3. **Trims drop whole laps,** instead of `remove(0, n)` on a 750K-row vector.
   That ends the O(n) front copies, and the `QVector` doubling slack only
   applies within one lap.
4. **Rewinds** (`truncateAfter`) drop laps after the target and truncate one.

Expected effect: about half of Qt's live telemetry memory, and no large
reallocations on long sessions. This is an estimate, not a measurement.

**Measure first.** Before porting, run a long Qt live session and compare:

- `addSessionRetention` against process private memory
- the rolling buffers against `laps`

That shows how large the duplication actually is, and whether anything else
dominates. The replay setup above works for Qt too; only the probe's
DevTools parts are Electron-specific.

**Not worth porting:** the lap-progress builder, the decode worker, binary
lap transport, and the restore revision. Qt doesn't have the problems they
solve.
