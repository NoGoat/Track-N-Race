# Track N Race — Architecture & Design

_Last updated: 2026-07-16 (branch `feature/opengl-charts`)._

Track N Race is an F1 24/25/26 telemetry suite consisting of four components in
one repository:

| Component | Path | Tech | Role |
|---|---|---|---|
| Telemetry engine (libtnrp) | `protocol_parser_library/` | C++20, glaze, Zstandard, zlib, libxlsxwriter | UDP receive, packet parsing, `.tnrd` record/playback, XLSX export, label/colour catalogs |
| Node addon | `electron-frontend/node_addon/` | N-API (node-addon-api, cmake-js) | In-process bridge exposing libtnrp to Electron's main process |
| Electron dashboard | `electron-frontend/src/` | Electron 42, React 18, Zustand, TimeChart (WebGL), Tailwind | Primary live dashboard + session player UI |
| Qt frontend | `qt_frontend/` | Qt 6 (Qt 5 fallback), QCustomPlot (OpenGL) | Standalone lightweight desktop app (recording + full dashboard UI) |

Both apps have feature parity and read/write the same `.tnrd` files.

## 1. The shared engine (libtnrp)

Everything protocol- or file-format-shaped lives in the library; the two apps
are hosts. The design rule: **one engine, two hosts** — a feature that touches
parsing, recording or playback is implemented once in C++ and consumed by both
UIs.

### 1.1 Components

```
tnrp::Engine  — orchestrator; the only class hosts construct directly
  ├── UdpListener   dual backend: raw winsock2/POSIX (TNRP_USE_QT=OFF, addon)
  │                 or QUdpSocket (ON, Qt recorder); one receive thread;
  │                 optionally forwards each raw datagram to ≤15 IPv4 targets
  ├── Parser        pure decode: format detect + override + debounce +
  │                 duplicate rejection + dispatch to protocols/f1_24|25|26.cpp
  ├── TnrdWriter    .tnrd V5 recording (own disk thread)
  ├── TnrdReader    V1–V5 playback (index, per-lap blocks, binary stores)
  └── Sink*         the single seam to the host (onRow/onBinary/onSeekFlush)
```

- **`Sink` (Sink.h)** — the engine pushes every parsed row through this
  interface. `onRow(json)` delivers pre-serialised JSON strings (cold + control
  rows); `onBinary(bytes)` delivers packed hot-row batches; `onSeekFlush(...)`
  delivers a playback seek backfill (binary-playback mode only). Calls arrive
  on the engine's UDP or playback thread — implementations must be
  thread-safe. Both optional methods default to no-ops so JSON-only sinks stay
  trivial.
- **`Config` (Config.h)** — host-supplied at construction: port, bind address,
  protocol override, logging, optional raw-UDP forwarding targets (maximum 15),
  plus two host-shape flags:
  `binaryPlayback` (Electron: playback hot rows go out via `onBinary`, seeks
  via `onSeekFlush`) and `hotRowsAsJson` (emit live hot rows as JSON instead of
  binary; both apps currently leave this **off** and take the binary channel).
- **`AnyRow` (AnyRow.h)** — typed decode seam for in-process consumers: one
  call turns a raw JSONL row into a `std::variant` of the typed structs from
  `rows.h`/`control_rows.h` (glaze-parsed after a cheap type-tag sniff). The Qt
  recorder routes every row through this; no dynamic JSON objects exist in its
  UI code.
- **Serialisation** is glaze end-to-end: hot structs in `rows.h`, control/
  playback/low-frequency structs in `control_rows.h`. Optionals are omitted by
  default (race_event) or emitted as explicit `null` via `writeJsonNullable`
  (protocol_status, protocol_warning, playback_loaded).

### 1.2 Hot/cold row split

The four per-frame packet-derived row types — **telemetry, motion, positions,
motion_ex** — are all-numeric, so they skip JSON entirely on the live path:
`BinaryRows.h` packs them into fixed-layout little-endian records
(`u8 tag` + fields; tags: 1 telemetry, 2 motion, 3 positions, 4 motion_ex) and
ships raw bytes. Other packet-derived rows follow the game's configured UDP
send rate and are delivered as JSON via `onRow`.

Hot rows are only serialised to JSON when they must be recorded
(`wantHotJson = recording || hotRowsAsJson` in `Engine::onDatagram`); with
logging off the JSON form is never built at all.

Three decoders exist and **must stay in lockstep** with the single C++ encoder:

| Decoder | Used by |
|---|---|
| `tnrp::bin::decodeBatch` (BinaryRows.h) | Qt recorder (typed structs, in-process) |
| `electron-frontend/src/main/binaryRows.ts` | Electron main (record lengths used by hidden-window history filtering) |
| `electron-frontend/src/renderer/src/lib/decodeBinaryBatch.ts` | Electron renderer (full decode to row objects) |

A schema drift here is silent corruption; treat any field change in
`BinaryRows.h` as a three-file change.

### 1.3 Parser: detection, override, duplicate rejection

