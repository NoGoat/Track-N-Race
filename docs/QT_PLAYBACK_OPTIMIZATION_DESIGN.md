# Qt Playback Optimization Design

_Status: proposed_  
_Scope: Qt frontend playback only_  
_Baseline: source tree reviewed 2026-09-02; no benchmark claims are made in this document_

## 1. Purpose

The Electron frontend now has a mature playback pipeline built around indexed native reads, packed hot-row delivery, bounded history, generation-safe asynchronous seeks, and demand-driven chart presentation. The Qt frontend has adopted several of the chart-side optimizations, but its playback feeder still follows the older eager-scan design. V4 and V5 now both remain payload-lazy and bound decompressed and packed data with LRUs.

This document records the current architectural differences and defines a staged plan for bringing Qt playback onto the same performance model. It is a design and implementation plan only. It does not authorize changes to the `.tnrd` format or to the shared parser/recording contracts.

The intended result is not a byte-for-byte port of the Electron/React implementation. Qt should reuse the same invariants while taking advantage of being in-process: typed C++ data, move-only publication, and shared ownership of native seek buffers.

## 2. Scope and non-goals

In scope:

- recording load time and memory use;
- steady playback at 0.25x through 4x;
- scrubbing, jump seeks, lap seeks, and replay from end-of-file;
- chart-history acquisition and publication;
- playback work performed while a page, chart, or window is not visible;
- GUI-thread scheduling, batching, and back-pressure;
- measurement needed to prove the optimization.

Out of scope:

- changing any TNRD generation or wire schema;
- changing F1 UDP parsing;
- replacing QCustomPlot or redesigning the Qt UI;
- preloading every row merely to make later views instant;
- lowering playback fidelity by dropping due data from an active consumer;
- changing Electron behavior.

The existing `tnrp::Engine`, `TnrdReader` binary-playback path, row masks, seek generations, range reads, and seek-flush callback are sufficient for the target design. The plan should therefore be implementable as Qt-host work without a shared-library contract change.

## 3. Current architecture

### 3.1 Electron playback

The Electron application uses the shared `tnrp::Engine` as its playback owner.

1. `bridgeManager.ts` constructs the engine with `binaryPlayback: true`.
2. `playerLoad` runs `Engine::playerLoad()` on a worker. The native reader publishes recording metadata and slim lap blocks rather than a second frontend-owned full-session model.
3. The native playback thread owns the 16 ms clock and emits packed telemetry/motion/motion-ex batches plus coalesced cold JSON.
4. `historyDependencies.ts` reduces the active tab, visible sections, selected graph/table modes, chart windows, and Analyze metrics into one stream mask and one history mask.
5. The engine reads and forwards only those families. Current-lap, finite-window, and All-Laps history are requested independently and additively.
6. The renderer keeps non-reactive source buffers, publishes only dirty slices, caps histories, and lets hot chart data bypass root React state.
7. Rapid seeks are asynchronous and latest-wins. The main process, native engine, and renderer use request generations plus a delivery barrier so one timeline is installed atomically.
8. Large seek payloads are decoded cooperatively. Normal playback rows produced during installation are retained behind a bounded barrier and released only after acknowledgement.
9. Charts keep a persistent WebGL instance, append new points incrementally, use aligned paged buffers, and share a visibility-aware frame scheduler. Playback cursor updates are imperative; the transport's React state is limited to roughly 10 Hz unless state changes structurally.

Principal evidence:

- `electron-frontend/src/main/bridgeManager.ts`
- `electron-frontend/node_addon/addon.cpp`
- `electron-frontend/src/renderer/src/lib/historyDependencies.ts`
- `electron-frontend/src/renderer/src/stores/telemetryStore.ts`
- `electron-frontend/src/renderer/src/app/hooks/usePlayback.ts`
- `electron-frontend/src/renderer/src/components/charts/TimeChartView.tsx`
- `electron-frontend/src/renderer/src/lib/timechart/dataBridge.ts`
- `electron-frontend/src/renderer/src/lib/timechart/engine/core/frameScheduler.ts`
- `protocol_parser_library/src/Engine.cpp`
- `protocol_parser_library/src/TnrdReader.cpp`

