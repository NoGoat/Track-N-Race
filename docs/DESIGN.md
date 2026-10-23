# Track N Race — Architecture & Design

_Last updated: 2026-10-21._

Track N Race is an F1 24/25/26 telemetry suite consisting of five components in
one repository:

| Component | Path | Tech | Role |
|---|---|---|---|
| Telemetry engine (libtnrp) | `protocol_parser_library/` | C++20, glaze, Zstandard, zlib, libxlsxwriter | UDP receive, packet parsing, `.tnrd` record/playback, paired-display server, XLSX export, label/colour catalogs |
| Node addon | `electron-frontend/node_addon/` | N-API (node-addon-api, cmake-js) | In-process bridge exposing libtnrp to Electron's main process |
| Electron dashboard | `electron-frontend/src/` | Electron 42, React 18, Zustand, TimeChart (WebGL), Tailwind | Primary live dashboard + session player UI |
| Qt frontend | `qt_frontend/` | Qt 6 (Qt 5 fallback), QCustomPlot (OpenGL) | Standalone lightweight desktop app (recording + full dashboard UI) |
| Android frontend | `android_frontend/` | Kotlin, Jetpack Compose Material 3, JNI | Phone/tablet dashboard, timing, charts, tyres and analysis; direct UDP, encrypted desktop pairing, on-device TNRD recording and playback |

The two desktop apps have feature parity and read/write the same `.tnrd` files.
The Android host runs either on its own (game UDP or a recording on the phone)
or paired to a desktop as a second screen that follows the desktop's playback.

## 1. The shared engine (libtnrp)

Everything protocol- or file-format-shaped lives in the library; the frontends
are hosts. The design rule is **one engine, multiple hosts** — a feature that
touches parsing, recording or playback is implemented once in C++ and consumed
by both desktop UIs, while focused hosts can consume the same row stream.

### 1.1 Components

```
tnrp::Engine  — orchestrator; the only class hosts construct directly
  ├── UdpListener   dual backend: raw winsock2/POSIX (TNRP_USE_QT=OFF, addon)
  │                 or QUdpSocket (ON, Qt recorder); one receive thread;
  │                 optionally forwards each raw datagram to ≤15 IPv4 targets
  ├── Parser        pure decode: format detect + override + debounce +
  │                 duplicate rejection + dispatch to protocols/f1_24|25|26.cpp
  ├── TnrdWriter    .tnrd V6 recording (own disk thread)
  ├── TnrdReader    V1–V6 playback (index, per-lap blocks, binary stores)
  ├── PairServer    WebSocket/approval/subscriptions/latest-state cache
  │                 (+ PairDiscovery: DNS-SD `_tracknrace-pair._tcp`;
  │                    PairCrypto: X25519/Ed25519/CPace + ChaCha20-Poly1305,
  │                    shared with the Android client)
  └── Sink*         host seam (onRow/onBinary/onSeekFlush/onPairState)
```

- **`Sink` (Sink.h)** — the engine pushes every parsed row through this
  interface. `onRow(json)` delivers pre-serialised JSON strings (cold + control
  rows); `onBinary(bytes)` delivers packed hot-row batches; `onSeekFlush(...)`
  delivers a playback seek backfill (binary-playback mode only), and
  `onPairState(public, persisted)` exposes UI-safe state plus an opaque document
  for host persistence. Calls arrive
  on the engine's UDP or playback thread — implementations must be
  thread-safe. Both optional methods default to no-ops so JSON-only sinks stay
  trivial.
- **`Config` (Config.h)** — host-supplied at construction: port, bind address,
  protocol override, logging, optional raw-UDP forwarding targets (maximum 15),
  plus two host-shape flags:
  `binaryPlayback` (Electron: playback hot rows go out via `onBinary`, seeks
  via `onSeekFlush`) and `hotRowsAsJson` (emit live hot rows as JSON instead of
  binary; both apps currently leave this **off** and take the binary channel),
  plus paired-server enablement, identity label, port and opaque saved state.
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

