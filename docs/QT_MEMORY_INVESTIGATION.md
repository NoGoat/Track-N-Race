# Qt Memory Investigation Guide

Status: written on 2026-10-08 from reading the Qt frontend code, then
measured the same day. The results and the changes made are in
[Measured on 2026-10-08](#measured-on-2026-10-08); the issue sections below
are the original reading and are kept for their reasoning. The Electron work
it builds on is in [RENDERER_MEMORY_DESIGN.md](RENDERER_MEMORY_DESIGN.md).

## Measured on 2026-10-08

Windows 11, RTX 4050 laptop GPU (NVIDIA driver 32.0.16.1088), Qt 6.11.1, a
1920×1080 window at 125% scaling. Live 1x replay of the São Paulo capture,
memory log at one sample a second. Most runs spend 4–16 minutes on Overview,
then visit every page for 40 s and return to Overview. Numbers are process
private bytes unless stated.

### What the memory is

| Where | Size | Notes |
|---|---|---|
| Graphics backend (Vulkan), idle | ~250 MB of 359 MB at idle | Driver allocations outside the heaps, plus ~80 MB of unused Vulkan allocator blocks |
| Chart render targets, MSAA 4x | ~36 bytes/pixel per chart before the changes | Colour + 4x MSAA colour + 4x depth-stencil; every visited page kept its targets |
| Engine (heap) | ~75 MB | Strategy rollback ~31 MB (at its cap), live recording-writer lap builders 28–47 MB (sawtooth per lap) |
| Session model, Current Lap | ~1.1 MB/min | Issue 1's double storage, confirmed: about half is the duplicate |
| Session model, All Laps | ~1.4 MB/min | |
| Chart series, All Laps | 49 MB at 27 min (16-byte points) | Issue 3, confirmed; grows with the race |
| Unattributed heap growth | ~35–40 MB | Widgets and Qt caches across all pages |

Backend comparison, same build and tour (runs 3 and 4):

| Backend | Idle | After the page tour, 17 min | GPU dedicated / shared |
|---|---|---|---|
| Vulkan | 359 MB | 553 MB | 187 / 70 MB |
| Direct3D 11 | 205 MB | 412 MB | 98 / 5 MB |
| Direct3D 12 | 502 MB | not run | 294 / 123 MB (idle) |

On Vulkan, private memory outside the heaps also jumps by 130–190 MB on some
page switches and only partly comes back. D3D11 doesn't do this. Compare Vulkan
runs by heap and chart figures, not process totals.

### Bug found: page switches wiped live history

`MainWindow::updatePlaybackDataRequirements` runs on every page switch and
calls `PlaybackController::setDataRequirements` before its `inPlayback_`
check. That called `SessionModel::retainPlaybackHistoryMask`, which cleared
the rolling buffers and every lap's samples for the families the new page
doesn't chart. In live mode, opening Standings, Session, Strategy or Damage
erased the session's chart history (All Laps, comparisons, Trends), and
`QVector::clear()` kept the allocations. Fixed: the mask only drops data in
playback, and drops free their memory.

### Changes made

| Change | Effect |
|---|---|
| Charts build a colour-only render target and own their MSAA buffer (`ChartView.cpp`, `RhiCanvas::makeTarget`) | No depth-stencil buffer: 36 → 20 bytes/pixel at 4x |
| Hidden charts free their MSAA buffer and vertex buffers; both are rebuilt on show | Page tour cost on Vulkan: +490 MB before, about +100 MB after |
| Chart points stored as float offsets from a per-series origin (8 bytes, was 16) | All Laps series 49 → 24 MB at 27 min |
| Large upload staging buffers are dropped after each full rebuild | Removes a CPU copy of each rebuilt series |
| Only the selected graphics backend is probed at startup; the rest when Settings lists them | ~15 MB of driver state not loaded |
| `retainPlaybackHistoryMask` live fix, and `SessionData::clear()` frees its buffers | See the bug above |
| Memory log: session split, per-chart breakdown, render targets, `gpu_texture_bytes`, QRhi statistics, Windows heap and committed-memory breakdown, unattributed remainder | The figures above |

### Bugs found while testing live page switches

- **No backfill into lap views.** Live, the engine streams only the families
  the visible page charts, and backfills the rest from its own history store
  when a page needs them. Qt merged that backfill into the session buffers
  only, so lap-relative views (Current, Previous, Fastest, Selected) kept a
  gap for the time spent on other pages, and Trends missed per-lap ERS when a
  lap ended on a page without status rows. The backfill now merges into the
  lap block (or loose block) that owns each sample's time
  (`mergeLiveHistory` in `SessionModel.cpp`).
- **Stale motion forward-fills.** Qt's `HotRowSmoother` (which Electron never
  had) re-emitted the last motion and ride-height rows on every late-telemetry
  tick, synthesising rows nobody received. A row held from the formation lap
  sat ahead of the race's reset clock, so it passed as current and was
  recorded at every fill while Misc was hidden; the backfill then interleaved
  the real rows with those grid values (ride height spikes, 3,388 in the
  first 40 s of lap 1). The smoother is removed: charts hold only received
  samples.