### 3.2 Qt playback

Qt uses a separate frontend-owned `TnrdPlayer` on top of `TnrdReader` rather than the playback path of the already-running `tnrp::Engine`.

1. Load runs on a `std::thread`, but after `TnrdReader::load()` it calls `readRange(startTime, totalTime)` with the all-row mask.
2. `scanIntoSessionData()` holds all returned JSON strings, removes raw damage, inserts reconstructed damage, stable-sorts the combined rows, parses selected rows, and builds a second full-session `SessionData`.
3. The same samples are retained both in whole-session vectors and in per-lap vectors. Telemetry also expands into a separate `TyreSample`, increasing duplication.
4. Once load completes, the full `SessionData` is moved to the GUI-owned `SessionModel`, regardless of the visible page or configured chart window.
5. A GUI-thread `QTimer` owns the 16 ms playback clock. Each tick synchronously calls `pullUntil()` or `drainRest()`, parses each JSON row into `AnyRow`, derives strategy, emits rows individually, and updates transport state.
6. `MainWindow` sends every cursor tick to Overview, Analyze, Tyres, Input, Power, and Misc. Hidden widgets normally decline the final refresh, but still receive cursor calls and can arm zero-delay timers.
7. Seeks run synchronously on the GUI thread. The scrubber correctly limits intermediate seeks to about 10 Hz and commits the release position, but each accepted seek still performs cursor movement, strategy reconstruction, state snapshot reads, JSON parsing, and UI publication before returning.
8. The Qt engine uses packed rows for live telemetry, but `TnrdPlayer` playback remains JSON-only and does not implement `Sink::onSeekFlush`.

Principal evidence:

- `qt_frontend/src/TnrdPlayer.cpp`
- `qt_frontend/src/PlaybackController.cpp`
- `qt_frontend/src/EngineSink.h`
- `qt_frontend/src/SessionModel.cpp`
- `qt_frontend/src/MainWindow.cpp`

## 4. Optimizations Qt already has

The playback redesign must retain, not replace, the useful work already present in Qt:

- live hot rows use `BinaryRows` instead of JSON;
- `SessionModel` coalesces related dirty notifications per event-loop pass;
- QCustomPlot instances are persistent;
- chart data paths support incremental append and front trimming;
- queued replots collapse duplicate paint requests;
- wide windows enable adaptive sampling;
- OpenGL rasterization is used when available, with a documented software fallback;
- chart refresh functions use binary search rather than scanning from the start of full buffers;
- hidden chart widgets avoid rebuilding their series, and a hidden/minimized/occluded main window suspends model repaint notifications;
- the scrub bar already uses the Electron-style 100 ms leading-edge throttle plus an exact final seek;
- playback load and XLSX export already run off the GUI thread.

The largest remaining gains are therefore before and around the charts: avoid acquiring irrelevant data, avoid duplicating it, keep blocking reader work off the GUI thread, and wake only the consumers that can present the result.

## 5. Difference matrix

