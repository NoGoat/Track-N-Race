# Track N Race — Architecture & Design

_Last updated: 2026-07-13 (branch `feature/opengl-charts`)._

Track N Race is an F1 24/25/26 telemetry suite consisting of four components in
one repository:

| Component | Path | Tech | Role |
|---|---|---|---|
| Telemetry engine (libtnrp) | `protocol_parser_library/` | C++20, glaze, Zstandard, zlib, libxlsxwriter | UDP receive, packet parsing, `.tnrd` record/playback, XLSX export, label/colour catalogs |
| Node addon | `node_addon/` | N-API (node-addon-api, cmake-js) | In-process bridge exposing libtnrp to Electron's main process |
| Electron dashboard | `src/` | Electron 42, React 18, Zustand, TimeChart (WebGL), Tailwind | Primary live dashboard + session player UI |
| Native recorder | `native_recorder/` | Qt 6 (Qt 5 fallback), QCustomPlot (OpenGL) | Standalone lightweight desktop app (recording + full dashboard UI) |
| Headless pipe bridge | `protocol_parser/` | C++ CLI | **Legacy.** Former out-of-process bridge (named pipe/UDS). Superseded by the N-API addon. |

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
  │                 or QUdpSocket (ON, Qt recorder); one receive thread
  ├── Parser        pure decode: format detect + override + debounce +
  │                 rate limit + dispatch to protocols/f1_24|25|26.cpp
  ├── TnrdWriter    .tnrd recording (own disk thread)
  ├── TnrdReader    .tnrd playback (index, per-lap blocks, binary stores)
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
  protocol override, logging, plus two host-shape flags:
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
ships raw bytes. Everything else (~2 Hz and rarer) is JSON via `onRow`.

Hot rows are only serialised to JSON when they must be recorded
(`wantHotJson = recording || hotRowsAsJson` in `Engine::onDatagram`); with
logging off the JSON form is never built at all.

Three decoders exist and **must stay in lockstep** with the single C++ encoder:

| Decoder | Used by |
|---|---|
| `tnrp::bin::decodeBatch` (BinaryRows.h) | Qt recorder (typed structs, in-process) |
| `src/main/binaryRows.ts` | Electron main (record lengths + session_time only, for the smoother) |
| `src/renderer/src/lib/decodeBinaryBatch.ts` | Electron renderer (full decode to row objects) |

A schema drift here is silent corruption; treat any field change in
`BinaryRows.h` as a three-file change.

### 1.3 Parser: detection, override, rate limiting

`Parser::feed()` is a pure function of the datagram + override state — no I/O,
no persistence (the host persists the last detected format; Electron uses
`electron-store`).

- **Format detection**: packet-format word (bytes 0–1) must read 2024/2025/2026;
  auto-detection debounces 3 consecutive same-format packets before switching.
  A `protocol_status` control row is emitted on every switch. With a manual
  override active, a mismatching stream raises one `protocol_warning`
  (cleared with a null-field warning when the mismatch ends).
- **Rate limiting** (fixed arrays indexed by packet id): motion / car
  telemetry / motion_ex dedupe on `frameId`; session and event packets are
  unlimited; participants 5000 ms; everything else 500 ms.
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

`load()` detects the container signature and decompresses either TNRD V1/gzip
or TNRD V2/Zstandard to a temp file (`tmpdir/tracknrace_*.tmp`),
builds a time/type index in one pass, and in the same pass builds the per-lap
blocks, scanned lap list, event log and fastest lap. Streaming reads return raw
JSONL lines (no re-parse); block reads (`readBlock`) fetch contiguous index
ranges with one `fread` into a reused scratch buffer.

With `setBinaryPlayback(true)` (the Electron path) the index pass additionally:

- pre-encodes hot rows (telemetry/motion/motion_ex) into a packed binary store
  with per-record times/offsets and a cumulative count per index position, so
  any index range maps to a contiguous byte slice;