- **Formation lap kept under the race's times (bands in lap 1).** Rewinds
  were detected only on telemetry rows, which a page without telemetry (Misc)
  does not stream, so the race start's clock reset (128 s → 0) went unseen:
  the formation lap's samples stayed in lap 1's block and the race's were
  appended after them, out of time order, and every chart drew both laps
  interleaved. The renderer was not at fault: a self-test fed it two
  interleaved laps (time and distance axes) and stuck-value rows, and it
  matched a reference given the same data pixel for pixel. Qt now does what
  Electron's store does: lap rows (streamed on every page) also detect the
  rewind; `truncateAfter` cuts every block by time and moves the clock back;
  and each live append keeps its family in time order (jitter is inserted in
  place, a real regression drops that family's newer rows, as
  `reconcileReversal`), bumping the data revision so charts rebuild.
- **Tyres table cycled through all cars.** Live Tyre Sets rows arrive for
  every car in turn. Qt now keeps them per car and shows the player's, as
  Electron does.

### End-of-race playback, All Laps on every page

The São Paulo recording (92 min) seeked to ~minute 88, played at 1x, every
chart page visited. Playback stands in for live All Laps on the chart side
(same charts and sample counts); its session data and engine differ.

| Change | Input page | End of tour | Peak |
|---|---|---|---|
| Before | 1,002 MB | 789 MB | 1,131 MB |
| Static vertex buffers | 698 MB | 804 MB | 896 MB |
| + segment heap (`app.manifest`) | 692 MB | 750 MB | 949 MB |
| + playback lap copies only for the cursor's lap | 657 MB | 659 MB | 953 MB |
| + streamed V6 history decode | 628 MB | 700 MB | 844 MB |
| + hidden charts free their series, shared x columns, slimmer tyre samples | 591 MB | 570 MB | 712 MB |

Electron on the same run (production build, all processes): Input 688 MB,
end of tour 643–808 MB, peak 1,115 MB. The last row closes the three gaps it
showed:

- **Hidden charts hold no series.** Like Electron, a chart on a hidden page
  frees its points (`ChartView::setReleaseSeriesWhenHidden`) and its owner
  rebuilds from the session data when it is shown. Chart points went from
  ~106 MB (every visited page) to the visible page's share.