| Concern | Electron frontend | Qt frontend now | Required Qt direction |
|---|---|---|---|
| Playback owner | Shared `tnrp::Engine` | Separate `TnrdPlayer` + direct `TnrdReader` | Make the existing engine the single live/playback owner |
| Playback mode | `binaryPlayback=true` | Legacy JSON playback | Enable binary playback and implement seek-flush handling |
| Load | Worker invokes indexed native load | Worker load followed by full `readRange` and frontend scan | Consume metadata/slim lap blocks; remove the eager frontend scan |
| V4 behavior | Metadata-only open followed by bounded on-demand chunk reads | Full all-family `readRange` builds an unbounded frontend model instead of using indexed reads | Preserve the shared reader's lazy behavior and eliminate any second Qt scan/materialization |
| V5 behavior | Metadata opens without full-session chart materialization | Full-range read decompresses all row families | Preserve V5's indexed/on-demand behavior end-to-end |
| History ownership | Requested families and ranges only | Every chart family for the whole clip | Family-specific bounded buffers with coverage tracking |
| Per-lap data | Slim load payload plus bounded on-demand lap cache | Full per-lap copies created for every family | Fetch comparison laps by mask and cache a small LRU |
| Hot-row transport | Packed binary batches | JSON strings parsed per row | Decode packed batches once and publish once per family batch |
| Cross-thread delivery | At most one JSON and one binary flush in flight | One queued signal per live engine callback; playback executes on GUI thread | Coalesce pending data; back-pressure grows a batch, never an event queue |
| Playback clock | Native playback thread | GUI `QTimer` | Use the engine clock; GUI only presents state |
| Tick work | Native range pull; one renderer batch ingest | Reader pull, JSON parse, strategy, signals, widgets on GUI | No file I/O, decompression, strategy replay, or per-row fan-out on GUI |
| Data subscription | Active UI reduced to stream/history masks | No playback subscription | Add one declarative Qt requirement registry and generation-tagged updates |
| Finite/current-lap history | Indexed range backfill | Full session is always resident | Request exactly current lap or the largest visible finite window |
| All Laps/Stint Laps | Additive, family-selective request | Already resident due to eager scan | Request on entry; append incrementally; discard when no longer required |
| Seek execution | Worker, cancellation, latest-wins generation | Synchronous GUI call | Queue engine seek work and register generation before queueing |
| Seek publication | Authoritative bulk flush and timeline barrier | State rows emitted directly; charts reuse pre-scan | Build off-thread and atomically swap one generation into the model |
| Stale seek protection | Native, main, and renderer generations | Scrub throttle only | Discard superseded extraction, decode, and publication work |
| Cursor consumers | Imperative hot cursor; transport state around 10 Hz | All pages plus slider/label/lap combo every tick | One cursor source; active render consumers only; throttle transport cosmetics |
| Chart scheduling | Shared visibility/focus-aware frame scheduler | Per-widget zero timers and queued replots | Centralize presentation budget and park hidden pages/charts |
| Hidden window | Hot forwarding stops; bounded resume data is restored | Painting pauses but playback tick/parse/control work continues | Continue engine clock, gate presentation, restore selected history/state on show |
| Buffer caps | 750,000-row hard cap plus chunked trimming | Playback vectors intentionally unbounded | Apply explicit family caps and chunked/ring eviction |
| Close | Native large stores are retired away from the UI path | Full `SessionModel::clear()` releases duplicated vectors on GUI | Move/swap large playback storage out in constant time and retire off-thread |
| EOF replay | Engine rewinds cursor before playing | `TnrdPlayer::play()` does not rewind | Inherited automatically by using `Engine::playerPlay()` |
| Diagnostics | Ingest/draw/long-task profiling hooks | Per-chart optional replot timing only | Add staged load, GUI-stall, batch, seek, memory, and queue-depth measurements |

### 5.1 V4-specific behavior and optimization boundary

V4 is an indexed container with an uncompressed control plane and independently
compressed payload chunks grouped by lap and row family. Opening a V4 recording
must retain only metadata, lap/control summaries, and bounded caches. It must not
reconstruct a whole-session packed history or keep sparse rows as raw JSON.

Unlike V5, V4 does not persist exact per-chunk time bounds or per-row offsets.
The reader therefore discovers a chunk's bounds when that chunk is first touched.
Range extraction is processed in short timeline slices so decoded JSON is packed
or consumed and then released instead of being aggregated for the entire request.
The archive's decompression LRU remains capped at 64 MiB and the packed hot-row
seek cache remains capped at 32 MiB.

The implementation must preserve these V4 invariants:

- derive recording metadata, laps, lap times, and control summary from the
  uncompressed control plane;
- scan only sparse Lap Data chunks needed for sector/distance metadata at load;
- read chart, state, lap, and strategy families only when requested;
- preserve chronological ordering by `(session_time, segment sequence)`;
- preserve cancellation, checksum errors, checkpoint recovery, cache limits, and
  early-V4 timestamp normalization through the shared reader APIs; and
- never retain a full-session raw-JSON or packed mirror.
### 5.2 Expected behavior by recording generation

