# Track N Race — Architecture & Design

_Last updated: 2026-07-11 (branch `feature/opengl-charts`)._

Track N Race is an F1 24/25/26 telemetry suite consisting of four components in
one repository:

| Component | Path | Tech | Role |
|---|---|---|---|
| Telemetry engine (libtnrp) | `protocol_parser_library/` | C++20, glaze, zlib, libxlsxwriter | UDP receive, packet parsing, `.tnrd` record/playback, XLSX export, label/colour catalogs |
| Node addon | `node_addon/` | N-API (node-addon-api, cmake-js) | In-process bridge exposing libtnrp to Electron's main process |
| Electron dashboard | `src/` | Electron 42, React 18, uPlot, Tailwind | Primary live dashboard + session player UI |
| Native recorder | `native_recorder/` | Qt 6 (Qt 5 fallback), QCustomPlot | Standalone lightweight desktop app (recording + full dashboard UI) |
| Headless pipe bridge | `protocol_parser/` | C++ CLI | **Legacy.** Former out-of-process bridge (named pipe/UDS). Superseded by the N-API addon. |

## 1. Data flow

### Live path (Electron)

```
F1 game ──UDP:20777──> UdpListener ──datagram──> Parser ──rows──┬─> TnrdWriter (.tnrd, when logging)
        (raw sockets)   (engine thread)   format detect,        │
                                          debounce, rate limit  ├─> cold rows: JSON strings ──> Sink.onRow()
                                                                └─> hot rows (telemetry/motion/
                                                                    motion_ex, 60 Hz): packed
                                                                    binary ──> Sink.onBinary()
node_addon (TNRPAddon : tnrp::Sink)
  onRow    ── newline-batched string, coalesced via ThreadSafeFunction ──> bridgeManager JSON callback
  onBinary ── byte-batched Buffer,   coalesced via ThreadSafeFunction ──> bridgeManager binary callback

bridgeManager.ts
  JSON batch  ──'telemetry-batch' IPC──> renderer (gated on page visibility)
  binary batch ─> HotRowSmoother (coalesce to measured frame period, forward-fill)
               ──'telemetry-binary' IPC──> renderer (gated on visibility)

renderer: useTelemetry.ts decodes batches, maintains 10-min rolling buffers,
derives per-lap state; TrackMap renders positions outside React via rAF.
```

Key design decisions on this path:

- **Hot/cold row split.** The three per-frame packet types (telemetry, motion,
  motion_ex) travel as a packed binary channel; everything else (~2 Hz) is JSON.
  Hot rows are only serialised to JSON when recording is on (`wantHotJson`).
- **TSFN coalescing** (`addon.cpp`): rows accumulate in a shared buffer and at
  most one flush is in flight, so back-pressure degrades to batching instead of
  queue growth.
- **Visibility gating**: when the renderer is hidden, hot channels are dropped
  in main (not buffered), and the cached `protocol_status` is re-pushed on
  refocus.
- **Format detection is stateless in the parser**; persistence of the last
  detected protocol lives in the host (`electron-store`), keeping the parser a
  pure function of inputs + override.
- **Catalog pattern**: labels (per-format i18n) and card-colour specs are
  library-owned declarative JSON (`labelsJson()`, `cardColorsJson()`), shipped
  on `protocol_status` rows by both the live parser and playback
  (`Parser::statusRowForFormat`), so the renderer sees one shared model.

### Live path (native recorder)

Same engine, built with `TNRP_USE_QT=ON` (QUdpSocket backend). `EngineSink.h`
adapts `Sink` to Qt signals; UI is QWidget pages backed by QCustomPlot charts
(OpenGL rasterization when available). Hot-row smoothing is
`native_recorder/src/HotRowSmoother.h`.

### Playback path

One engine: **C++ (`tnrp::Engine::player*` + `TnrdReader`)**, two hosts.
Decompress to temp file, index in one pass (per-lap blocks + slim chart points,
event log, fastest lap), stream rows from a 16 ms playback thread.

- **Native recorder** drives `TnrdReader` directly through its own Qt timer
  (`TnrdPlayer`), all-JSON rows.
- **Electron** drives the engine's `player*` API through the addon with
  `Config::binaryPlayback` on: hot rows (telemetry/motion/motion_ex) are
  pre-encoded at load into a packed store and stream out via `Sink::onBinary`
  — the same channel as live — so the TS `HotRowSmoother` provides
  measured-period pacing, forward-fill and visibility gating for both live and
  playback. Seeks flush a binary hot-row slice + cold JSON via
  `Sink::onSeekFlush` (`playback_seek_flush_bin` to the renderer); the playback
  loop re-emits the sparse panel rows (status/damage/lap/positions/timing/
  session/all_status) with the playhead's `session_time` so panels never look
  frozen; `playerLoad`/`playerClose` emit `protocol_status` to switch labels to
  the clip's format and back. `playerLoad` runs on the libuv pool
  (`Promise<boolean>`) so a multi-second index scan doesn't block the main
  process. `bridgeManager.ts` adapts `playback_state` (relative→absolute time,
  filename, isScanning) for the renderer.

JSON-only consumers (Qt recorder, the legacy pipe exe) run with
`binaryPlayback` off and see the legacy all-JSON playback stream unchanged.

## 2. File format & shared conventions