- keeps the sparse cold rows (status/damage/lap) whole for seek flushes;
- fills slim per-lap chart points (`speed_kph`/`rpm` + `ers_pct`) into
  `lapBlocksMessage()` so the load payload is small.

`Engine`'s playback thread ticks every 16 ms (step capped at 0.1 s), advancing
an absolute session_time cursor scaled by speed:

- **Binary mode** (`pullUntilSplit`): due cold rows are appended as JSONL, due
  hot rows as packed bytes, and a bitmask records which type ids delivered.
  Sparse panel row types (status, damage, lap, session, timing, all_status,
  positions) that delivered nothing on a delivering tick are re-emitted from a
  **dup cache** with their `session_time` string-spliced to the playhead, so
  panels track the cursor between their native ~2 Hz updates. The cache is
  seeded on load and re-keyed on every seek via `latestOfTypesTagged`
  (a backward index walk reading only matching lines).
- **Seek** (`playerSeek`): binary mode emits one `Sink::onSeekFlush` — the hot
  rows of `[min(target−600 s, currentLapStart), target]` as one binary slice
  plus the window's cold status/damage/lap JSONL — then the state snapshot rows
  and an explicit positions restore (positions has no cold cache). Legacy mode
  emits a `playback_seek_flush` control row + snapshot instead.
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
(independent of any active playback), walks the whole index in file order and
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
| Electron main | single JS thread + libuv pool (XLSX export, player load) | No TS playback tick — the engine's playback thread drives; one flush timer paces both live and playback hot rows. |
| Electron renderer | store ingest outside React; rAF loops per chart | Zustand slices re-render only subscribed leaves. |
| Qt recorder | GUI thread + engine threads + playback load thread + export thread | `EngineSink` marshals rows to the GUI thread via queued signals. |

## 2. File format & shared conventions

- **`.tnrd`**: compressed JSONL with two supported generations. TNRD V1 uses
  gzip and `magic: "TNRD_V1"`; TNRD V2 uses Zstandard and
  `magic: "TNRD_V2", compression: "zstd"`. `TnrdReader::load()` detects the
  codec from its native bytes and then validates the JSON header/container pair.
  Normal recording writes V2. Explicit `setLoggingGzip()` retains deprecated V1
  writing for compatibility. Every subsequent line is one typed row with
  `session_time`; the telemetry row schema is shared by both generations.
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

## 3. Electron app (`src/`)

### 3.1 Process architecture

```
F1 game ──UDP:20777──> tnrp::Engine (in-process via node_addon)
                          │ onRow (JSON batch)      │ onBinary (packed hot rows)
                          ▼                          ▼
main: bridgeManager.ts  'telemetry-batch' IPC   HotRowSmoother → 'telemetry-binary' IPC
                          │  (visibility-gated)     │  (paced to measured frame period)
preload (contextBridge)   ▼                          ▼
renderer: telemetryStore.ts (Zustand) ── slices ──> pages/components
                                        └─ TimeChartView (WebGL) per chart
```

### 3.2 node_addon (`node_addon/addon.cpp`)

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

### 3.3 Main process (`src/main/`)

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
- **Binary batches** accumulate in `binPending` and are flushed by a single
  self-rescheduling timer through `HotRowSmoother`
  (`binaryForwardFill.ts`): the smoother measures the real frame period
  (median of telemetry session_time deltas, 0.01 ms granularity, so 20/40/60 Hz
  and fps-capped rates all lock in), forward-fills empty ticks by re-emitting
  the last telemetry/motion/motion_ex records with session_time advanced
  exactly one frame (never more than one frame past the last real sample), and
  the flush timer schedules against an **absolute deadline** so integer-ms
  setTimeout rounding can't drift the cadence. Fills are display-only;
  recording happened upstream in C++.
- **Visibility gating** (`setRendererVisible`, fed by the renderer's
  `document.visibilityState` over IPC): while hidden, hot channels are dropped
  in main (the smoother still drains so nothing accumulates); on refocus the
  cached `protocol_status` is re-pushed. `requestStatus()` lets a renderer
  pull the cached status on demand (e.g. after mounting with fallback labels).