| Generation | Unavoidable native load work | Work Qt must remove | Target steady/seek behavior |
|---|---|---|---|
| V1 gzip | Legacy monolithic decompression and indexing | Full frontend rescan, expanded duplicate history, GUI-thread destruction | Engine batches and bounded/demand-driven Qt histories after native load |
| V2/V3 Zstandard | Legacy monolithic decompression and indexing | Same Qt duplication and unconditional chart materialization | Same engine transport, seek generations, and presentation gating |
| V4 indexed | Control-plane open plus a bounded Lap Data metadata scan | Qt all-row `readRange`, JSON merge/sort, `SessionData`, per-lap family copies | Bounded on-demand seek flushes; lap/family-selective forward playback and Qt publication |
| V5 indexed | Control-plane/index open; requested payloads can remain on demand | Any unconditional full-range read or full-session Qt model | Metadata-only initial open followed by family/range-selective reads |

## 6. Target architecture

```text
                        worker commands
Qt controls  --------------------------------->  tnrp::Engine
  load/play/seek/speed/requirements                 |
                                                    | native playback thread
                                                    | selective TnrdReader reads
                                                    v
                    +-------------------- QtPlaybackSink --------------------+
                    | coalesced cold batch | packed hot batch | seek flush   |
                    +-----------------------+------------------+--------------+
                                            |
                           generation gate / bounded pending batch
                                            |
                         decode and assemble large flush off-thread
                                            |
                                            v
                                PlaybackSessionModel
                       current state + bounded family histories
                       lap catalog + small comparison-lap LRU
                                            |
                        active-page requirements and dirty-family signals
                                            |
                                            v
                             Qt chart presentation scheduler
                  visible charts only; incremental append; queued replot
```

### 6.1 One engine, two host modes

`MainWindow::engine_` should remain the only engine instance. It already owns UDP, recording, mutual exclusion between live and playback, playback state, seek generations, strategy checkpoints, and EOF rewind. Construct it with `binaryPlayback=true` and retain `hotRowsAsJson=false`.

`PlaybackController` remains the owner of transport widgets, but its player becomes a command facade over the engine rather than an owner of a second reader and clock. Long engine calls must run through a bounded Qt worker executor. Loads remain serialized; seek and requirements requests are registered with their generation before their worker is queued.

Playback control rows (`playback_loaded`, `playback_lap_blocks`, `playback_state`, `playback_finished`, and `playback_close`) need a Qt typed-control decoder. They should not be forced through `AnyRow`, whose current purpose is telemetry/UI rows.

### 6.2 A coalescing Qt playback sink

Extend the Qt sink adapter with three paths:

- cold/control rows appended to a reusable newline-delimited pending buffer;
- packed hot bytes appended to a reusable pending byte buffer;
- seek/history flushes transferred as a generation-tagged object retaining the reader's shared binary store plus begin/end offsets.

Only the empty-to-non-empty transition schedules a GUI delivery. A GUI callback swaps pending and draining buffers, clears the scheduled flag, ingests the batch, and retains capacity for reuse. If the GUI is behind, the batch grows; the number of queued callbacks does not.

For normal batches, binary decoding on the GUI thread is acceptable only if profiling keeps it inside the frame budget; it must still produce one model publication per dirty family, not one widget update per row. Large seek/history flushes must be decoded and assembled on a worker and moved into the GUI model in one short commit.

### 6.3 Declarative data requirements

Add one Qt registry equivalent in role to Electron's `historyDependencies.ts`. It maps every visible consumer to:

- row families needed for current state/streaming; and
- row families needing historical ranges.

Inputs include the selected page, layout section visibility, chart versus table mode, each chart's coordinate mode, the visible tyre mode, and selected Analyze series. Global toolbar/banner consumers are unioned separately. The coordinator computes:

- `streamMask`;
- `historyMask`;
- `historyWindowSeconds`: `-1` for All/Stint Laps, `0` for current-lap or Analyze scope, otherwise the largest visible finite window.

The resulting generation is sent once through `Engine::setDataRequirements()`. Enabling a new family requests only its missing range. Disabling a family releases its history while optionally retaining one latest state row. Coverage is tracked per family so page switches do not re-read already installed prefixes.

This coordinator is the source of truth. Individual widgets must not start independent reader requests.