Four decoders exist and **must stay in lockstep** with the single C++ encoder:

| Decoder | Used by |
|---|---|
| `tnrp::bin::decodeBatch` (BinaryRows.h) | Qt recorder (typed structs, in-process) |
| `electron-frontend/src/main/binaryRows.ts` | Electron main (record lengths used by hidden-window history filtering) |
| `electron-frontend/src/renderer/src/lib/decodeBinaryBatch.ts` | Electron renderer (full decode to row objects) |
| `android_frontend/.../BinaryTelemetryDecoder.kt` | Android (direct JNI and paired WebSocket batches) |

A schema drift here is silent corruption; treat any field change in
`BinaryRows.h` as a four-file change. The paired handshake checks
`kBinaryRowsVersion` (PairCrypto.h); bump it with any layout change.

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
- **`TeamColors.h`** — per-protocol F1/F2 constructor ids, display names and
  preset hex colours. Hosts may supply sparse per-team overrides; the parser
  resolves override → preset → packet livery colour, and playback reapplies the
  current overrides to recorded participant/strategy state.
- **`AeroMode.h`** — `"drs"` (≤2025) vs `"slm"` (2026), shipped on
  `protocol_status`; selects the track-map overlay (DRS zones vs SLM dry/wet
  zones).

For protocol 2026, `Session m_formula` gates those 2026 presentation overrides:
Formula 13 uses the F1 26 catalog/capabilities, while any other captured value
uses the legacy 2025 presentation without changing the 2026 wire parser. Missing
Formula metadata (older recordings or pre-session live state) defaults to F1 26.

`Parser::statusRow()` (live) and `Parser::statusRowForFormat()` (playback —
labels a loaded clip with *its* recorded format) both emit the full
`protocol_status` row: detected/active/presentation format, raw Session
`m_formula` (when known), override, capabilities
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
  `FLBK` event supplies the authoritative target; only when that event is absent
  does a session-time reversal larger than 200 ms trigger the fallback. V5 keeps
  already-written payload chunks and commits a wall-clock-ordered branch cut;
  playback excludes every older-branch row from that session-time target onward.
- **Dedup**: state-row types are deduped against the last written value.
- **V5 durability/size**: row indexes stay in writer memory while recording.
  Thirty-second append-only checkpoints contain the current directory with
  optional index locators set to zero, so a crash can recover without repeatedly
  copying the cumulative index. The reader linearly scans selected decompressed
  chunks when an index is absent. Clean close writes the complete row index once
  and commits a final directory/footer that references it. Discarded rewind
  branches remain physically present; wall-clock branch metadata makes them
  logically invisible without decompressing and rewriting the target lap.
- **Live Strategy**: the UDP thread only places normalized dependency rows on a
  coalescing queue. A dedicated worker owns the live reducer, calculates at a
  bounded cadence, and publishes only the latest display snapshot. Flashback
  rebuilds use the engine's shared, cadence-limited cold-row histories with a
  generation guard; there is no duplicate full-session Strategy JSON journal.

### 1.6 Playback (`TnrdReader` + `Engine::player*`)

`load()` detects the container signature. TNRD V1/gzip and V2/V3 monolithic
Zstandard use the legacy temp-file path (`tmpdir/tracknrace_*.tmp`) and build a
time/type index. V4/V5 first open their uncompressed metadata, lap table, chunk
directory, control summary, and commit footer. V4 load then streams only the cold
strategy dependencies to retain one processor checkpoint per completed lap and
releases decoded chunks, parsed rows, and worker scratch when that pass finishes.
V5 preserves its direct indexed path: apart from the sparse Lap Data scan used
for sector metadata, payload and strategy rows remain lazy until requested.
Every consumer decompresses requested playback chunks through a bounded cache.
Binary playback packs hot rows
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