- **Shared x columns.** Series with identical keys (Speed and RPM, the four
  tyre corners, Input's channels) share one key column, 4 bytes a sample;
  each series stores only its 4-byte values. A series whose keys diverge takes
  a private copy. Overview's chart points: 56 → 34 MB.
- **Slimmer tyre samples.** `TyreSample` carried tyre wear (16 of 68 bytes),
  copied from the damage family into every sample and never read. Wear stays
  in `DamageSample`.

Run-to-run variation is about ±40 MB (tour timing shifts how much history
each page holds). Each step keeps every sample:

- **Playback lap copies.** Seek and window history was stored twice, in the
  session buffers and in every lap block. Now only the lap under the cursor
  gets sample families (it backs the current-lap fallback); other laps keep
  their progress and lap views read them from the indexed-lap cache, as they
  already did. As playback crosses a lap line, the lap it leaves drops its
  copies.
- **Streamed decode.** `TnrdPlayer` expanded every V6 history field to doubles,
  then built a second double table per family, then the samples. Fields now
  stay packed in the payload and are read in order during the merge, which
  hands each row straight to the sample builder.
- **Segment heap.** Lower settled memory on every page (6–54 MB); higher
  transient peaks on some page switches.

Found while checking lap views: Power's Fuel History was invisible in
playback lap windows (Previous, Current, ...). Its axis top fell back to the
first session-wide status sample, which lap windows don't install, giving a
0–1 kg axis. It now also considers the laps on screen.

Still held at the end of the race: chart points (~110 MB for all pages, every
sample by design), session data (~80 MB in playback), the reader's decoded
chunk cache (up to 64 MB) and ~86 MB taken when a recording is loaded, which
the engine's memory figures don't report yet.

Static instead of Dynamic vertex buffers: no gain on Vulkan, but on D3D11
(now the Windows default) Qt keeps a CPU copy of every Dynamic buffer and the
driver keeps more. End-of-race All Laps playback: Input page 1,002 → 698 MB,
peak 1,131 → 896 MB. Series vertex buffers are now Static.

### Still open

1. **Default backend: done.** D3D11 idles about 155 MB lower and ends a page
   tour about 140 MB lower than Vulkan on this machine, with the same
   rendering, so "auto" on Windows now tries D3D11, then Vulkan, then D3D12
   (`ChartGraphicsBackend.cpp`). Other platforms keep Vulkan first. Vulkan
   stays selectable in Settings.
2. **Issue 1, lap-owned storage: done.** Live samples are stored once, in
   their lap block, or in a loose block when they fall outside a lap (before
   the first lap, in the garage). Session-wide readers get a `SampleRange`
   (`SampleRange.h`) that chains the blocks in time order, with
   random-access iterators so `std::lower_bound` and index loops work
   unchanged. Playback keeps its separate buffers. The session model at
   17 minutes went from 19.4 MB to 10.7 MB.
3. **MSAA on visible charts.** 4x still costs 16 bytes/pixel per visible
   chart (~26–34 MB per chart page at 1080p). Edge antialiasing in the line
   shader would remove it, with some visual change on fills and discs.
4. **Engine (issue 8).** The recording writer's per-lap builders and the
   strategy rollback are the largest heap items, shared with Electron.

Each issue is labelled:

- **Confirmed in code:** the behaviour is certain from reading the source.
  How much it costs still needs measuring.
- **Hypothesis:** plausible from the code and from Qt or backend behaviour,
  but unverified.

## How Qt holds and draws telemetry

```text
engine threads (UDP, playback, live store)
  └─ EngineSink (EngineSink.h)
       JSON rows and binary batches appended to QByteArrays under a mutex,
       flushed to the GUI thread with one queued call per burst
GUI thread
  └─ MainWindow::onEngineRow / onEngineBinary (MainWindow.cpp)
       split the batch into rows (copies), scan for row types, parse:
       live: tnrp::parseRow (glaze) → tnrp::AnyRow
       playback: PlaybackPatchMerger::decode (QJsonObject merge)
  └─ SessionModel / SessionData (SessionModel.h/.cpp)
       rolling buffers: telBuf, stsBuf, tyreBuf, damageBuf, motionBuf,
       motionExBuf (QVector of float structs, up to 750,000 rows)
       laps: QVector<LapBlock>, every lap of the session; curLap
       playback: playbackCatalogLaps_, playbackLapDataCache_ (LRU of 6)
  └─ charts: TelemetryChart and other ChartView users (components/ChartView.cpp)
       CPU: one std::vector<Point> per series, Point = { double x; float y }
       GPU: one QRhiWidget per chart, QRhiBuffer::Dynamic vertex buffers
playback decode
  └─ TnrdPlayer worker lanes (std::thread), off the GUI thread
```

Sample structs (`SessionModel.h`): `TelSample` 28 B, `TyreSample` 68 B,
`StsSample` about 44 B, `DamageSample` 20 B, `MotionSample` 12 B,
`MotionExSample` 12 B, `LapProgressSample` 16 B.

## What does not apply from the Electron work

| Electron problem | Qt |
|---|---|
| Lap-progress map rebuilt every render | Not present. `SessionData::distanceAtTime` binary-searches `LapBlock::progress` directly |
| Charts missing restored history after a minimize | Not present. Qt's host is always visible, so no restore payloads |
| History decode on the UI thread | Not present. `TnrdPlayer` decodes on worker lanes |
| Base64 live laps | Not present. Qt builds its own laps and never calls `liveGetLapData` |
| Float64 for every field | Not present. Qt stores floats in fixed structs |
| GC ratchet (heap grows to the peak and stays) | Not applicable. C++ frees deterministically, though the allocator can still keep freed pages (see 6) |

## Issues

Ordered by expected size.

### 1. The live session is stored twice — confirmed in code

`SessionData::onTelemetry` and `onTyre` (`SessionModel.cpp`, around lines 90
and 136) append every sample to the rolling buffer **and** to `curLap`:

```cpp
telBuf.push_back({ t, speed, rpm, gear, throttle, brake, steering });
if (curLapNum >= 0) curLap.tel.push_back({ t, speed, rpm, gear, throttle, brake, steering });
```

`finalizeCurrentLap` then pushes `curLap` into `laps`, which keeps every lap
for the session. `curLap` is reset afterwards, so through implicit sharing
each lap has one owner. But the rolling buffers also hold the whole session in
live mode (up to 750,000 rows; `trim()` only acts beyond that). So every
family recorded both ways is held twice.

**Estimate.** Electron's runs on the same capture show about 60 rows/s for
the hot families and about 10/s for damage. One copy costs about:

60 × (28 + 68 + 44 + 12 + 12) + 10 × 20 ≈ 10 KB/s ≈ **0.6 MB/min**

Two copies are about 1.2 MB/min, or about 120 MB for a 100-minute race,
before `QVector` slack (issue 2).

**Measure:** split `retentionDiagnostics()`'s `active` estimate into rolling
buffers, completed laps and the current lap (see below), then watch them over
a long live session.

**Fix if confirmed:** keep each lap's samples only in its `LapBlock`.
Rolling-window and All Laps readers take a range of lap blocks and
binary-search the first one; the blocks are time-ordered, so iterating them
is a small adapter. Trims drop whole laps, and rewinds (`truncateAfter`) drop
the laps after the target and truncate one. This is the Qt equivalent of
Electron's chunk sharing.

### 2. QVector growth slack and front removal — confirmed in code

- **Slack:** `QVector` grows geometrically, so a large buffer can hold up to
  about twice its rows in capacity. Qt's estimate counts capacity, so this
  shows up as soon as it's measured.
- **Front removal:** `trim()` calls `rows.remove(0, n)`, which moves every
  remaining row, but only beyond 750,000 + 4,096 rows (about 3.5 hours at
  60 rows/s). Rarely reached.
- **Merges:** `mergeTimed` reserves `size + incoming` and builds a new merged
  vector, holding both copies at once during a playback merge.

**Measure:** capacity against size per buffer (add `samples` against
capacity-derived bytes to the split above).

**Fix:** mostly solved by issue 1's lap-owned storage. Laps are small,
bounded vectors, so the slack is bounded per lap.

### 3. Chart series in All Laps — confirmed in code; size is a hypothesis

`components/ChartView.cpp`:

- **Point size:** each series stores `std::vector<Point>` with
  `struct Point { double x; float y; }`, which pads to **16 bytes per point**.
- **X per series:** every series stores its own X, so Speed and RPM on the
  same chart hold the same X twice. Electron's charts keep one X column per
  chart, at 8 + 4 bytes per channel.
- **Cap:** each series caps at 750,000 points (`kMaxPoints`), and trimmed
  fronts are compacted only once `first >= 65536` and at least half the
  vector.
- **Full rebuilds:** `TelemetryChart::refresh` rebuilds a series in full on
  any mode change, rewind, or jump of more than 1 s
  (`std::abs(now - prevEndTime_) > 1.0f`). Off time axes it rebuilds through
  temporary `QVector<double>` arrays, then `setSeriesData` copies them into a
  new `std::vector<Point>`. In All Laps that's the full session, with up to
  three copies alive at once.

**Estimate.** For a 100-minute race in All Laps, the Speed/RPM/ERS chart holds
about 360,000 points × 3 series × 16 B ≈ **17 MB CPU**. A tyre chart has four
series per chart. A page with several such charts could reach tens of MB of
CPU series. Electron, for comparison, measured 7.3 MB CPU plus 6 MB GPU of
chart buffers, flat, in a 17-minute run.

**Measure:** `ChartView::retentionDiagnostics()` already reports `cpu_bytes`
and `gpu_buffer_bytes`. Log them per page in All Laps over a long playback,
and add a series count and total points.

**Fix options, if large:**
- One X column per chart, shared by its series.
- Store X as float relative to a chart origin, making `Point` 8 bytes.
- Draw All Laps from a per-pixel min/max outline instead of every sample.
  That was rejected in Electron because charts weren't its cost there; in Qt
  they might be.

### 4. GPU buffers per chart — hypothesis

- **Buffer type:** vertex buffers are `QRhiBuffer::Dynamic` and sized to the
  next power of two (`ensureGpu`, `capacity()` in `ChartView.cpp`).
  `ChartGraphicsBackend.cpp` tries Vulkan first, then D3D12, then D3D11. On
  Vulkan and Metal, a dynamic buffer is backed by one host-visible copy per
  frame in flight (two in Qt's Vulkan backend), so GPU-side bytes may be
  about twice `gpu_buffer_bytes`. D3D11 keeps one.
- **Per-widget resources:** each chart is its own `QRhiWidget`, with its own
  render target and swapchain-related resources. Pages with many charts
  multiply that.

**Measure:**
- `QRhi::statistics()`: on Vulkan it reports the memory allocator's totals
  (`totalPipelineCreationTime`, `blockCount`, `allocCount`, `usedBytes`,
  `unusedBytes`).
- the GPU process or driver figures in Task Manager's GPU memory columns
  (dedicated and shared)
- the same session on Vulkan, D3D12 and D3D11

**Fix options:** static or immutable vertex buffers for history that doesn't
change, with a small dynamic tail. Fewer `QRhiWidget`s on heavy pages (one
widget drawing several panels).

### 5. GUI-thread row parsing — confirmed in code; cost unknown

`MainWindow::onEngineRow`:

- splits each burst into rows with `json.mid()`, one `QByteArray` copy per row
- runs up to four `json.contains("\"type\":...")` scans per row, each a full
  pass over the row (all-car rows are large)
- parses with `tnrp::parseRow` (glaze) into the `tnrp::AnyRow` variant, live
- in playback, decodes through `PlaybackPatchMerger`, which merges V6 patches
  as `QJsonObject`s. The code's own comment notes this used to re-serialise
  the 22-car state per patch and froze Standings; it's mitigated by folding
  superseded patches within a burst.

Electron's engine log showed about 2 MB/s of JSON and 628,000 rows in 50
minutes delivered to its renderer. Qt receives the same engine output.

**Measure:** CPU and allocation profiling of the GUI thread during a 1x
replay (tools below). Look at time in `onEngineRow`, `parseRow`,
`PlaybackPatchMerger::decode` and `QJsonDocument`, and at allocation counts.

**Fix options, only if it shows up:**
- dispatch on the row's type field once, instead of repeated `contains`
- parse rows in place with `string_view` instead of `mid()` copies
- move parsing to the sink's thread and hand typed rows to the GUI
- replace the `QJsonObject` merge with typed patches

### 6. Allocator retention after large transient work — hypothesis

C++ frees deterministically, but the heap allocator (the Windows segment heap
or NT heap, glibc malloc on Linux) may keep freed pages committed. Large
temporary allocations (full-session chart rebuilds, playback merges,
`decodeHistory` batches) can leave process private memory high after the work
ends, which looks like the ratchet Electron had.

**Measure:** compare private memory against the session plus chart estimate
before and after a large operation (All Laps entry, a seek, a driver switch).
The gap that stays after the work ends is retention. On Linux,
`malloc_trim(0)` after the operation tests the theory.

**Fix options:** avoid the large temporaries (issues 1 and 3). Reserve and
reuse scratch buffers for rebuilds.

### 7. Diagnostics gaps — confirmed in code

Qt already writes an Electron-compatible `ram_usage.log` (`Diagnostics.cpp`,
one sample a second, process private bytes from `GetProcessMemoryInfo`). The
dev-tools RAM viewer reads it. What's missing:

| Gap | Effect |
|---|---|
| `active` session retention is one total | Issue 1 can't be seen directly |
| `gpu_buffer_bytes` vs Electron's `gpu_texture_bytes` | The viewer's "Chart buffers · GPU" row is empty for Qt |
| No per-chart or per-series breakdown | Can't tell which page or chart costs what |
| Implicit sharing not deduplicated | A `QVector` shared between, for example, the playback catalog and the lap cache is counted twice (`addVectorRetention` uses `capacity()` per holder). Check with `QVector::isDetached()` or by comparing `constData()` pointers |
| No allocation-rate figure | Churn (issue 5) is invisible in the log |

### 8. Engine-side memory — shared with Electron, measured there

Qt hosts the engine in its own process, so the engine figures from the
Electron logs apply to Qt's process directly:

- **Live V6 store:** grows about 1 MB/min compressed, by design.
- **Strategy rollback:** about 32 MB, at its cap. It always runs (wanted
  behaviour).
- **Parser:** about 8 MB/s of short-lived result-buffer allocations.
- **JSON:** produced for every hot family.

These are Electron's deferred Phase D items: reuse parser buffers, drop
committed chunks while a recording file holds them, and feed the writer
structs. Fixing them helps both frontends.

## How to measure

### Running the UDP replay

The replay tool lives in `tools/TNR stuff/udp sender and recorder/`:

| File | What it is |
|---|---|
| `play_udp.py` | Sends a recorded capture to `127.0.0.1:20777` at the recorded 60 Hz frame rate |
| `record_udp.py` | Records UDP from port 20777 into `udp_capture.bin`, overwriting it |
| `udp_capture.bin` | The São Paulo race capture (2.5 GB, 92 minutes) that `play_udp.py` sends by default |
| `.venv` | The Python environment both scripts run in |

1. **Start the app first,** so its UDP listener is bound to port 20777.
   Check from PowerShell; the owning process should be the app:

   ```powershell
   Get-NetUDPEndpoint -LocalPort 20777
   ```

2. **Start the replay** at normal speed:

   ```powershell
   cd "D:\Personal Code\Track-N-Race\tools\TNR stuff\udp sender and recorder"
   .\.venv\Scripts\python.exe -u play_udp.py
   ```

   It prints `Playing ... at 1x speed (60 recorded game frames/second)` and
   runs until the capture ends or you press Ctrl+C. The first few minutes are
   the pre-race and formation lap; after that a lap takes about 72–75
   seconds, so 10 laps is about 15–17 minutes from the start.

3. **Never use `--xs`.** It replays at 16x, and the app can't keep up, so the
   numbers are meaningless.

4. **Run exactly one sender.** Each sender is two processes (the venv
   launcher and Python). A forgotten sender from an earlier run interleaves a
   second timeline of the same session, and the run becomes invalid: laps
   jump and memory goes up. Before every run, stop all of them:

   ```powershell
   Get-CimInstance Win32_Process |
     Where-Object { $_.CommandLine -like '*play_udp.py*' } |
     ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
   ```

5. **Restart the replay with the app,** so each measurement starts at the
   beginning of the session. There's no seek in `play_udp.py`; it always
   starts at the first packet.

6. **Recording a new capture** (optional): run `record_udp.py` the same way
   while the game sends telemetry to port 20777; stop it with Ctrl+C. Note
   that it overwrites `udp_capture.bin`. `udp_capture_sao_paulo.bin` beside it
   has the same size and the same first megabyte, so it looks like a backup
   of the São Paulo capture; keep it before recording over the default file.

### Setup

1. **Data:** the 1x UDP replay above.
2. **Memory log:** Settings ▸ Memory log. It writes
   `%LOCALAPPDATA%/<app>/launch-diagnostics/ram_usage.log`
   (`QStandardPaths::AppLocalDataLocation`), which is wiped on every launch.
   Copy it before relaunching.
3. **Reading:** open it in the dev-tools RAM viewer
   (`Track-N-Race-utils/track-n-race-dev-tools`). Process totals and
   `telemetry_data` work today; GPU needs the key fix above.
4. **Playback:** `tools/udp_capture_to_tnrd/udp_capture_sao_paulo_v6_2026-10-07.tnrd`
   (92 minutes, opens on the current format).

### Scenarios

| Scenario | Shows |
|---|---|
| Live, Current lap, 10 laps, page switches | Baseline; issue 1 slope; GUI-thread cost |
| Live, All Laps, 10 laps | Chart series growth (3), GPU (4) |
| Live, 60–100 minutes (or the full capture) | Long-run slope of 1 and 2; engine growth (8) |
| Playback: All Laps after a seek to 60%, a driver switch, switch back | Merges (2), rebuilds (3), retention after large work (6) |
| The same on Vulkan, D3D12 and D3D11 | Backend GPU cost (4) |

### Small instrumentation to add first

All of these go in the memory log only, with no behaviour change:

1. **Split the session estimate** in `SessionModel::retentionDiagnostics()`:
   `rolling_buffers`, `completed_laps`, `current_lap`, each with `samples`
   and capacity `bytes`.
2. **Per-chart totals** in `ChartView::retentionDiagnostics()`: series count,
   points, CPU bytes and GPU bytes per `ChartView` instance, with its page and
   section name.
3. **`gpu_texture_bytes`:** also report the GPU figure under this key (or
   teach the viewer `gpu_buffer_bytes`).
4. **QRhi statistics:** add `QRhi::statistics()` per backend to
   `main_runtime_memory`.
5. **Unattributed remainder:** process private bytes minus the session, chart
   and engine estimates.

### Tools for CPU and allocations

| Platform | Tool | Use |
|---|---|---|
| Windows | Visual Studio Performance Profiler (CPU Usage, .NET/Native Memory) or Windows Performance Recorder + WPA (heap tracing) | GUI-thread time per function; allocation counts and sizes |
| Linux | `heaptrack`, `perf` | Allocation call stacks and peak; CPU |
| macOS | Instruments (Allocations, Time Profiler) | The same |
| Qt | `QT_LOGGING_RULES="qt.rhi.*=true"`, `QRhi::statistics()` | GPU memory per backend |

Profile 2–3 minutes of steady 1x replay, not the start-up burst, and the
playback operations from the scenario table separately.

## Where to focus

1. **Add the instrumentation** above. It's small and decides everything
   else.
2. **Issue 1, the double session.** It's certain from the code, and the
   estimate is about 0.6 MB/min of avoidable memory. Confirm the slope with
   the split estimate, then move to lap-owned storage. Biggest likely win for
   live use.
3. **Issue 3, chart series in All Laps.** If per-chart CPU bytes grow with the
   session as estimated, start with a shared X per chart and 8-byte points;
   consider outlines only if that isn't enough.
4. **Issue 4, GPU backend.** Compare backends with `QRhi::statistics()`. If
   Vulkan doubles chart memory, either move history to static buffers or
   prefer a backend.
5. **Issue 5, GUI-thread parsing.** Only if profiling shows it in the top
   functions or the UI stutters.
6. **Issue 8, engine Phase D.** Shared with Electron, so do it once in the
   library.

## Code references

| Area | Location |
|---|---|
| Sample structs, `SessionData`, `LapBlock` | `qt_frontend/src/SessionModel.h` |
| Double append, `finalizeCurrentLap`, `trim`, `mergeTimed`, retention estimate | `qt_frontend/src/SessionModel.cpp` |
| Chart series storage, `kMaxPoints`, `compact`, GPU buffers | `qt_frontend/src/components/ChartView.cpp` |
| Rebuild conditions and full-session rebuilds | `qt_frontend/src/TelemetryChart.cpp` (`refresh`) |
| Backend order | `qt_frontend/src/ChartGraphicsBackend.cpp` |
| Row delivery and parsing | `qt_frontend/src/EngineSink.h`, `MainWindow::onEngineRow` |
| Playback patch merge | `qt_frontend/src/PlaybackPatchMerger.cpp` |
| Memory log | `qt_frontend/src/Diagnostics.cpp`, `MainWindow::memoryDiagnosticsSnapshot` |