### 6.4 Playback model

Replace the eager, duplicated playback shape with these logical stores:

- latest current-state row per family;
- bounded, time-sorted history buffer per chart family;
- lap metadata and slim speed/RPM/ERS blocks from `playback_lap_blocks`;
- current-lap progress/history only when a distance consumer needs it;
- a small mask-aware LRU of completed comparison laps;
- per-family coverage start and timeline generation.

Finite-window histories use append plus chunked/ring front eviction. The hard safety cap should match Electron's current 750,000 rows per retained family unless measurement justifies a smaller Qt-specific value. All-Laps data may remain append-only while that mode is active, but must still respect the hard cap and must be released when no visible consumer requires it.

Do not build `TyreSample`, motion, motion-ex, damage, and per-lap duplicates unconditionally. Materialize only selected families. Where multiple charts consume fields from one telemetry row, store the source sample once and expose field projections rather than maintaining parallel copies solely for presentation.

### 6.5 Atomic seeks

Retain the existing scrub throttle, then add the full seek protocol:

1. Allocate a monotonically increasing request id.
2. Register it with `Engine::playerRequestSeek()` before queuing work.
3. Freeze publication of the old timeline; keep the last complete chart frame visible.
4. Run `Engine::playerSeek()` on a worker with the current history mask and window policy.
5. Drop superseded flushes before decoding.
6. Decode the authoritative binary/cold payload off-thread into replacement family buffers.
7. During steps 4-6, retain post-seek stream batches behind a bounded barrier. Old-generation batches are discarded.
8. On the GUI thread, swap the new generation into the model, merge only newer retained rows, update current state, and emit one dirty-family publication.
9. Release retained batches after the commit.

Additive All-Laps, finite-window, and comparison-lap requests use the same generation vocabulary but must not replace the playhead. They merge only the requested families and update coverage independently.

Because Qt is in-process, a seek flush should retain the `shared_ptr` and byte offsets supplied by `Sink::onSeekFlush`; it should not make Electron's IPC-mandated buffer copy. Ownership is released after decode or cancellation.

### 6.6 Cursor and presentation scheduling

Create one playback cursor source. The full-rate cursor is consumed imperatively only by the active page's visible charts/map. The transport UI publishes immediately for structural changes (load, play/pause, speed, finish, close) and at no more than 10 Hz for progress-only changes.

Replace per-widget always-arm behavior with a shared chart presentation scheduler:

- one frame owner for all Qt charts;
- at most one refresh/replot per visible chart per permitted frame;
- hidden pages and hidden chart/table alternatives are parked, not merely checked after a timer fires;
- minimized, hidden, or unexposed windows perform no chart presentation work;
- focus/unfocus frame caps follow the same setting choices as Electron;
- resuming wakes the active consumers once after required history/state is installed.

Current incremental append/trim, adaptive sampling, queued QCustomPlot replots, and OpenGL setup stay in place. Remove remaining full-vector rebuilds from steady playback paths. All-Laps should append only the newly available suffix. Auto/dynamic Y ranges should expand from new samples and perform a full visible-range shrink scan on a throttle, rather than on every cursor tick.

### 6.7 Hidden-window behavior

Hiding or minimizing Qt must not pause UDP reception, recording, or the native playback clock. It should stop GUI delivery of hot presentation batches and stop chart scheduling. Keep only bounded data needed to restore the configured window, or request that range from the indexed reader when the window becomes visible again. Restore latest current-state rows and selected chart history before waking the visible page.

## 7. Implementation plan

### Phase 0 — Establish measurements

Add opt-in counters and scoped timers before changing ownership:

- load stages: container open, index/metadata, frontend scan, sort, parse/model build;
- indexed-load stages split by generation: control open, sparse lap-metadata scan, first requested payload, decompression-LRU bytes, and packed-cache bytes;
- steady tick: reader/decompression, JSON parse, strategy, model ingest, panel refresh, chart sync, replot;
- GUI heartbeat delay and count of tasks over 16 ms and 50 ms;
- cold rows, hot rows, bytes, GUI callbacks, and maximum pending batch size;
- seek request-to-native-commit and request-to-first-correct-frame latency;
- retained rows/bytes by family, lap cache size, and close-time destruction;
- V4 decompressed-chunk count, archive-LRU bytes, bounded packed-cache bytes, and payload decompression by request;
- active versus parked chart refresh/replot counts.