Analysis lap delta is also library-owned. `LapDelta` cleans the recorded lap
progress streams, interpolates both laps at common physical distances, applies
optional per-sector baselines, and returns a complete immutable delta curve.
Electron sends only the selected lap numbers/file sources through the addon;
the renderer clips the native curve to the playback cursor and performs WebGL
presentation work. Consequently delta progress is independent of Chromium's
hidden/minimized renderer scheduling.

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
  no whole-recording packed or JSON history is retained. V5 selects the complete
  requested range directly from persisted time metadata, while V4 processes long
  ranges in short slices as it discovers missing chunk time bounds.
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
| libtnrp Engine | UDP receive, playback, Strategy worker, writer disk thread, callers' control threads | One `mutex_` guards engine state; `inPlayback_` atomic gates the UDP path. Strategy parsing/calculation and recording I/O have independent queues, so neither blocks UDP receive. |
| node_addon | engine threads → 3 TSFNs → JS main thread | Flush state is `shared_ptr` so queued callbacks survive wrapper teardown. |
| Electron main | single JS thread + libuv pool (XLSX export, player load) | No TS playback tick or hot-row pacing timer — the engine drives playback and addon-coalesced binary batches are forwarded directly. |
| Electron renderer | store ingest outside React; rAF loops per chart | Zustand slices re-render only subscribed leaves; All Laps charts append directly from stable full-session arrays. |
| Qt recorder | GUI thread + engine threads + playback load thread + export thread | `EngineSink` marshals rows to the GUI thread via queued signals. |
| Android | main thread + engine threads (JNI) or OkHttp socket thread + source/playback/lap executors + raster workers | Hot rows decode on the producing thread into atomic slots; cold rows post to the main thread (§5.3). |

## 2. File format & shared conventions