- **`.tnrd`**: gzip-compressed JSONL. First line is a header row
  (`magic: "TNRD_V1"`, includes `protocol` format year). Every subsequent line
  is one typed row with `session_time`. Written by `TnrdWriter` (session
  rotation keyed off the parser's active format), read by both playback stacks.
- **Row types** (both stacks share the numbering): 1 telemetry, 2 motion,
  3 status, 4 damage, 5 lap, 6 positions, 7 participants, 8 session, 9 timing,
  10 all_status, 11 motion_ex, 0 other.
- **Temp-file convention**: both apps decompress to `tmpdir/tracknrace_*.tmp`
  and both sweep stale files at startup (guarded by Electron's single-instance
  lock / skip-on-locked semantics).
- **Binary hot-row encoding** is defined in `tnrp/BinaryRows.h` (C++, the only
  encoder — live and playback). The decode side is mirrored in
  `src/main/binaryRows.ts` (record lengths, for the smoother) and
  `src/renderer/src/lib/decodeBinaryBatch.ts`. These must be kept in lockstep —
  a schema drift is silent corruption.

## 3. Threading & concurrency model

| Context | Threads | Notes |
|---|---|---|
| libtnrp Engine | UDP receive thread, playback thread, callers' control threads | One `mutex_` guards all mutable state; `inPlayback_` atomic gates the UDP path. `onDatagram` holds the lock across parse + record + emit. |
| node_addon | engine threads → TSFN → JS main thread | Flush state is `shared_ptr` so queued callbacks survive wrapper teardown. |
| Electron main | single JS thread + libuv pool (XLSX export via `AsyncProgressQueueWorker`, player load via `AsyncWorker`) | No TS playback tick — the engine's playback thread drives; the binary flush timer paces both live and playback hot rows. |
| Renderer | React state for cold rows; rAF/refs for hot rows | `MAX_ROWS` 750k cap + 10-min time cutoff per buffer. |

## 4. Known issues & roadmap (prioritised)

### ~~P1 — Consolidate the duplicated playback engine~~ (done)
`sessionPlayer.ts` is deleted; the Electron app drives the addon's `player*`
API with `Config::binaryPlayback` (binary hot rows, `onSeekFlush`, slim lap
blocks, sparse-row re-emission, label switch on load/close, async load). One
engine, two hosts — see §1 "Playback path".

### P1 — Delete or clearly retire `protocol_parser/`
Its README claims Electron spawns it over a pipe; `bridgeManager.ts` actually
loads the N-API addon in-process. Dead code + wrong docs mislead contributors.

### P2 — De-duplicate the smoothing/forward-fill logic
Two copies exist: `native_recorder/src/HotRowSmoother.h` and
`src/main/binaryForwardFill.ts` (which now paces both live and playback for
Electron). Candidate for a libtnrp-owned implementation behind the Sink.

### P2 — Table-drive the versioned protocol parsers
`f1_24.cpp` / `f1_25.cpp` / `f1_26.cpp` are ~500 lines each and largely
parallel. A shared field-offset table (or common structs with per-year deltas)
would shrink maintenance for each yearly game release, which is this project's
main recurring cost.

### P2 — Tests & CI
There are currently no tests. Highest-value, lowest-effort targets:
- Golden-file tests for the parsers: captured datagrams in, expected JSON rows out.
- Round-trip test for `TnrdWriter` → `TnrdReader`.
- A lockstep test for the binary hot-row encoding (C++ encode → TS decode).

### P3 — Smaller cleanups
- `App.tsx` is 1,783 lines; split the header/banner/playback-bar/tab components
  into files.
- Type the addon boundary (`engine: any` in `bridgeManager.ts`) with a
  hand-written `.d.ts` for `protocol_parser.node`.
- `useTelemetry.ts` still has handlers for `playback_previous_lap_raw` /
  `playback_fastest_lap_raw`, which nothing emits — delete the dead cases.
- Windows taskbar-theme detection shells out to `reg query` synchronously on a
  1.5 s poll in the main process; replace with an async query or a registry
  watcher.
- Repo hygiene: sample `.tnrd`, `test_loop*.js`, `*.tsbuildinfo` at the root;
  commit messages are all "More changes".
- Root `README.md` describes a deleted architecture (`udpReceiver.ts`,
  `packetParsers.ts`) and doesn't mention F1 26 support, the native recorder,
  or the library.

### Strategic question — two full UIs
The Qt recorder and the React app each maintain a complete dashboard (Strategy,
Session, Standings/Timing, Tyres, Power, Input, Misc pages; track map; toasts;
per-tab layout editors — ~15k lines of Qt UI mirroring ~10k lines of React).
Every feature is currently implemented twice. If both apps must stay
full-featured, extend the declarative-catalog pattern (labels, card colours)
upward — e.g. shared JSON chart/layout specs consumed by both UIs — so new
panels are defined once. Otherwise, scope the native app down to
record-and-review and let the Electron app be the rich dashboard.

## 5. Build & packaging

- `npm run dev/build` → electron-vite; `pre*` hooks run `scripts/gen-icon.mjs`
  and `scripts/build-bridge.mjs` (cmake-js build of the addon, which pulls
  `protocol_parser_library` in as a subdirectory with `TNRP_USE_QT=OFF`).
- The addon **must** link as `SHARED` (not `MODULE`) on Windows so cmake-js's
  `/DELAYLOAD:NODE.EXE` flag applies; `win_delay_load_hook.cc` redirects N-API
  resolution to the hosting process (electron.exe).
- Native recorder: standalone CMake project; adds the library with
  `TNRP_USE_QT=ON`; Linux packaging via AppImage (`build-appimage.sh`) with
  bundled Breeze style/icons and host-Qt plugin-path shims (see
  `native_recorder/src/main.cpp` comments).
- Dependencies are FetchContent-pinned (glaze v7.8.3, zlib v1.3.2, libxlsxwriter
  v1.2.4, nlohmann/json v3.11.3 in the recorder) with parent-project reuse
  guards (`if(NOT TARGET ...)`).