- **Playback glue**: `playerLoad` closes any open clip first, tracks the
  active file path, resets the smoother, and adapts the engine's
  `playback_state` (relative time → absolute, adds filename/isScanning) for
  the renderer. The seek-flush callback resets the smoother *before*
  broadcasting `playback_seek_flush_bin` so a held fill can't leak a stale
  session_time across the jump.

**`index.ts`** — window/app lifecycle: single-instance lock (second instance
forwards a `.tnrd` path and exits pre-flash), tray icon + menu, frameless
window with custom titlebar (optional native titlebar setting), window sized to
60% of the work area (min 1200×700), Windows taskbar/tray icon theme detection
(sync `reg query` + 1.5 s poll + `nativeTheme` events), `.tnrd` open-file
handling (argv, macOS `open-file`, drag/drop confirm flow), and the IPC surface
(store get/set, dialogs, player controls, protocol config, UDP restart, XLSX
export with progress relay). `stopBridge()` on `will-quit` closes the player
and destroys the engine.

**`preload/index.ts`** exposes narrow bridges via `contextBridge`:
`electronStore`, `telemetryBridge` (`on`/`onBatch`/`onBinary`),
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
uplot-react) to **TimeChart** (WebGL) behind one reusable component. uPlot
survives only as type imports (`uPlot.AlignedData` in table helpers) and a
to-be-removed package dependency.

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
- **`axisPlugin.ts`** draws axes/grid into TimeChart's SVG overlay with pooled
  nodes (attribute mutation, not DOM churn), reproducing the uPlot look: fixed
  or derived tick values, m:ss x labels, faint grid, L-frame borders, 11px
  Cascadia Code. **`referenceLines.ts`** (zero/threshold lines),
  **`areaFill.ts`** (translucent fill to a baseline, step-aware),
  **`ticks.ts`** (`niceTicks`) and a draw profiler complete the plugin set.
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
DRS/SLM zone overlays selected by `aero_mode`. Its Electron renderer retains
track polylines in WebGL2 buffers and draws cars/markers as one instanced GPU
batch; a transparent Canvas 2D overlay composites cached driver-label sprites.
It can linearly interpolate between already-received position rows when the
motion packet's source cadence is a stable 60 Hz; 20/40 Hz streams stay raw,
and users can disable the persisted `mapInterpolation` setting to see raw 60 Hz
positions as well.

## 4. Native recorder (`native_recorder/`)

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
- **`HotRowSmoother` (`src/HotRowSmoother.h`)** — a C++ port of the Electron
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
All chart leaves render through `TimeChartView`; the unused uPlot scrolling
hook has been removed. Remaining cleanup: drop the `uplot`/`uplot-react`
dependencies, the type-only `import type uPlot` lines (`GraphTable`'s
`uPlot.AlignedData` prop type), and the retained `hooks/useDrawProfiler.ts`
profiling helper once the current performance investigation ends.

### P1 — Delete or clearly retire `protocol_parser/`
Its README claims Electron spawns it over a pipe; `bridgeManager.ts` actually
loads the N-API addon in-process. Dead code + wrong docs mislead contributors.

### P2 — De-duplicate the smoothing/forward-fill logic
Two ports of the same algorithm: `native_recorder/src/HotRowSmoother.h`
(typed rows, Qt timer) and `src/main/binaryForwardFill.ts` (packed bytes,
absolute-deadline timer). Candidate for a libtnrp-owned implementation behind
the Sink.

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
- Windows taskbar-theme detection shells out to `reg query` synchronously on a
  1.5 s poll in the main process; replace with an async query or a registry
  watcher.
- `speedRpmBlocks` and the playback lap-block plumbing are typed `any[]` in
  the store and `App.tsx`.
- Repo hygiene: sample `.tnrd`, `test_loop*.js`, `*.tsbuildinfo` at the root;
  commit messages are all "More changes".