Use the same recordings and scenarios for every phase. Keep instrumentation opt-in and cheap when disabled.

### Phase 1 — Move Qt playback onto `tnrp::Engine`

- Enable binary playback in the existing Qt engine configuration.
- Convert `TnrdPlayer` into an engine command/state facade or retire it after moving its transport responsibilities to `PlaybackController`.
- Decode engine playback control rows in the Qt host.
- Run load and every potentially blocking seek/history/requirements call on bounded workers.
- Keep the load overlay active through metadata and sector-boundary discovery; do not eagerly materialize chart or strategy history.
- Remove the GUI-owned playback timer, direct `pullUntil()`, duplicate strategy processor, and direct reader cursor.
- Verify load, play/pause, speed, finish, close, and EOF replay before changing chart history ownership.

This phase removes the highest-risk duplication: two playback implementations with different correctness and performance behavior.

### Phase 2 — Add batched binary ingestion and the seek channel

- Implement coalesced cold and binary sink buffers with at most one GUI flush in flight.
- Implement `onSeekFlush` with shared-store ownership, request metadata, and cancellation.
- Change GUI ingest to accumulate dirty-family bits and publish once per batch.
- Add a bounded post-seek barrier and an atomic model commit.
- Keep the existing scrub throttle and exact release seek.

### Phase 3 — Replace the eager full-session scan

- Consume `playback_lap_blocks` for lap metadata and slim overview data.
- Remove `readRange(start, total)`, full JSON vector retention, damage insertion plus stable sort, and unconditional `SessionData` construction.
- Introduce bounded per-family histories, coverage tracking, and a small comparison-lap LRU.
- Move large model retirement off the GUI close path.

For V1-V3, native load may still require legacy decompression/indexing. The Qt frontend must avoid an additional full-session scan and duplicate model for every generation. V4 and V5 must preserve payload-lazy indexed range reads through to the UI.

### Phase 4 — Add the Qt data-requirements coordinator

- Define consumer-to-row-family mappings next to Qt page/layout configuration.
- Compute requirements on page, section visibility, chart/table, chart window, and Analyze-series changes.
- Apply generation-tagged requirements off-thread.
- Request only missing finite/current-lap/All-Laps history and discard disabled histories.
- For V4/V5, use masks to avoid reading unneeded row-family chunks and suppress irrelevant frontend work.
- Add hidden-window restore using the same coverage mechanism.

### Phase 5 — Make presentation demand-driven

- Route full-rate cursor updates only to the active page and visible consumers.
- Limit transport progress repainting to 10 Hz while preserving immediate structural changes.
- Introduce the shared visibility/focus-aware chart scheduler.
- Ensure hidden charts do not arm timers or queue replots.
- Convert any remaining per-tick vector reconstruction to incremental append or generation-triggered rebuild.
- Throttle full dynamic-range rescans and keep current adaptive sampling/OpenGL behavior.

### Phase 6 — Remove compatibility scaffolding and tune

- Delete the obsolete direct-reader playback scan and its duplicated state only after all supported recording generations use the engine path.
- Compare phase metrics, identify the remaining dominant GUI and render costs, and tune batch sizes/caps based on evidence.
- Update the authoritative architecture documentation after the implementation is complete.

## 8. Verification matrix

Test every supported TNRD generation that has a repository fixture or can be produced by the existing compatibility tests:

- V1/gzip, V2/V3 monolithic Zstandard, and V4/V5 indexed containers;
- finalized V4, checkpoint-recovered V4, and an intentionally corrupt V4 payload to verify recovery/error behavior remains native;
- short and full-race recordings;
- 1x and 4x continuous playback;
- pause/resume, skip backward/forward, lap selection, rapid scrub, and final exact scrub position;
- seek while playing and seek while paused;
- repeated seeks whose workers complete out of order;
- 15 s, 30 s, 1/2/5/10 minute, Current/Previous/Fastest/Selected Lap, Stint Laps, and All Laps modes;
- every page, chart/table alternatives, disabled sections, and Analyze metric changes;
- hide/minimize/occlude, wait, restore, and verify a continuous selected window;
- load a second file, close a large file, then return to live telemetry/recording;
- play after reaching EOF;
- OpenGL available and software fallback.