- **`.tnrd`**: four supported generations. TNRD V1 uses
  gzip and `magic: "TNRD_V1"`; TNRD V2 and V3 use monolithic Zstandard with matching
  `magic` and `compression: "zstd"` header values. V3 adds `track_length_m` to
  the header and `lap_distance_m` to lap rows. `TnrdReader::load()` detects the
  codec from its native bytes, then uses and validates the JSON header to
  distinguish V2 from V3. Normal recording writes V6 (see `TNRD_V6_DESIGN.md`). Explicit
  `setLoggingGzip()` retains deprecated V1 writing for compatibility. Every
  subsequent line is one typed row with `session_time`. Telemetry rows expose the
  game's exact `rev_lights_pct` and 15-bit `rev_lights_bit_value` fields for live
  data and V5/V6 recordings; V1-V4 playback deliberately marks them unavailable so
  clients do not synthesize rev lights from RPM. V4/V5 use an uncompressed indexed
  control plane and independently checksummed `(lap,rowType,segment)` Zstandard
  JSONL chunks. V5 checkpoints omit row indexes, completed recordings write the
  full index once, and the reader supports either indexed or index-free control
  snapshots.
  Electron Analyze distance alignment and delta are enabled for
  V3/V4/V5; V1/V2 recordings retain elapsed-time overlays.
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
                          │  on show: one engine restore flush ('playback_seek_flush_bin' + restore)
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
  stops and main caches nothing, so Chromium never accumulates a per-frame IPC
  backlog. Main tells the engine (`setHostVisible`). On show the engine rebuilds
  what the renderer missed from its live history or the recording: chart
  families from the later of the chart window and the hide point, and in live
  mode race events from the hide point. It moves that start back for rewinds,
  seeks and loads while hidden, and reports a header `m_sessionUID` change.
  The result is one restore flush, followed by the latest current-state rows,
  and the cached `protocol_status` is re-pushed. See
  `docs/RENDERER_RESTORE_DESIGN.md`.
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
- **Restore after hidden**: the engine's restore flush installs each requested
  family with `replaceRange` (held rows before and after the range survive),
  merges race events without replaying banners, rebuilds lap state, publishes
  once and recomputes affected slices once. A session change resets the session
  first.
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
```

- **Typed rows everywhere**: the UI never touches dynamic JSON. Cold rows are
  glaze-parsed once into `AnyRow`; hot rows never round-trip through JSON at
  all (the engine is constructed with `hotRowsAsJson = false`).
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

## 5. Android frontend (`android_frontend/`)

A single-activity Kotlin/Jetpack Compose Material 3 app (`MainActivity` →
`TrackNRaceApp`). It has no WebView or JavaScript runtime. Native code is limited
to libtnrp behind one JNI library (`libtracknrace_android`,
`cpp/native_bridge.cpp`). Discovery, QR scanning, scoped storage and preferences
are plain Android code. Paired-mode protocol, security and product decisions
are specified in `docs/ANDROID_PAIRED_MODE_DESIGN.md`; this section covers the
host.

### 5.1 Sources

`TelemetryController` owns the active source and switches between three of them.
Only one runs at a time:

| Source | How rows arrive |
|---|---|
| **Direct** | `NativeTelemetry` → JNI → an in-process `tnrp::Engine` bound to `0.0.0.0:20777` (`hotRowsAsJson=false`, `binaryPlayback`, `sparseV6Playback`, `columnarV6History`, the desktop's engine mode). |
| **Paired** | `PairedTelemetryClient` → OkHttp WebSocket to a desktop `PairServer`, through the encrypted `PairChannel`. |
| **Recording on the phone** | The same JNI engine with no UDP listener: `openRecording(path)`, then play/pause/seek/speed/driver commands. It temporarily replaces the direct or paired source, which resumes on close. |

- **JNI bridge.** One `Session` (sink + engine) exists at a time. Every call
  takes its own `shared_ptr`, so a slow seek or lap read that finishes after
  `stop()` still has a live engine. `AndroidSink` forwards only the packed
  families the Kotlin decoder reads (telemetry, motion, motion_ex, positions).
  It shrinks `playback_lap_blocks` to the paired-phone form
  (`pairPhoneLapBlocks`) and turns a seek flush into `playback_seek_flush` plus
  its rows. A V6 columnar (`V6H1`) history block is skipped, because the phone
  reads history one lap at a time. While listening for the game, the engine row
  mask is fixed (`kAndroidRowMask`), and Strategy is added only while the
  Strategy tab is open (`nativeSetLiveStrategy`). Recording ignores the mask.
- **Threads.** These are `tnrp-native-source` (start/stop, source switches),
  `tnrp-playback` (ordered player commands; a newer seek id overtakes a running
  seek through `nativeRequestSeek`) and `tnrp-playback-laps` (Analysis/Graphs lap
  reads, kept off the player queue). Lap data and lap delta are served from the
  phone's recording when one is open and from the desktop otherwise, through the
  same `lap_data` / `lap_delta` reply shapes. A low-latency Wi-Fi lock is held
  while the app is in the foreground.
- **Lifecycle.** `onHostStop` suspends the source, or pauses a phone recording
  where it is. `onHostStart` restarts it. The QR scanner's activity hand-off is
  excluded, so pairing never races a restart. On Android 17+ the
  `ACCESS_LOCAL_NETWORK` permission is requested at launch, on return and before
  pairing. Without it, nothing listens or connects.

### 5.2 Data requirements

Each page states exactly the data it shows, and nothing else is loaded or sent
(`DataRequirements.kt`):

- `DataConsumer` is one piece of UI with a row-family mask (`DataRow`, libtnrp's
  logical ids) and the V6 fields it needs (`V6Data`, libtnrp's `V6DataType`).
- `PairedTelemetryPage` is the union of the consumers on one page or tab (the
  four dashboard tabs, timing, tyres, each Graphs tab in a time variant and a
  `*_LAPS` variant that adds lap progress, analysis, none). It also names the
  `GraphFamily` histories kept while that page is subscribed.
- `setActivePage` sends that union to whichever source is active: a paired
  subscription, `nativeSetRequirements` for a phone recording, or live Strategy
  for direct mode. It also enables or disables the graph histories. Pager tabs
  call it as they settle, so swiping the dashboard to Map subscribes to car
  positions only then.

The table is hand-written for Android and deliberately not shared with
Electron's `historyDependencies.ts`.

In paired mode, the phone follows the desktop. During V6 playback it shows the
driver chosen on the desktop (stamped as `player_idx` on projected rows) and that
driver's `driver_restriction`, with null meaning *unknown*, never *public*. The
app bar names that driver. A missing roster or restriction is re-requested every
3 s, because the desktop caches both. Only a recording on the phone lets the
phone pick the driver (the menu sheet's driver picker).

### 5.3 Ingest and state (`TelemetryStore`)

`TelemetryStore` is the thread boundary between libtnrp/OkHttp workers and
Compose:

- **Hot rows** never touch Compose state. `BinaryTelemetryDecoder` (a fourth
  `BinaryRows.h` decoder, see §1.2) decodes a whole batch on the producing thread
  and publishes the newest telemetry and all-car positions into `AtomicReference`
  slots (`hot`, `mapPositions`). The playback transport and the dashboard lap
  comparison also use atomic slots. Visible composables read them on the display
  frame clock, so playing or receiving recomposes nothing beyond the values that
  changed.
- **Graph samples.** When a Graphs or Tyres-charts tab is subscribed, the same
  decode also fills `RowSamples` for every hot record, not only the newest.
  These are handed whole to the main thread and appended to that family's
  `SampleHistory` (columnar, session-time ordered).
- **Cold rows** are parsed off the UI thread and applied to snapshot state on
  the main thread at their native cadence. V6 playback arrives as field patches:
  hot samples and timing/tyre-status rows merge into the last known value, while
  a complete row replaces it. A driver switch blanks the driver-scoped state
  before the desktop restates it. `participants_reset` and a local seek reset
  the session, but a reconstructed roster sent just before a reset is kept.
  Strategy rows are whole snapshots. After a seek, the previous snapshot stays
  dimmed until the rebuilt one arrives.
- Derived state that is shown at several places, such as the selected driver's
  name, restriction, cold labels and the tyre display, is read through
  `derivedStateOf(structuralEqualityPolicy())`. This keeps the app root from
  recomposing on every timing row.

### 5.4 Screens

Bottom navigation has five destinations: **Dashboard**, **Timing**, **Charts**,
**Tyres** and **Analysis**. Settings, Pairing and Licenses are secondary screens
reached from the app-bar menu sheet (`AppMenuSheet`, which shows the current
source and driver and the action that fits). Pages are hosted in an
`AnimatedContent` fade-through. An outgoing page freezes its state and stops its
work as soon as it becomes inactive.

- **Dashboard** is a pager with four tabs:
  - *Drive*: speed/gear/RPM lights, pedals, tyre temperatures and wear, ERS and
    fuel, lap progress and gaps.
  - *Map*: live all-car track map on the shared track outlines.
  - *Damage*: the shared `assets/car/f1-car-wireframe.svg`, parsed into zoned
    paths by `CarWireframe.kt`.
  - *Strategy* (`DashboardStrategy.kt`): a Defend/Attack plan picker, both plans
    as race-distance rails, the current stint's pace against the plan, and
    position/gap stats. It fits on one screen in either orientation.
- **Timing**: the timing tower with tyre compound and age, with a one-line row
  option.
- **Charts** (`GraphsPage.kt`): Power, Inputs, Speed, Misc and Trends tabs that
  mirror the desktop graphs. The window selector uses the desktop's values: last
  N seconds, a lap by distance with an optional faded comparison lap, stint, or
  all laps. One global window applies, with optional per-chart overrides.
  `GraphsController` backfills recorded laps behind the playback cursor with
  `request_lap_data`, a few requests in flight at a time so the newest laps
  arrive first. Trends computes per-lap ERS totals from the counters the game
  resets at the line.
- **Tyres**: *Allocation* (the available and fitted sets) and *Charts* (tyre
  temperature, brake temperature and wear history, on the same chart machinery).
- **Analysis**: *Graph*, *Split* and *Map* views of the lap under the playback
  cursor, or a fixed Lap A / Lap B pair, against a chosen lap. These are
  configured from a bottom sheet. Each lap is fetched once with only the channels
  drawn, and lap delta is the library's `LapDelta`.
- **Settings**: Appearance (theme, one-line timing, Wheel display), Telemetry
  (source, pairing, forget desktop), Recording (record sessions, folder), and
  About (licenses, rendered per component from `res/raw`).

**Charts render without decimation.** `GraphRaster.kt` draws each chart's lines
on `Dispatchers.Default` into a pair of bitmaps, and the bitmap on screen is
never drawn over. New samples extend the off-screen bitmap, copying only the
strip that changed since that bitmap was last current. Equal render requests
reuse the existing raster. Analysis charts and the Analysis map draw from
complete lap columns in the same way.

**Layout.** Material 3 width classes (`ui/WindowWidth.kt`) drive the layout. A
device whose shorter side is at least 600 dp counts as a tablet. In landscape on
a phone, the screen becomes a steering-wheel display by default: the app bars
hide until a tap and fade again after 3 s, and the dashboard rearranges into
columns. On a tablet, landscape keeps its bars unless the *Wheel display*
setting is on. Material You dynamic colour is used on Android 12+, with light
and dark fallback schemes.

### 5.5 Recording and recordings

- **Writing.** The JNI bridge exposes `Engine::setLogging`, so the shared
  asynchronous TNRD V6 writer records direct sessions; nothing is reimplemented
  in Kotlin. Recording is off by default. Files are staged in the app's external
  Documents directory (`Track N Race/`). When a Storage Access Framework tree is
  chosen, a serial worker moves each finalised file there. `recording_error`
  rows appear in the UI.
- **Library and player.** `RecordingLibrary` scans the staging folder, the
  chosen recording tree and any extra folders added through the system picker,
  with persisted read grants. Duplicates are removed by name, size and time.
  `nativeSummarize` reads each file's index header (track, session, protocol,
  start time, duration, drivers) without loading the file. `RecordingsSheet`
  lists recordings by day, newest first. A content-URI file is handed to the
  engine as `/proc/self/fd/N` while its descriptor is held open. Older V2–V5
  files need a temp directory, so `TMPDIR` is set to the app cache.
- **Player** (`PlayerBar.kt`). While closed, the player is a strip on the
  navigation bar (or along the screen edge in landscape) with a progress line
  and play/pause. Opened, it adds the lap picker, the clock, a scrubber with
  thinned lap ticks (one seek on release, not one per frame), the desktop's
  speeds, ±5 s skip and close. The clock is sampled on the frame clock and drawn
  rather than composed.

## 6. Build & packaging

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
- **Android**: Kotlin/Jetpack Compose Material 3 Gradle (wrapper checked in)
  application with an NDK 27 CMake build. It uses compileSdk/targetSdk 37,
  minSdk 26 and arm64-v8a, and release builds are minified.
  `build-and-run.ps1` / `.sh` installs and launches the debug APK on a physical
  device. `build-and-run-release.ps1` / `.sh` does the same for the release
  build type, signed with the debug key for local install only.
- **Library dependencies** are FetchContent-pinned (glaze v7.8.3, zlib v1.3.2,
  Zstandard v1.5.7, libxlsxwriter v1.2.4, libsodium 1.0.20 via libsodium-cmake,
  plus CPace for pairing) with parent-project reuse guards (`if(NOT TARGET …)`);
  a shadowed `FindZLIB.cmake` points libxlsxwriter at the fetched zlib.
- **Release driver**: root `build.ps1` bumps the version
  (minor/major/overhaul), builds the native recorder, then
  `npm run build` / `dist` / `dist:win:portable`, with an optional
  version-bump commit. `build-release-candidate.ps1` is the RC variant.

## 7. Known issues & roadmap (prioritised)

### ~~P1 — Consolidate the duplicated playback engine~~ (done)
`sessionPlayer.ts` is deleted; the Electron app drives the addon's `player*`
API with `Config::binaryPlayback`. One engine, multiple hosts — see §1.6.

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