`Parser::feed()` is a pure function of the datagram + override state — no I/O,
no persistence (the host persists the last detected format; Electron uses
`electron-store`).

- **Format detection**: packet-format word (bytes 0–1) must read 2024/2025/2026;
  auto-detection debounces 3 consecutive same-format packets before switching.
  A `protocol_status` control row is emitted on every switch. With a manual
  override active, a mismatching stream raises one `protocol_warning`
  (cleared with a null-field warning when the mismatch ends).
- **Packet cadence**: the game controls cadence through its UDP send-rate
  setting, and every received packet is parsed. Motion / car telemetry /
  motion_ex reject only an exact repeated `frameId` for the same packet type.
- **Dispatch**: `f1_24.cpp` / `f1_25.cpp` / `f1_26.cpp` are parallel ~500-line
  versioned parsers behind a shared `protocol.h` (header layout, byte readers).
  2026 adds `slm` (Straight Line Mode, the active-aero DRS replacement) and an
  8 MJ harvest scale.

### 1.4 Catalog pattern (labels, card colours, aero mode)

User-facing enum→string labels and card colour rules are **library-owned,
declarative data**, so both UIs render from one model:

- **`Labels.h`** — layered i18n catalog keyed by stable ids (`ers.mode.3`,
  `drs.label`, …): a format-neutral base overlaid with per-year patches (e.g.
  ERS mode 3 is "Overtake" in 2025, "Boost" in 2026; DRS becomes "SLM" in
  2026). Qt calls `labelsFor(format)` in-process; Electron receives the whole
  catalog embedded in every `protocol_status` row and resolves via a `t(key)`
  hook (`lib/labels.tsx`).
- **`CardColors.h`** — per-card colour specs: a default token plus ordered
  conditional rules (`on`/`op`/`value` → token, first match wins). Only
  semantic tokens (pos/neg/warn/…) are shipped; each app maps tokens to real
  colours with its own theme-aware palette. Format-independent.
- **`AeroMode.h`** — `"drs"` (≤2025) vs `"slm"` (2026), shipped on
  `protocol_status`; selects the track-map overlay (DRS zones vs SLM dry/wet
  zones).

For protocol 2026, `Session m_formula` gates those 2026 presentation overrides:
Formula 13 uses the F1 26 catalog/capabilities, while any other captured value
uses the legacy 2025 presentation without changing the 2026 wire parser. Missing
Formula metadata (older recordings or pre-session live state) defaults to F1 26.

`Parser::statusRow()` (live) and `Parser::statusRowForFormat()` (playback —
labels a loaded clip with *its* recorded format) both emit the full
`protocol_status` row: detected/active format, override, capabilities
(gameYear, hasBlisters, hasLiveryColors, hasLapPositions — 2025+), labels,
cardColors, aero_mode.

### 1.5 Recording (`TnrdWriter`)

Owns per-session file rotation, flashback handling and durability. Since the
disk-thread refactor, all writes happen on a dedicated writer thread fed by an
event queue (`SetLogging`/`NotePacket`/`Record`/`Close`); the engine's packet
path only enqueues. `isRecording()` is a relaxed atomic mirroring logging
intent so the per-packet fast path can skip the whole recording pipeline
(datagram copy + JSON enqueue + thread wakeup) when logging is off.

- **Rotation**: `notePacket()` watches session packets; a new track/session id
  closes the active compressed stream and starts a new file.
- **Flashback**: a 30 s rolling buffer absorbs in-game flashbacks — a
  session_time reversal within the window truncates the timeline and rewrites
  cleanly.
- **Dedup**: state-row types are deduped against the last written value.
- **Durability**: a codec flush every 300 rows (~5 s), so a crash leaves a
  stream decodable up to the last complete flushed row.

### 1.6 Playback (`TnrdReader` + `Engine::player*`)

`load()` detects the container signature. TNRD V1/gzip and V2/V3 monolithic
Zstandard use the legacy temp-file path (`tmpdir/tracknrace_*.tmp`) and build a
time/type index. V4/V5 first open their uncompressed metadata, lap table, chunk
directory, control summary, and commit footer. Load then streams only the cold
strategy dependencies to retain one processor checkpoint per completed lap;
decoded chunks, parsed rows, and worker scratch are released when that pass
finishes. Every consumer then decompresses requested playback chunks on demand
through a bounded cache. Binary playback packs hot rows
only as they are requested and retains those packed records in a separate bounded
seek cache. Raw decompressed JSON remains bounded by the archive LRU rather than
being duplicated for the whole session.
Every selected indexed chunk is an independent job
on an eight-worker reader pool, so long All Laps/range requests decode up to eight
chunks in parallel regardless of row type. Concurrent consumers of the same chunk
share one in-flight read and decompression before the result enters the LRU.
Legacy block reads (`readBlock`) fetch contiguous index
ranges with one `fread` into a reused scratch buffer. Temp-file positions are
64-bit on every platform because long sessions can decompress beyond 2 GiB.

With `setBinaryPlayback(true)` (used by both frontends), requested data uses the
packed playback path:

- requested hot rows (telemetry/motion/motion_ex) are encoded into packed binary
  and cached only within a bounded LRU;
- sparse cold rows (status/damage/lap) are read from indexed chunks on demand;
- reconstructs deduplicated Car Damage state at the specification's fixed
  10 Hz cadence for streaming playback, seeks and per-lap Analyze payloads;
- keeps `lapBlocksMessage()` to compact lap/control metadata for indexed files;
  detailed lap data is materialized only when requested. The initial metadata
  scan interpolates each lap's S1/S2 end distances from sparse Lap Data chunks.

`Engine`'s playback thread ticks every 16 ms (step capped at 0.1 s), advancing
an absolute session_time cursor scaled by speed:

- **Binary mode** (`pullUntilSplit`): due cold rows are appended as JSONL, due
  hot rows as packed bytes, and a bitmask records which type ids delivered.
  Car Damage is emitted only from its reconstructed 10 Hz stream. Other sparse
  panel row types (status, lap, session, timing, all_status, positions) that
  delivered nothing on a delivering tick are re-emitted from a **dup cache**
  with their `session_time` string-spliced to the playhead. The cache is seeded
  on load and re-keyed on every seek via `latestOfTypesTagged` (a backward index
  walk reading only matching lines).
- **Seek** (`playerSeek`): binary mode emits one `Sink::onSeekFlush` containing
  exactly the active history families for the selected mode: current lap,
  `[target-windowSeconds,target]`, or `[startTime,target]` for AL. V4/V5 resolve
  that range from its `(lap,rowType)` chunk directory and decodes only
  intersecting ZSTD blocks. Visible sparse state is restored separately with a
  single indexed latest-at-time query. Legacy mode
  emits a `playback_seek_flush` control row + snapshot instead.
  Electron All Laps seeks request `[startTime, target]` in that same flush;
  extraction runs on a libuv worker, the seek payload views the reader's
  immutable packed store until one IPC-compatible V8 Buffer copy, and request ids
  discard superseded scrubs before renderer IPC/decode.
  For V4/V5, requested chunks are decompressed through the bounded archive LRU;
  no whole-recording packed or JSON history is retained. V5 selects exact rows
  from persisted time metadata, while V4 discovers missing chunk time bounds as
  chunks are first touched.
  A request generation is registered before its worker is queued: playback is
  gated until that generation commits, so an overtaken worker cannot move the
  cursor or leak stale future rows into the winning seek. Electron adds a
  delivery barrier across the seek, JSON, and binary IPC channels: queued rows
  from the old cursor are discarded until the authoritative flush arrives,
  rows produced while the renderer installs that flush are bounded and buffered,
  and the renderer explicitly acknowledges the new timeline before those rows
  are released. A seek is therefore published atomically instead of briefly
  displaying samples from both sides of the cursor change.
- **Lifecycle rows**: `playback_loaded` (with the file header, or `ok:false`),
  `playback_lap_blocks`, `protocol_status` for the clip's format (binary mode),
  `playback_state` on every transition, `playback_finished` at the end,
  `playback_close` + live-format `protocol_status` on close. Pressing play at
  the end rewinds and replays.

Live and playback are mutually exclusive: `inPlayback_` (atomic) makes
`onDatagram` drop incoming UDP while a clip is loaded; recording is suspended
(`writer_.closeActiveStream()` on load).

JSON-only consumers (the Qt recorder drives `TnrdReader` directly, the legacy
pipe exe drives the engine without `binaryPlayback`) see the legacy all-JSON
playback stream unchanged.

### 1.7 XLSX export

`exportTnrdFileToXlsx()` opens its **own throwaway reader** on the file
(independent of any active playback), walks the legacy index or V4/V5 chunk directory and
writes one sheet per row type plus an "Info" sheet from the header, reporting
progress `(rowsDone, totalUnits, stage)`. Both apps run it off their UI thread
(Electron: `AsyncProgressQueueWorker` on the libuv pool; Qt:
`XlsxExportWorker` on a `QThread`) behind identical save-dialog + progress
overlay flows.

### 1.8 Threading model

| Context | Threads | Notes |
|---|---|---|
| libtnrp Engine | UDP receive, playback, writer disk thread, callers' control threads | One `mutex_` guards engine state; `inPlayback_` atomic gates the UDP path. The writer thread drains an event queue; the parse path never blocks on disk. |
| node_addon | engine threads → 3 TSFNs → JS main thread | Flush state is `shared_ptr` so queued callbacks survive wrapper teardown. |
| Electron main | single JS thread + libuv pool (XLSX export, player load) | No TS playback tick or hot-row pacing timer — the engine drives playback and addon-coalesced binary batches are forwarded directly. |
| Electron renderer | store ingest outside React; rAF loops per chart | Zustand slices re-render only subscribed leaves; All Laps charts append directly from stable full-session arrays. |
| Qt recorder | GUI thread + engine threads + playback load thread + export thread | `EngineSink` marshals rows to the GUI thread via queued signals. |