Correctness checks:

- at a chosen timestamp, current cards, timing, map position, strategy, lap, status, and damage agree with Electron;
- a committed seek never shows a mixture of old- and new-cursor rows;
- superseded seeks and requirement generations publish nothing;
- a newly enabled history family appears complete for its requested range;
- recording remains complete while live UI families are unsubscribed;
- no playback sample is synthesized by the live `HotRowSmoother`.

Performance gates:

- no file read, decompression, full strategy reconstruction, or whole-history parse occurs on the GUI thread;
- steady finite-window playback has bounded memory and no growing callback queue;
- hidden pages perform zero chart synchronization and zero replots;
- hidden/minimized windows perform zero chart presentation work;
- normal batches cause at most one model publication per dirty family and one replot per visible chart per frame;
- rapid scrubbing remains interactive and only the newest request reaches the model;
- a long V4 performs exactly one native warm/validation pass during engine load, performs no Qt full-range scan afterward, and installs no unrequested family history in the Qt model;
- loading and closing a long V5 recording do not materialize or destroy an unconditional duplicate full-session Qt model;
- V5 load does not regress to the V4 whole-payload warm behavior;
- 4x playback degrades into larger bounded batches, not increasing queued task count.

Initial latency budgets should be treated as hypotheses and adjusted after Phase 0 on the minimum supported machine. V4 load time must be reported separately from V5 because V4 remains proportional to payload size. A useful starting gate is no GUI task over 50 ms during steady playback or post-load V4/V5 scrubbing, with visible chart work staying inside the selected frame interval at the 95th percentile.

## 9. Risks and mitigations

### Cross-thread lifetime

The engine can call the sink from UDP, playback, and command workers. Sink state must outlive queued deliveries, shutdown must invalidate generations before object destruction, and workers must retain safe engine ownership. Use shared batch state and explicit shutdown ordering.

### Ordering across cold, binary, and seek paths

These are separate channels. Do not rely on Qt queued-signal arrival order across producer threads. Tag authoritative changes with a generation, gate normal rows during installation, and make the model commit the ordering boundary.

### Worker-pool starvation

Loads, seeks, history requests, and export can all be expensive. Use a bounded executor with serialized engine playback mutations and cancellation-aware history jobs. Do not create an unbounded `std::thread` per scrub event.

### Legacy recordings

V1-V3 still have unavoidable decompression/index costs. The target is to remove Qt's extra full scan and keep all subsequent work bounded; indexed-container expectations must not be falsely applied to legacy files.

### V4 startup expectations

V4 lacks V5's persisted per-chunk time bounds, so its first query may inspect more chunks while discovering those bounds. That discovery must remain bounded by the archive LRU and must not create a second whole-recording index. Measure control-open time, first-request decompression, retained cache bytes, and ready-to-first-frame separately.

### QCustomPlot container cost

Packed delivery alone will not fix a chart that replaces its entire `QCPDataContainer` every tick. Measurements must distinguish ingest/model cost from chart sync and replot cost. Preserve the existing incremental path and audit non-time modes separately.

### Memory retained by zero-copy seek payloads

Shared seek stores can keep a large recording generation alive after cancellation. Release superseded payloads immediately, cap retained post-seek batches, and expose retained-byte counters in diagnostics.

## 10. Completion criteria

The work is complete when Qt no longer owns a second playback reader/clock or performs an eager frontend full-session scan; V4 performs only its shared-reader warm/index work and V5 remains payload-lazy; playback and seeks use the engine's binary, generation-safe path; model/history memory is demand-driven and bounded; hidden consumers are parked; and the verification matrix passes with recorded before/after measurements.

The key architectural invariant is:

> Native format work happens once per generation-appropriate path; Qt materializes only requested families, moves them in batches, installs them by generation, and presents them only through visible consumers.