## 2. File format & shared conventions

- **`.tnrd`**: four supported generations. TNRD V1 uses
  gzip and `magic: "TNRD_V1"`; TNRD V2 and V3 use monolithic Zstandard with matching
  `magic` and `compression: "zstd"` header values. V3 adds `track_length_m` to
  the header and `lap_distance_m` to lap rows. `TnrdReader::load()` detects the
  codec from its native bytes, then uses and validates the JSON header to
  distinguish V2 from V3. Normal recording writes V5. Explicit
  `setLoggingGzip()` retains deprecated V1 writing for compatibility. Every
  subsequent line is one typed row with `session_time`; other telemetry row
  schemas remain compatible. V4/V5 use an uncompressed indexed control plane and
  independently checksummed `(lap,rowType,segment)` Zstandard JSONL chunks. Electron Analyze distance alignment and delta are
  enabled for V3/V4/V5; V1/V2 recordings retain elapsed-time overlays.
- **Row type ids** (assigned by `TnrdReader::scanType`, shared by the index,
  seek machinery and the engine's dup cache): 1 telemetry, 2 status, 3 damage,
  4 lap, 5 session, 6 race_event, 7 timing, 8 participants, 9 all_status,
  10 tyre_sets, 11 motion, 12 motion_ex, 13 positions, 0 other.
- **Temp-file convention**: both apps decompress to `tmpdir/tracknrace_*.tmp`
  and both sweep stale temps at startup (`TnrdReader::sweepStaleTempFiles`,
  exposed to Electron as `addon.sweepTempFiles()`), guarded by the
  single-instance lock / skip-on-open-failure semantics so an active session's
  temp is never reclaimed.
- **File association**: `.tnrd` (and legacy `.trnd`) are registered to the
  Electron app; opening a file while an instance runs routes through the
  `second-instance` handler to an in-app confirm dialog.

## 3. Electron app (`electron-frontend/`)

### 3.1 Process architecture

```
F1 game ──UDP:20777──> tnrp::Engine (in-process via node_addon)
                          │ onRow (JSON batch)      │ onBinary (packed hot rows)
                          ▼                          ▼
main: bridgeManager.ts  'telemetry-batch' IPC   direct real batches → 'telemetry-binary' IPC
                          │  (visibility-gated)     │  (addon-coalesced, visibility-gated)
                          └──── bounded hidden-window cache ────┘
                                      'telemetry-resume' IPC
preload (contextBridge)   ▼                          ▼
renderer: telemetryStore.ts (Zustand) ── slices ──> pages/components
                                        └─ TimeChartView (WebGL) per chart
```

The active page/layout is reduced through `historyDependencies.ts`, the single
declarative chart/table/map-to-row registry. `AppShell` sends the union as one
generation-tagged stream/history subscription. The engine filters cold and
packed-hot rows before N-API; V4/V5 playback loads only selected chunk families.
Hidden live chart families remain compact native raw history solely so a later
visibility change can backfill them without prior renderer processing.
Recording is never subscription-filtered.

### 3.2 node addon (`electron-frontend/node_addon/addon.cpp`)

`TNRPAddon` is an `ObjectWrap` that is itself the `tnrp::Sink`. The constructor
takes `(config, jsonCb, binCb?, seekCb?)` and creates up to three
`ThreadSafeFunction`s (unref'd so they don't hold the event loop open):

- **JSON coalescing** (`onRow`): rows append to a shared newline-delimited
  buffer; at most one flush is scheduled at a time (a swap-and-drain on the JS
  thread), so back-pressure degrades to bigger batches instead of queue
  growth. Buffers are `shared_ptr` so a queued flush survives `destroy()`.
- **Binary coalescing** (`onBinary`): same strategy over raw bytes, delivered
  as one `Buffer` per flush.
- **Seek flush** (`onSeekFlush`): user-rate, no coalescing — one copy handed
  to JS whole.

Long operations run on the libuv pool and resolve Promises:
`playerLoad` (`AsyncWorker`; a `loadBusy_` atomic makes a concurrent second
load resolve `false` instead of racing the playback thread) and
`playerExportXlsx` (`AsyncProgressQueueWorker`, marshalling progress ticks to
the JS progress callback). Module-level exports: `labelsJson(format)`,
`cardColorsJson()`, `sweepTempFiles()`.

### 3.3 Main process (`electron-frontend/src/main/`)

**`bridgeManager.ts`** owns the engine instance and all row routing:

- Constructs `Engine` with `binaryPlayback: true` and settings from
  `electron-store` (`udp.port`, `udp.bindAddress`, `udp.protocol`,
  `logging.*` — logging changes are pushed live via `store.onDidChange`).
- **JSON batches** are forwarded to all windows as `'telemetry-batch'`,
  gated on renderer visibility — except one-shot playback control rows
  (`playback_lap_blocks`/`playback_loaded`/`playback_close`), which must never
  drop. Control rows are additionally intercepted (cheap substring checks,
  then per-line JSON.parse): `protocol_status` feeds a cached copy +
  `udp.lastDetectedProtocol` persistence; `playback_state`/`playback_close`
  drive the playback-state channel.
- **Binary batches** are already coalesced by the addon's one-in-flight TSFN
  flush and are forwarded unchanged as `'telemetry-binary'`. Electron does not
  synthesize or retime hot rows, so chart samples retain the engine-provided
  `session_time` values.
- **Visibility gating** (`setRendererVisible`, fed by the renderer's
  `document.visibilityState` over IPC): while hidden, normal IPC forwarding
  stops and main retains only the selected chart window (hot chart records plus
  status/damage history; position records are excluded). On refocus this is
  coalesced into one `telemetry-resume` payload, bulk-applied by the renderer,
  and the cached `protocol_status` is re-pushed. The cache is time-bounded and
  compacted in chunks, so Chromium never accumulates a per-frame IPC backlog.
  `requestStatus()` lets a renderer pull the cached status on demand (e.g.
  after mounting with fallback labels).
- **Playback glue**: `playerLoad` closes any open clip first, tracks the
  active file path, and adapts the engine's
  `playback_state` (relative time → absolute, adds filename/isScanning) for
  the renderer. The seek-flush callback clears hidden-window history before
  broadcasting `playback_seek_flush_bin`.

**`index.ts`** — window/app lifecycle: single-instance lock (second instance
forwards a `.tnrd` path and exits pre-flash), tray icon + menu, frameless
window with custom titlebar (optional native titlebar setting), window sized to
60% of the work area (min 1200×700), Windows taskbar/tray icon theme detection
(async `reg.exe query` + 1.5 s poll + `nativeTheme` events), `.tnrd` open-file
handling (argv, macOS `open-file`, drag/drop confirm flow), and the IPC surface
(store get/set, dialogs, player controls, protocol config, UDP restart, XLSX
export with progress relay). `stopBridge()` on `will-quit` closes the player
and destroys the engine.

**`preload/index.ts`** exposes narrow bridges via `contextBridge`:
`electronStore`, `telemetryBridge` (`on`/`onBatch`/`onBinary`/`onResume`),
`windowControls`, `udpBridge`, `protocolBridge`, `fsBridge`, `playerBridge`.

### 3.4 Renderer state (`stores/telemetryStore.ts`)

The old `useTelemetry` hook is now a **module-level Zustand store**; the reason
is performance: the 60–120 Hz stream used to bump state at the App root and
re-render the whole tree. Now IPC ingestion writes to module-level buffers
*outside React*, and components subscribe to just the slices they read.

- **Buffers**: telemetry/motion/motionEx/status/damage arrays sorted by
  session_time. `appendRow` is O(1) amortized: retention trimming (10 min /
  750 k rows) only slices once 4096 stale rows accumulate; a session_time
  reversal (flashback) rebuilds the buffer and rolls back lap history / race
  events / fastest lap.
- **Published windows**: the visible time window (15 s–10 min, user-selected)
  is cut with binary search and filled into **double-buffered array pools**
  (two alternating arrays per slice, so identity-based memo deps see a change
  while the previous frame's array is never mutated mid-hold).
- **Dirty-slice recompute**: each delivered batch ORs a `DirtySlice` bitmask
  (Telemetry/Motion/MotionEx/Status/Damage/Derived) and only the touched
  window groups are recomputed — unchanged groups keep their array identity so
  their subscribers stay cold.
- **Resume backfill**: the single `telemetry-resume` payload appends retained
  hot and cold chart histories directly to their monotonic buffers, publishes
  current status/damage once, and recomputes affected slices once. This restores
  the selected window without a per-row Zustand/render storm.
- **Lap state**: lap-number changes snapshot the completed lap (last 3 kept +
  fastest), maintaining live lap times; in playback the same derived slices are
  served from the engine's `playback_lap_blocks` slim blocks instead, filtered
  to the playhead. A `SEND` race event (session end) resets the session when
  live.
- **Playback seek** (`playback_seek_flush_bin`): buffers are wholesale
  replaced from the decoded binary slice + cold JSONL, and status/damage/lap
  panel state is restored from the window's latest rows.
- Race events additionally fan out synchronously to `subscribeRaceEvent`
  listeners (toast banners) so React batching can't drop one.

### 3.5 Charts — TimeChart WebGL layer

The chart stack was migrated from uPlot (canvas 2D, destroy+recreate via
uplot-react) to **TimeChart** (WebGL) behind one reusable component. The old
chart packages and migration scaffolding have been removed.

**`components/charts/TimeChartView.tsx`** — the generic chart. Creates the
chart **once** and updates it imperatively (recreating a WebGL context per
theme/resize is exactly what the migration removed):

- Built on the maintained source fork under `lib/timechart/engine/`, imported
  from upstream `v1.0.0-beta.10` and installed as the local private package
  `@track-n-race/timechart-engine`. `lib/timechart/tc.ts` exposes a hand-picked
  plugin set — lineChart, crosshair, nearestPoint, plus custom plugins — instead
  of the default bundle (which would inject d3Axis/legend/zoom/tooltip). Fork
  provenance and local fixes are recorded in `engine/UPSTREAM.md`.
- **`dataBridge.ts`** reconciles the store's re-published windowed slices with
  the fork's aligned circular store: append genuinely-new samples into one
  shared X timeline plus typed Y channels, advance a logical head for front
  eviction, and rebuild on any non-contiguous window (seek/flush/restart).
- **`axisPlugin.ts`** draws axes/grid into two stable canvas layers (grid below
  WebGL, labels/borders above it), reproducing the uPlot look without per-tick
  DOM mutation: fixed or derived tick values, m:ss x labels, faint grid,
  L-frame borders, 11px Cascadia Code. **`referenceLines.ts`** supplies zero/threshold lines and
  **`ticks.ts`** supplies `niceTicks`. Step-aware translucent area fills are
  generated directly by the paged WebGL renderer from the complete series;
  they do not rebuild or pixel-bin a CPU canvas path.
  Plugin configs live in mutable refs so theme changes update colours in place
  and just redraw.
- **y-ranges** (`YRangeSpec`): `fixed`; `expand` (bounds only push outward on
  the newest sample — Ride Height); `auto` (uPlot-style fit: every publication
  cheaply expands by the newest sample, a full window rescan runs at most every
  200 ms so the axis can shrink — full scans per publication are what made the
  tyre charts lag).
- **Scrolling** (`hooks/useTimeChartScroll.ts`): one shared, visibility-aware
  scheduler slides every active x-window from the same animation-frame timestamp,
  extrapolating from the newest sample; stalls are detected from the stream's
  own measured rate (median arrival gap × 2), playback pause halts instantly
  via a module-level `playback_state` subscription, and small backward jumps
  hold rather than visibly scrolling backwards. `fastScroll` mode draws
  WebGL-only frames between throttled full model updates (axes/SVG work at
  ~60 fps, line scroll at display rate). Hidden charts are parked through an
  `IntersectionObserver`, and rAF stops when no visible chart has pending work.
  Sparse status/damage charts subscribe directly to the store's dense telemetry
  session clock (outside React), so their viewport motion is independent of
  their 2 Hz-ish sample cadence while their plotted data remains sparse.
- Tooltip: custom HTML element snapped to the nearest sample via binary search
  on the shared x-buffer (single index across series, like uPlot's
  `cursor.idx`).

Chart leaves are thin `TimeChartView` consumers: GForceChart, RideHeightChart
(Misc); GearChart, InputsChart, SteeringChart (Input); PowerBreakdownChart
(Power); TyreTrendCharts (Tyres/Overview thermal); SpeedRpmTimeChart under
SpeedRpmChart (Overview — mode logic for Current/Previous/Fastest/Compare lap
overlays with normalized series and muted reference traces). `GraphTable` is
the shared chart-alternative raw-values table.
All Laps charts and tables subscribe to source-family-specific imperative
signals, so a Motion append cannot wake Telemetry, Status, Damage, or MotionEx
consumers. Signals are coalesced per renderer task while every source row stays
in the bounded full-session arrays. Ordinary All Laps line traces use native
GPU line strips (one vertex per retained point); stepped traces retain their
explicit generated step geometry.

### 3.6 UI composition

`App.tsx` (2,098 lines) owns the shell: custom titlebar/window controls, tab
bar (Overview, Session, Strategy, Standings, Input, Power, Tyres, Misc),
window-seconds selector, race-event banner queue, playback controls bar
(transport, scrub, speed, lap jump, XLSX export with progress), per-tab layout
editors persisted via `electron-store`, and the Settings panel (UDP, protocol
override, recording, theme, attributions). `LabelsProvider` /
`CardColorsProvider` (from `protocol_status`) resolve all labels/colours.
`TrackMap` renders car positions outside React via rAF on bundled per-track
JSON outlines (`assets/maps/track_*.json`, shared with the Qt app), with
DRS/SLM zone overlays selected by `aero_mode`. Its Electron renderer uses two
Canvas 2D layers: cached track geometry is redrawn only when map styling,
dimensions or the follow-driver camera changes, while cars and cached
driver-label sprites render the latest received positions on the display-rate
foreground layer.

## 4. Qt frontend (`qt_frontend/`)

A single-window QWidget app (`MainWindow`) with a `QStackedWidget` of
self-contained pages — Overview, Standings, Session, Tyres, Strategy, Input,
Power, Misc — mirroring the Electron tabs, plus `AppToolbar` (page tabs,
session timer, chart-window selector, action icons), `ToastHost`
(race-event/safety-car notifications) and a playback transport bar.

### 4.1 Live path

```
tnrp::Engine (TNRP_USE_QT=ON, hotRowsAsJson=false)
  ├─ onRow (JSON)    ─┐ EngineSink: queued Qt signals (QByteArray, no UTF-16 round-trip)
  └─ onBinary (bytes)─┘        marshalled to the GUI thread
MainWindow::onEngineRow    — tnrp::parseRow → typed AnyRow; protocol_status
                             updates the label catalog / harvest scale / aero overlay
MainWindow::onEngineBinary — tnrp::bin::decodeBatch → typed rows directly
        └─> routeLiveRow: emitLiveData (panels) + ingestForModel (charts)
                          + feedHotSmoother (forward-fill)
```

- **Typed rows everywhere**: the UI never touches dynamic JSON. Cold rows are
  glaze-parsed once into `AnyRow`; hot rows never round-trip through JSON at
  all (the engine is constructed with `hotRowsAsJson = false`).
- **`HotRowSmoother` (`qt_frontend/src/HotRowSmoother.h`)** — a Qt-specific
  smoother, driven by `hotFillTimer_` at the measured cadence
  (`periodMs()` re-applied each tick): on an interval with no fresh telemetry
  it re-emits the last telemetry/motion/motion_ex one frame forward (capped at
  one frame past the last real sample); a large backward jump (flashback)
  drops fill state. Display-only.
- **Coalesced panel refresh**: packets only set per-panel dirty flags
  (`dirtyTiming_`, `dirtyTyres_`, …); `flushUiRefresh()` runs on a timer and
  rebuilds **only the visible page's** dirty panels — hidden pages refresh
  when shown, so a 20-row standings rebuild can't hitch the visible page.
- **Rendering gate**: when the window is hidden/minimized/occluded (tracked
  via a QWindow expose-event filter), all UI work pauses — refresh timers,
  chart flushes (`SessionModel::setLiveFlushActive`) — while UDP → parse →
  record continues untouched. Resume forces one full rebuild.
- **Toasts**: `ToastEvents` maps race-event codes (and session-packet
  safety-car transitions) to transient/persistent toasts; a one-shot suppress
  flag swallows the snapshot-induced SC toast after a playback seek.

### 4.2 Chart data model (`SessionModel` / `SessionData`)

`SessionData` is the plain (non-QObject) session core: slim per-sample structs
(`TelSample`, `TyreSample`, `StsSample`, `MotionSample`, `MotionExSample`)
in rolling buffers (live: 10 min trim; playback: whole session), per-lap
`LapBlock`s with lap-segmentation and fastest-lap tracking — the same logic as
the Electron store, in one place, shared by both feeders: `SessionModel`
mutates it live (signal flushes coalesced per event-loop pass), and
`TnrdPlayer` builds one on its background scan thread and swaps it in with
`load()`.

### 4.3 Charts (`ChartView` / `TelemetryChart`)

`ChartView` is a backend-agnostic chart widget: QCustomPlot lives entirely
behind a pimpl, callers declare axes/series/bands by spec and push data.
Features: multi-panel mode (several charts share one QCustomPlot → one GL
context/FBO/replot instead of N widgets; row-major or explicit-rows layout
with blank-cell holes for overlaid tables), adaptive per-pixel decimation once
the visible window is wide enough, hover crosshair + value readout, coalesced
`requestReplot()`, theme-following palettes. OpenGL rasterization
(`QCUSTOMPLOT_USE_OPENGL`, Qt 6 only) cuts the Speed/RPM chart from ~100 ms/
frame (software, seconds at a 10-min window) to single-digit ms; MSAA sample
count is a live-applied setting (`reapplyRenderSettings()`); main.cpp logs at
startup whether a GL context was actually obtained so silent software fallback
is visible. `TelemetryChart` is the Speed/RPM/ERS domain config over it, with
the same Default/CurrentLap/PreviousLap/FastestLap/Compare modes as Electron.
`GraphTable` provides the per-graph raw-values alternative, laid out on the
chart's own panel grid.

### 4.4 Playback

`PlaybackController` owns `TnrdPlayer` and the transport bar, raising
signals (`entered`/`exited`/`rowReady`/`seeked`/`timeChanged`/
`exportRequested`) for the app-level reactions. `TnrdPlayer` drives
`tnrp::TnrdReader` directly with its own Qt-timer clock (all-JSON legacy
reader mode, rows parsed to `AnyRow` before emission): `load()` runs
decompress + index + a full `scanIntoSessionData()` on a joinable background
thread (loading overlay meanwhile), then the scanned `SessionData` is moved
into the model — charts read the pre-scanned session, `rowReady` rows feed
only the panels. While in playback, live engine rows are dropped at
`onEngineRow` and recording is suspended; closing restores both.

### 4.5 Settings, theming, packaging

- Persistence is `QSettings` ("TrackNRace/NativeRecorder"): output directory,
  auto-record, theme (system/light/dark), widget style, protocol override,
  UDP port/bind, toolbar labels, per-section compact modes, per-graph
  chart/table views, track-map options, MSAA, toast options, window geometry
  (with maximize-aware normal-geometry capture).
- **Breeze bundling**: `breeze_stack/` is a CMake superbuild of the KDE Breeze
  style + its exact KF6 runtime closure. When `BREEZE_STACK_PREFIX` is set
  (native `build.ps1 -WithBreeze` / `build.sh --with-breeze`), the plugin +
  libs are copied next to the app (Windows: DLLs beside the exe; Linux:
  `lib_breeze/` with patchelf `$ORIGIN` RPATHs), `BreezePalette.cpp` applies
  the real KDE colour schemes via KColorScheme, and BreezeIcons registers the
  icon theme — so Breeze is selectable on machines without KDE.
- **Linux/AppImage** (`build-appimage.sh`): linuxdeploy-plugin-qt bundling;
  `main.cpp` adds the *host's* Qt plugin path (via `qtpaths`, pre-QApplication,
  through `QT_PLUGIN_PATH`) so "System Default" style resolves to the host's
  real style rather than falling back to Fusion inside the sandboxed plugin
  search; `--no-as-needed` keeps Svg/PrintSupport/OpenGLWidgets in DT_NEEDED so
  the deploy tool bundles their plugins.
- Fonts (Noto Sans), track maps, and licences ship as Qt resources.

## 5. Build & packaging

- **Electron**: `npm run dev/build` → electron-vite; `pre*` hooks run
  `scripts/gen-icon.mjs` and `scripts/build-bridge.mjs` (cmake-js build of the
  addon, which adds `protocol_parser_library` as a subdirectory with
  `TNRP_USE_QT=OFF`; `TNR_SKIP_BRIDGE=1` skips). Distribution via
  electron-builder: Windows NSIS + portable, Linux AppImage, macOS dmg;
  `.tnrd` file association registered.
- The addon **must** link as `SHARED` (not `MODULE`) on Windows: cmake-js
  injects `/DELAYLOAD:NODE.EXE` via `CMAKE_SHARED_LINKER_FLAGS`, which CMake
  only applies to SHARED targets; `win_delay_load_hook.cc` (compiled into the
  addon) redirects N-API resolution to the hosting process (electron.exe).
- **Native recorder**: standalone CMake project; Qt 6 preferred (enables
  QCustomPlot OpenGL), Qt 5 fallback (software rendering); Release + LTO/IPO
  by default, optional `-march=native`; `TNRP_USE_QT=ON`.
- **Library dependencies** are FetchContent-pinned (glaze v7.8.3, zlib v1.3.2,
  Zstandard v1.5.7, libxlsxwriter v1.2.4) with parent-project reuse guards (`if(NOT TARGET …)`);
  a shadowed `FindZLIB.cmake` points libxlsxwriter at the fetched zlib.
- **Release driver**: root `build.ps1` bumps the version
  (minor/major/overhaul), builds the native recorder, then
  `npm run build` / `dist` / `dist:win:portable`, with an optional
  version-bump commit. `build-release-candidate.ps1` is the RC variant.

## 6. Known issues & roadmap (prioritised)

### ~~P1 — Consolidate the duplicated playback engine~~ (done)
`sessionPlayer.ts` is deleted; the Electron app drives the addon's `player*`
API with `Config::binaryPlayback`. One engine, two hosts — see §1.6.

### ~~P1 — uPlot → TimeChart (WebGL) chart migration~~ (done on this branch)
All chart leaves render through `TimeChartView`; the old chart dependencies,
types, stylesheet, scrolling hook, and diagnostic plugin have been removed.

### ~~P1 — Delete the legacy headless pipe bridge~~ (done)
The obsolete `protocol_parser/` executable and its misleading README have been
deleted. Electron uses the N-API addon in-process.

### P2 — Table-drive the versioned protocol parsers
`f1_24.cpp` / `f1_25.cpp` / `f1_26.cpp` are ~500 lines each and largely
parallel. A shared field-offset table (or common structs with per-year deltas)
would shrink maintenance for each yearly game release, which is this project's
main recurring cost.

### P2 — Tests & CI
There are currently no tests. Highest-value, lowest-effort targets:
- Golden-file tests for the parsers: captured datagrams in, expected JSON rows out.
- Round-trip test for `TnrdWriter` → `TnrdReader`.
- A lockstep test for the binary hot-row encoding (C++ encode → TS decode →
  C++ decode), covering all four record types.

### P3 — Smaller cleanups
- `App.tsx` is 2,098 lines; split the titlebar/banner/playback-bar/tab
  components into files.
- Type the addon boundary (`engine: any` in `bridgeManager.ts`) with a
  hand-written `.d.ts` for `protocol_parser.node`.
- Dead playback plumbing: `telemetryStore.ts` still handles
  `playback_previous_lap_raw` / `playback_fastest_lap_raw`, which nothing
  emits; conversely the `playerGetLapData` chain (renderer preload → IPC →
  addon → `playback_lap_data`) is emitted but no renderer code consumes it.
  Delete both halves or wire them up.
- `speedRpmBlocks` and the playback lap-block plumbing are typed `any[]` in
  the store and `App.tsx`.
- Repo hygiene: sample `.tnrd`, `test_loop*.js`, `*.tsbuildinfo` at the root;
  commit messages are all "More changes".
