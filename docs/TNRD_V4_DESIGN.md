# TNRD V4 — Indexed Chunked Telemetry Container

**Repository:** `NoGoat/Track-N-Race`  
**Status:** Implemented format contract (2026-08-11)  
**Scope:** Shared `libtnrp` recording/playback format and the minimal host/API changes required to exploit it  
**Primary goal:** Eliminate full-file decompression and large temporary files while preserving all current Track N Race behavior.

---

## 1. Summary

TNRD V4 replaces the current "one compressed JSONL stream" storage model with an **uncompressed container header and directory plus independently Zstandard-compressed telemetry chunks**.

The core storage key is:

```text
(lap, packet/row type) -> compressed chunk
```

A lap is the primary time boundary. Within each lap, each stored row family is independently compressed. For example:

```text
Lap 17 / telemetry
Lap 17 / motion
Lap 17 / motion_ex
Lap 17 / status
Lap 17 / damage
Lap 17 / lap
Lap 17 / positions
...
```

The top-level V4 control structures remain uncompressed:

```text
TNRD V4
├── fixed header                  uncompressed
├── session metadata              uncompressed
├── lap table                     uncompressed
├── chunk directory               uncompressed
└── chunk payloads
    ├── lap 1 / telemetry         ZSTD frame
    ├── lap 1 / motion            ZSTD frame
    ├── lap 1 / status            ZSTD frame
    ├── ...
    ├── lap 2 / telemetry         ZSTD frame
    └── ...
```

This gives the reader immediate knowledge of:

- recording format and session metadata;
- every lap number;
- every lap start and end time;
- recorded lap time;
- which packet/row families exist for each lap;
- the exact byte offset and compressed size of each chunk;
- the uncompressed size required for allocation;
- integrity information for every chunk.

No ZSTD work is required merely to open a file and obtain its session skeleton.

TNRD V4 is **not** intended to change the logical row schemas, playback semantics, frontend page behavior, Analyze semantics, damage reconstruction, seek behavior, or XLSX contents. It is a storage and retrieval redesign underneath the existing engine.

---

## 2. Why V4 Exists

The current V1/V2/V3 reader decompresses the entire recording into an OS temporary JSONL file before building its playback index.

For long races the decompressed form can reach multiple gigabytes. A full race around 5 GiB uncompressed means loading a recording temporarily requires roughly that amount of disk space even though most user interactions need only a fraction of the recording at any one time.

This creates several avoidable costs:

1. large temporary-disk usage;
2. full-session decompression before playback becomes usable;
3. load latency proportional to the entire recording;
4. duplicate storage while the compressed source and uncompressed temp both exist;
5. expensive flashback rewrites when already-flushed data must be truncated;
6. inability to exploit the fact that individual UI surfaces consume only specific row families.

V4 makes the compressed file itself randomly addressable.

---

## 3. Compatibility Requirements

V4 is successful only if it does **not** regress existing Track N Race behavior.

The following are hard requirements.

### 3.1 Existing formats remain readable

`TnrdReader` continues to load:

- TNRD V1 / gzip;
- TNRD V2 / Zstandard;
- TNRD V3 / Zstandard;
- TNRD V4 / chunked Zstandard container.

V1–V3 continue using the existing legacy reader path, including temp-file decompression. V4 uses the new native indexed-container path.

Normal new recording should move to V4 once the implementation is considered stable.

The deprecated explicit V1/gzip writer remains available until separately removed.

### 3.2 Engine API behavior remains stable

The existing `Engine` playback interface should keep its public semantics:

```cpp
playerLoad(...)
playerPlay()
playerPause()
playerSeek(...)
playerSetSpeed(...)
playerGetLapData(...)
playerGetAllLapsData(...)
playerClose()
```

V4 changes what `TnrdReader` does behind these calls, not what the rest of the application expects those calls to mean.

### 3.3 Sink behavior remains stable

The existing `Sink` channels remain valid:

- `onRow(...)`
- `onBinary(...)`
- `onSeekFlush(...)`

A V4 reader may produce their contents from selected chunks rather than from a monolithic decompressed stream, but downstream consumers should receive equivalent logical data.

### 3.4 Electron behavior remains stable

Existing Electron behavior that must remain intact includes:

- `playback_loaded`;
- recorded-format `protocol_status`;
- `playback_lap_blocks`;
- normal binary playback;
- sparse-row duplicate cache behavior;
- seek flushes;
- positions restore on seek;
- `playback_state`;
- `playback_finished`;
- `playback_close`;
- Analyze per-lap payloads;
- PL/FL/RL comparison modes;
- AL mode;
- hidden-window resume behavior;
- existing chart rendering and Zustand publication semantics.

### 3.5 Qt behavior remains stable

Qt remains a first-class consumer of the same TNRD format.

The "one engine, two hosts" rule remains intact. Format parsing, indexing, chunk decompression, and compatibility logic live in `libtnrp`, not separately in Electron or Qt.

### 3.6 XLSX export remains complete

`exportTnrdFileToXlsx()` must continue to export the same logical rows and sheets as before.

For V4 it should iterate the chunk directory and decompress chunks incrementally instead of requiring a complete temp JSONL file.

### 3.7 Damage reconstruction remains unchanged

The current reader reconstructs deduplicated Car Damage state at the specification's 10 Hz cadence for playback, seeks, and Analyze.

V4 must continue to expose the same reconstructed behavior. The storage layout must not accidentally turn deduplicated raw damage rows into lower-frequency visible playback.

### 3.8 Sparse state re-emission remains unchanged

The existing playback duplicate cache for sparse panel rows remains an Engine behavior.

V4 should provide efficient "latest row at or before time T" queries so the Engine can preserve current re-emission behavior without scanning unrelated chunks.

---

## 4. Design Principles

### 4.1 Uncompressed control plane, compressed data plane

Everything required to discover the file is uncompressed.

Everything large is compressed.

Opening the file should require ordinary small reads and zero decompression.

### 4.2 Lap is the time partition

Every regular telemetry chunk belongs to one lap.

The lap table is authoritative for:

- `lap_num`;
- lap start session time;
- lap end session time;
- recorded lap time.

Playback must not have to reconstruct lap boundaries by rescanning lap rows every time a V4 file is opened.

### 4.3 Row family is the data partition

Within a lap, row families are independently compressed.

This allows the runtime to load only the packet families required by active consumers.

Examples:

```text
Speed/RPM/Throttle/Gear -> telemetry
G-force                  -> motion
ride-height data         -> motion_ex
tyre/fuel state history  -> status
damage history           -> damage
track/player position    -> positions
lap-distance mapping     -> lap
```

The exact dependency map remains application-owned and can evolve without changing the V4 file format.

### 4.4 Logical schemas remain the source of truth

V4 should not create a second semantic telemetry schema.

The simplest first implementation stores the same serialized row representation currently recorded, grouped into chunks.

Every stored row must remain independently time-addressable after rows are
partitioned by family. For logical rows such as `positions` that do not declare
`session_time`, the V4 writer adds the packet's external session timestamp as an
additive top-level storage field. Readers ignore unknown fields when decoding
the logical row schema. This prevents family partitioning from losing the
chronology that V1–V3 inherited from adjacent timed rows in the JSONL stream.

That minimizes migration risk:

```text
V3:
JSON row
JSON row
JSON row
...

V4:
chunk(lap=17,type=telemetry) = ZSTD(JSONL rows of telemetry)
chunk(lap=17,type=status)    = ZSTD(JSONL rows of status)
...
```

A later V5 can change the payload encoding if profiling proves JSONL inside chunks is still a major bottleneck. V4 should solve the storage architecture first.

### 4.5 The directory is authoritative

Readers must not discover chunk locations by walking ZSTD frames.

Every committed chunk has a directory entry.

The directory is the canonical map from logical data to physical bytes.

---

## 5. Proposed On-Disk Layout

A finalized V4 file is:

```text
+-------------------------------+
| Fixed V4 Header               |
+-------------------------------+
| Session Metadata              |
+-------------------------------+
| Lap Table                     |
+-------------------------------+
| Chunk Directory               |
+-------------------------------+
| Chunk Payload 0 (ZSTD)        |
+-------------------------------+
| Chunk Payload 1 (ZSTD)        |
+-------------------------------+
| ...                           |
+-------------------------------+
| Chunk Payload N (ZSTD)        |
+-------------------------------+
```

The layout should support two implementation strategies:

1. tables at the beginning with offsets known/finalized on close; or
2. payloads appended first, tables appended at the end, with the fixed header patched on finalization.

**Recommended:** payloads first, authoritative tables at the end, fixed header patched at close.

That fits streaming recording because the writer does not know final lap count, chunk count, or offsets in advance.

Conceptually:

```text
+-------------------------------+
| Fixed V4 Header               |
| directoryOffset = patched     |
| lapTableOffset  = patched     |
+-------------------------------+
| Session Metadata              |
+-------------------------------+
| ZSTD Chunk                    |
| ZSTD Chunk                    |
| ZSTD Chunk                    |
| ...                           |
+-------------------------------+
| Lap Table                     |
+-------------------------------+
| Chunk Directory               |
+-------------------------------+
| Footer / commit marker        |
+-------------------------------+
```

The fixed header remains tiny and uncompressed.

---

## 6. Fixed Header

The exact packing can be decided during implementation, but the header should be fixed-size, explicitly little-endian, versioned, and extensible.

Illustrative structure:

```cpp
struct TnrdV4Header {
    char     magic[8];             // e.g. "TNRD_V4"
    uint16_t containerVersion;     // 4
    uint16_t headerSize;
    uint32_t flags;

    uint64_t metadataOffset;
    uint64_t metadataSize;

    uint64_t lapTableOffset;
    uint32_t lapCount;
    uint32_t lapEntrySize;

    uint64_t chunkTableOffset;
    uint32_t chunkCount;
    uint32_t chunkEntrySize;

    uint64_t footerOffset;

    uint32_t headerCrc32;
    uint32_t reserved0;
};
```

Requirements:

- never depend on compiler-native struct packing when writing;
- use explicit serialization helpers;
- use fixed-width integer types;
- use 64-bit offsets everywhere;
- validate all offsets and sizes against actual file size before use;
- reject overlapping or out-of-range tables/chunks;
- permit newer readers to add optional fields through `headerSize`/flags;
- permit older V4 readers to reject unknown mandatory flags cleanly.

---

## 7. Session Metadata

Session metadata remains uncompressed because it is small and is required immediately.

It should carry the logical information currently represented by `HeaderRow`, including:

- protocol/game format;
- track id;
- track name;
- track length;
- session type;
- session name;
- recording wall-clock start time;
- future small session-level metadata.

Two reasonable encodings are possible:

1. current JSON `HeaderRow`;
2. fixed/length-prefixed binary fields.

**Recommended for V4:** retain a length-prefixed JSON metadata payload using the existing `HeaderRow` schema.

Reason:

- it avoids duplicating header semantics;
- it keeps the current glaze serialization;
- metadata is tiny, so binary optimization has no meaningful payoff;
- future optional metadata remains easy to add.

The binary fixed header describes the container. The JSON session metadata describes the recording.

---

## 8. Lap Table

The lap table eliminates repeated lap-boundary reconstruction.

Illustrative entry:

```cpp
struct LapEntryV4 {
    uint32_t lapNumber;

    float    startSessionTime;
    float    endSessionTime;

    uint32_t lapTimeMs;

    uint32_t flags;
};
```

`double` may be used instead of `float` if desired, but the table should be consistent with the effective precision of recorded `session_time`.

Recommended flags include:

```text
VALID_LAP_TIME
INCOMPLETE_LAP
FLASHBACK_REWRITTEN
HAS_DISTANCE_DATA
```

The lap table is used immediately by:

- playback lap selection;
- seek-to-lap;
- Analyze lap catalog;
- previous-lap lookup;
- fastest-lap lookup;
- reference-lap selection;
- AL tick boundaries;
- `currentLapAt(t)`;
- session progress;
- lap duration calculations.

### 8.1 Lap end semantics

`endSessionTime` is the logical boundary at which the next lap begins, or the recording end for an incomplete final lap.

It is not a replacement for `lapTimeMs`.

`lapTimeMs` remains the game's recorded completed-lap time when available.

This distinction matters because timeline boundary duration and authoritative game lap time are not guaranteed to be identical.

---

## 9. Chunk Directory

The chunk directory maps a logical chunk key to a physical ZSTD frame.

Illustrative entry:

```cpp
struct ChunkEntryV4 {
    uint32_t lapNumber;
    uint16_t rowType;
    uint16_t flags;

    uint64_t offset;
    uint64_t compressedSize;
    uint64_t uncompressedSize;

    uint32_t rowCount;
    uint32_t checksum;
    uint64_t segmentSequence;
};
```

The primary key is:

```text
(lapNumber, rowType, segmentSequence)
```

Multiple independently compressed delta segments may exist for one lap/row
family. `segmentSequence` is monotonic within a recording and makes every
checkpoint append-only; readers merge the segments chronologically. A newer
checkpoint directory may omit superseded segments after a flashback.

The directory may be physically sorted by:

```text
lapNumber ASC, rowType ASC
```

for simple binary search and good locality.

A reader may also build a tiny in-memory hash/index on load.

### 9.1 Row type IDs

Preserve the existing TnrdReader type IDs unless there is a compelling reason not to:

```text
0  other
1  telemetry
2  status
3  damage
4  lap
5  session
6  race_event
7  timing
8  participants
9  all_status
10 tyre_sets
11 motion
12 motion_ex
13 positions
```

Reusing these IDs prevents the storage layer from inventing another mapping and reduces risk around Engine caches and existing semantics.

### 9.2 Session-scoped rows

Not every row naturally belongs to one lap.

For session/global rows, reserve a special logical lap id:

```text
lapNumber = 0
```

or an explicit `scope` field.

Recommended:

```cpp
enum class ChunkScope : uint8_t {
    Session = 0,
    Lap = 1,
};
```

This makes global metadata/events explicit and avoids pretending every row has a meaningful lap.

Likely session-scoped candidates include some:

- participants;
- race events;
- session state;
- tyre set metadata;
- other rows whose semantics are not naturally lap-local.

However, state rows needed for time-based playback may still be stored in lap-scoped chunks when their `session_time` belongs to a lap. The division should preserve chronological behavior, not merely follow message names.

---

## 10. Chunk Payload Encoding

### 10.1 V4 first implementation

Use one independent ZSTD frame per chunk.

The uncompressed content of a chunk is newline-delimited rows of exactly one row type:

```text
{"type":"telemetry",...}\n
{"type":"telemetry",...}\n
{"type":"telemetry",...}\n
...
```

Advantages:

- reuses existing serializers/parsers;
- no row schema migration;
- trivial XLSX reuse;
- straightforward compatibility tests against V3;
- easy corruption inspection;
- no frontend schema change;
- isolates V4 work to container/indexing rather than every telemetry structure.

### 10.2 Compression context

Each chunk is independently decompressible.

Do not require the previous chunk's ZSTD state.

A shared ZSTD dictionary may be evaluated later, but V4 correctness must not depend on one.

### 10.3 Chunk size floor

Do not split below `(lap, rowType)` in V4.

In particular, do not initially split by:

- chart;
- UI card;
- individual field;
- car;
- arbitrary small time slice.

That would increase directory size, hurt compression ratio, create excessive tiny frame overhead, and tightly couple the file format to current UI organization.

`lap + row family` is the intended granularity.

---

## 11. Recording Architecture

The existing TnrdWriter owns:

- session rotation;
- a dedicated disk thread;
- flashback handling;
- state-row deduplication;
- recoverability;
- recording errors.

V4 should preserve those responsibilities.

### 11.1 New writer working state

Instead of one rolling JSONL stream, the writer maintains **per-row-type builders for the currently mutable timeline window**.

Conceptually:

```cpp
struct MutableLap {
    uint32_t lapNumber;
    float startSessionTime;

    std::unordered_map<RowType, ChunkBuilder> chunks;
};
```

A `ChunkBuilder` contains rows that have not yet been committed to an immutable compressed chunk.

The writer still receives rows in chronological arrival order through its existing queued `Record` event.

### 11.2 Determining lap ownership

Lap rows establish boundaries.

Rows are assigned to the current lap using the writer's current lap state.

Boundary rules must be deterministic for rows arriving exactly at rollover:

- define whether `session_time == nextLapStart` belongs to the new lap;
- use the same rule everywhere;
- test this explicitly.

Recommended interval:

```text
lap N: [startN, startN+1)
```

The final lap ends at recording end.

### 11.3 Commit timing

A lap should not necessarily be written the instant the next lap starts because the game supports flashbacks.

The existing writer retains a 30-second mutable window.

V4 should preserve this property.

Recommended behavior:

1. current lap remains mutable;
2. immediately previous lap may remain mutable while its tail is still within the flashback window;
3. a lap/type chunk becomes immutable only once its entire timeline lies older than the 30-second flashback horizon;
4. immutable chunks are ZSTD-compressed and appended to the file;
5. their directory entries are retained in memory until file finalization.

This avoids rewriting chunks during ordinary <=30 s flashbacks.

### 11.4 Flashback inside the mutable horizon

If session time reverses within retained mutable data:

- discard buffered rows newer than the new session time;
- update lap boundary working state;
- discard any not-yet-committed future lap builders;
- reset relevant dedupe state exactly as current semantics require;
- continue recording.

No on-disk rewrite is necessary.

### 11.5 Flashback behind committed data

This is the important edge case.

Current V3 behavior may decompress and rewrite the whole compressed file when a flashback jumps behind the in-memory rolling buffer.

V4 must not require full-file decompression.

Use an append-only supersession model:

1. locate affected committed chunks using lap table/chunk metadata;
2. mark affected directory entries as superseded in writer memory;
3. create replacement chunks containing only rows valid up to the flashback point;
4. discard logically future laps/chunks from the final directory;
5. append replacements/new timeline chunks;
6. final directory references only the surviving/replacement chunks.

Old superseded compressed bytes may remain physically in the file but become unreachable from the finalized directory.

This is effectively copy-on-write.

Benefits:

- no multi-gigabyte rewrite;
- no decompression of unrelated laps/types;
- flashback cost is proportional to affected chunks;
- final logical playback remains clean.

Optional future compaction can remove unreachable bytes, but it is not required for correctness.

### 11.6 State deduplication

Preserve the current `dedupeTypes()` behavior.

Do not alter which types are deduplicated as part of V4.

Chunking changes storage placement, not recording semantics.

### 11.7 Recoverability while recording

V4 must retain crash tolerance.

A finalized V4 directory cannot be assumed to exist after a crash, so the writer needs recoverability checkpoints.

Recommended design:

- every committed chunk has a small self-describing chunk prefix;
- periodically append a **checkpoint directory/footer**;
- checkpoint contains all currently valid committed lap/chunk entries;
- flush file data after checkpoint;
- fixed header may point to the latest checkpoint, or recovery may scan backward for a footer magic.

On normal close:

- commit all mutable chunks;
- append final lap table;
- append final chunk directory;
- append final footer;
- patch the fixed header to the final table offsets;
- flush/sync according to existing durability policy.

This preserves the spirit of the existing ~5-second recoverable flush without relying on a single streaming codec frame.

---

## 12. Reader Architecture

`TnrdReader::load()` becomes a format dispatch:

```text
V1/V2/V3
    -> existing legacy decompression/index path

V4
    -> read header
    -> read metadata
    -> read lap table
    -> read chunk directory
    -> validate
    -> ready
```

No V4 temp file is created.

### 12.1 Load-time work for V4

V4 load should be approximately:

```text
open source file
read fixed header
validate magic/version
read session metadata
read lap table
read chunk table
build tiny in-memory lookup structures
derive fastest lap from lap table
emit playback metadata
```

Avoid decompressing telemetry merely to populate `playback_lap_blocks`.

Any slim load-time lap information that is required immediately should come from:

- lap table; or
- a small explicit metadata structure.

Do not force all telemetry chunks to decode at load just to preserve an old optimization payload.

### 12.2 Reader cache

Add a bounded decompressed-chunk cache.

Key:

```text
(lap, rowType)
```

Value:

- decompressed bytes or parsed/pre-encoded representation;
- byte size;
- last-used generation.

Use an LRU or equivalent byte-budgeted cache rather than an unbounded "number of chunks" cache.

This cache is internal to `TnrdReader`.

### 12.3 Chunk read primitive

Introduce an internal primitive roughly equivalent to:

```cpp
ChunkView readChunk(uint32_t lap, uint16_t rowType);
```

It:

1. finds the directory entry;
2. validates bounds;
3. reads exactly `compressedSize` bytes;
4. decompresses exactly one ZSTD frame;
5. validates `uncompressedSize`;
6. validates checksum;
7. returns/caches the result.

No temp file and no unrelated rows are touched.

---

## 13. Packet/Row Dependency Loading

The major V4 optimization is selective row-family decoding.

### 13.1 Consumer dependencies

Every visible chart/card that needs history declares the row families it requires.

Examples:

```text
Speed/RPM        -> telemetry
Throttle/Brake   -> telemetry
Gear             -> telemetry
G-force          -> motion
Ride Heights     -> motion_ex
Tyre history     -> status
Damage history   -> damage
Track positions  -> positions
lap distance     -> lap
```

The frontend/runtime computes the **union** of dependencies.

If three visible charts all require telemetry:

```text
required = { telemetry }
```

not three independent telemetry requests.

### 13.2 Keep storage dependencies separate from visual components

The file format must know row types, not chart names.

Do not put chart IDs into TNRD.

UI-to-row dependency mapping belongs in the application layer.

This keeps TNRD stable when cards are renamed, reorganized, or replaced.

### 13.3 Initial integration strategy

To avoid stepping on existing behavior, introduce selective loading in phases.

**Phase 1:** V4 storage/read architecture, but preserve the existing broad playback stream requests.

**Phase 2:** add optional row-type masks to V4 history requests.

**Phase 3:** wire visible chart/card dependency aggregation into Electron and Qt.

This lets V4 ship without requiring the entire UI dependency system to land atomically.

---

## 14. Normal Playback

Playback still advances one absolute `session_time` cursor.

For current-time streaming, `TnrdReader` needs to provide rows in chronological order even though data is physically separated by row family.

### 14.1 Per-lap merge

For the current lap, load only row types required for active playback plus mandatory state types needed to preserve existing panels.

Each loaded chunk contains rows sorted by session time.

Merge the active chunk iterators by `session_time` using a small min-heap:

```text
telemetry iterator ─┐
motion iterator    ─┤
status iterator    ─┼─> chronological playback merge
damage iterator    ─┤
lap iterator       ─┘
```

This reconstructs the existing logical row stream without reconstructing a giant file.

### 14.2 Crossing a lap boundary

When playback enters the next lap:

- release/retain previous chunks according to cache policy;
- load required chunks for the new lap;
- initialize iterators;
- continue the merged stream.

Optional one-lap lookahead may prefetch required chunks.

### 14.3 Sparse duplicate cache

Keep `Engine`'s current duplicate-cache semantics.

The V4 reader should implement `latestOfTypesTagged(t, types)` by:

1. locating the lap containing `t`;
2. checking the relevant row-type chunk;
3. if no suitable row exists, walking previous laps for that row type only;
4. returning the latest row at/before `t`.

This is vastly cheaper than scanning unrelated telemetry.

---

## 15. Seek

Current seek semantics must remain unchanged.

### 15.1 Determine target lap from lap table

`currentLapAt(target)` becomes a binary search over the V4 lap table.

No scan of lap rows is required.

### 15.2 Normal seek history

The existing Electron binary seek path restores approximately:

```text
[min(target - 600 s, currentLapStart), target]
```

plus required cold rows/state.

For V4:

1. use lap table to find all laps intersecting the requested time range;
2. decode only required row-type chunks from those laps;
3. filter rows to exact time bounds;
4. build the same `SeekFlush` logical result;
5. preserve current state snapshot and positions restore behavior.

### 15.3 AL seek/history

AL currently requests complete history up to the current playhead.

In V4, AL requests:

```text
[startTime, target]
```

but only for row families required by visible AL charts/cards.

The user selecting AL does not imply every stored row family must be decoded.

---

## 16. AL Mode

AL is the main reason V4 should partition by both lap and row family.

Every chart can operate in AL mode, but a given set of visible charts generally consumes only a subset of all row families.

Example:

```text
Visible:
- Speed/RPM
- Throttle
- Gear

Required:
- telemetry
```

AL needs all laps for `telemetry`, but does not need:

```text
motion
motion_ex
damage
positions
...
```

Another layout:

```text
Visible:
- Speed/RPM
- G Force
- Tyre Temps

Required:
- telemetry
- motion
- status
```

The V4 reader loads:

```text
for every relevant lap:
    telemetry chunk
    motion chunk
    status chunk
```

and skips everything else.

### 16.1 Progressive AL publication

V4 makes progressive AL loading possible.

Decode lap chunks in order and publish batches incrementally.

The existing frontend may initially keep its current "wait for all-history response" behavior for compatibility. Progressive publication can be introduced later as a performance improvement.

### 16.2 Parallel decompression

Independent chunks may be decompressed by a small worker pool.

Do not spawn one thread per chunk.

A bounded pool of a few workers is sufficient and prevents decompression from monopolizing the system.

Parallel loading is an optimization, not a format requirement.

---

## 17. Analyze / PL / FL / RL

These modes map naturally to V4.

### 17.1 Analyze

`playerGetLapData(lapNum)` loads the packet families required to produce the existing `playback_lap_data` payload for that lap.

It must continue returning:

- telemetry;
- motion;
- motionEx;
- status history;
- damage history;
- lap progress;
- player positions;
- start/end times.

The existing frontend cache of up to six Analyze laps can remain unchanged.

### 17.2 PL

Load:

```text
current lap required chunks
previous lap Analyze/comparison chunks
```

### 17.3 FL

Fastest lap number comes directly from the lap table's valid `lapTimeMs` values.

Load the corresponding comparison chunks on demand.

### 17.4 RL

Reference lap options come directly from the lap table.

Load the selected reference lap on demand.

### 17.5 Distance support

V3 introduced lap distance support used for Analyze alignment.

V4 must preserve it.

The lap row chunk remains the source of time/distance progress points unless a future explicit compact progress index is added.

V4 recordings should report equivalent or stronger capability than V3 for distance-based Analyze modes.

Do not gate V4 out by conditions that currently check exactly for `TNRD_V3`; update capability checks to "format supports lap distance" instead of equality with one version string.

This is one of the few frontend compatibility changes that is required.

---

## 18. `playback_lap_blocks` Compatibility

Today load-time playback metadata includes slim per-lap chart/status summaries built during the V1–V3 index scan.

A V4 file should avoid decompressing all telemetry just to regenerate that payload.

There are two safe options.

### Option A — extend lap/session metadata

Store the small values required for `playback_lap_blocks` directly in an uncompressed or tiny compressed summary section.

### Option B — reduce the load payload

Refactor consumers so the initial payload is sourced from:

- lap table;
- lap times;
- initial fuel;
- events;
- track length;
- required small summaries.

**Recommended:** add a small V4 session/lap summary table sufficient to preserve the existing `playback_lap_blocks` contract without reading full chunks.

This is metadata, not a duplicate AL telemetry store.

---

## 19. Events and Global Data

Race events are session-wide and are used immediately for playback UI.

Store them in a small dedicated session chunk or summary structure.

Because event volume is bounded and small compared with telemetry, there is no reason to scatter event discovery across every lap during file open.

Similar treatment may be appropriate for:

- session-level participant metadata;
- initial fuel metadata;
- fastest-lap summary;
- other tiny catalog information used immediately on load.

The rule is:

> Metadata required to enter playback belongs in the control plane. High-volume historical rows belong in chunks.

---

## 20. XLSX Export

V4 export must not reconstruct a giant JSONL file.

Algorithm:

```text
open V4
read directory

for chunks in logical session order:
    decompress one chunk
    parse rows
    append to target sheet
    release chunk
```

Because XLSX sheets are row-type based while chunks are `(lap,rowType)`, an efficient traversal is:

```text
for lap in chronological order:
    for chunk belonging to lap:
        export rows
```

or group directory entries by row type if that reduces worksheet switching.

The output must be logically equivalent to V3 export.

Progress can use:

```text
totalUnits = sum(rowCount) or sum(uncompressedSize)
```

rather than relying on the legacy monolithic index length.

---

## 21. Temp Files

For V4:

- do not create `tmpdir/tracknrace_*.tmp`;
- do not decompress the whole source;
- do not require temp disk proportional to recording size.

The stale-temp sweep remains because V1–V3 still use legacy temp files.

It can be removed only when support for all legacy formats that need it is removed.

---

## 22. Memory Management

Chunking solves disk bloat only if the reader also avoids retaining every decoded chunk indefinitely.

Use a bounded cache.

Recommended controls:

```text
max decompressed cache bytes
max Analyze cache bytes / existing frontend limit
optional per-row-family priority
```

Normal playback should keep approximately:

- current lap required chunks;
- optional next-lap prefetched chunks;
- recently used comparison chunks if budget allows.

AL may use streaming accumulation into the frontend's existing history buffers rather than keeping raw decompressed chunk bytes in the native reader after publication.

---

## 23. Integrity and Corruption Handling

Every chunk is independently verifiable.

Directory entries should include a checksum of the uncompressed payload or use ZSTD frame checksum plus explicit size verification.

Recommended:

- enable ZSTD frame checksum;
- also retain an explicit directory checksum if useful for early validation.

On corruption:

- opening the file should succeed if control structures are valid;
- requesting a corrupt chunk should produce a clear reader error identifying lap and row type;
- unrelated chunks remain readable where possible;
- XLSX export may report partial failure rather than silently omitting data.

Header/table corruption should fail load.

Validate:

- magic;
- version;
- table bounds;
- entry sizes;
- monotonically sane lap ranges;
- duplicate chunk keys;
- chunk bounds;
- integer overflow;
- unreasonable decompressed sizes.

Never allocate directly from an unvalidated `uncompressedSize`.

---

## 24. Crash Recovery

A recording that crashes before normal finalization should remain recoverable to the latest committed checkpoint.

### 24.1 Chunk prefix

Each physical chunk should have a small prefix:

```cpp
struct ChunkRecordHeader {
    uint32_t magic;
    uint32_t lapNumber;
    uint16_t rowType;
    uint16_t flags;
    uint64_t compressedSize;
    uint64_t uncompressedSize;
};
```

This allows recovery scanning if the final directory is absent.

### 24.2 Checkpoint footer

Periodically append:

```text
checkpoint magic
lap-table snapshot
chunk-directory snapshot
checksum
```

A recovery reader can locate the latest valid checkpoint.

Normal finalized files point directly to the final directory from the header.

This keeps recovery explicit and bounded.

---

## 25. File Finalization

Normal close:

1. flush queued writer events;
2. finalize mutable timeline;
3. compress and append remaining chunks;
4. compute final lap end for incomplete final lap;
5. discard superseded directory entries;
6. write lap table;
7. write chunk directory;
8. write final footer/commit marker;
9. flush file;
10. patch fixed header offsets/counts/checksum;
11. flush header update;
12. close.

A file is "fully finalized" only when the final footer and patched header agree.

---

## 26. Data Ordering

Physical chunk order is not a semantic guarantee.

Recommended physical order during normal recording:

```text
lap 1 telemetry
lap 1 status
lap 1 damage
lap 1 motion
...
lap 2 telemetry
...
```

Actual order can reflect when chunks become immutable.

The directory defines meaning.

Readers must not depend on adjacent chunk order.

This is important because flashback replacement chunks may be appended much later than the data they logically replace.

---

## 27. API Extensions for Selective History

Do not replace existing APIs immediately.

Add capability alongside them.

Illustrative internal/public additions:

```cpp
using RowTypeMask = uint32_t;

void playerSetHistoryRowMask(RowTypeMask mask);
void playerGetAllLapsData(RowTypeMask mask);
```

or reader-level:

```cpp
SeekFlush seekFlush(
    float target,
    float currentLapStart,
    bool allHistory,
    RowTypeMask requestedTypes,
    float windowSeconds
);
```

Existing calls can default to the current full required set.

This makes the migration additive.

Finite time-window playback uses the current playhead session time as the upper
bound and `max(sessionStart, target - windowSeconds)` as the lower bound. The V4
reader uses the lap table to select every intersecting lap and then decompresses
only the requested row-family chunks for those laps. Current-lap comparison
modes continue to use `currentLapStart`; AL continues to use `sessionStart`.

### 27.1 Required state types

The engine may always add mandatory row types to a requested mask when needed to preserve existing playback semantics.

A frontend request is therefore a minimum requirement, not permission to break state restoration.

---

## 28. Frontend Dependency Registration

Electron cards/charts can declare history dependencies in a central table.

Illustrative:

```ts
const historyDependencies = {
  speedRpm:     ['telemetry'],
  throttle:     ['telemetry'],
  inputs:       ['telemetry'],
  gForces:      ['motion'],
  rideHeights:  ['motionEx'],
  tyres:        ['status'],
  damage:       ['damage'],
}
```

The visible layout produces a union.

Do not make each chart independently invoke the native reader.

Use one coordinator so duplicate row-family requests collapse into one native request.

Qt should use the same conceptual row dependency IDs from `libtnrp`, even if its UI wiring differs.

---

## 29. Threading

Preserve the existing model:

- writer compression/file writes stay on the dedicated writer thread;
- playback state remains serialized by Engine;
- Electron main remains free of heavy synchronous decompression;
- Qt UI thread remains free of heavy decompression.

V4 selective decompression may use a small native worker pool, but cache and playback cursor state still need clear ownership.

Avoid calling `Sink` concurrently from arbitrary decompression workers unless the existing Sink thread-safety contract is intentionally expanded and all hosts are audited.

Preferred:

```text
workers decode
       ↓
playback owner thread orders/assembles
       ↓
Sink
```

---

## 30. `TnrdFormat` Evolution

Extend:

```cpp
enum class TnrdFormat {
    Unknown,
    GzipV1,
    ZstdV2,
    ZstdV3,
    ChunkedV4,
};
```

`isZstd()` should not be used to imply "monolithic ZSTD stream."

Either:

```cpp
isLegacyZstdStream()
```

and:

```cpp
usesZstdCompression()
```

should be separate concepts, or callers should switch explicitly on format.

This prevents old assumptions from accidentally sending V4 through the V2/V3 decompressor.

---

## 31. Capability Checks

Frontend and engine logic must stop encoding capabilities as exact generation checks where possible.

Example of fragile logic:

```text
tnrdVersion === "TNRD_V3"
```

Replace with explicit capabilities from playback metadata:

```text
supportsLapDistance
supportsChunkedRead
supportsSelectiveHistory
```

V4 supports lap distance, so equality checks against V3 would otherwise regress current CL/Analyze behavior.

The wire message can keep `tnrdVersion` for display/debugging while adding capability booleans.

---

## 32. Minimal Required Changes by Component

### 32.1 `protocol_parser_library/include/tnrp/TnrdFormat.h`

- add `ChunkedV4`;
- separate "uses ZSTD" from "legacy stream codec" assumptions.

### 32.2 `TnrdWriter`

- add V4 writer backend;
- maintain current queue/disk-thread contract;
- add mutable lap/type builders;
- write independent ZSTD frames;
- maintain lap/chunk directory state;
- add checkpoint/finalization logic;
- implement copy-on-write flashback supersession;
- retain existing dedupe rules and error reporting.

### 32.3 `TnrdReader`

- dispatch V4 to direct indexed reader;
- no temp file for V4;
- load lap/chunk tables;
- add chunk cache;
- implement chronological merge for selected row types;
- implement latest-row lookup by type;
- implement seek ranges from selected chunks;
- build existing Analyze payloads from chunks;
- provide XLSX chunk iteration.

### 32.4 `Engine`

Initially minimal:

- preserve existing playback APIs;
- accept V4;
- update version/capability reporting;
- optionally add row-mask-aware all-history API later.

Do not move format logic into Engine.

### 32.5 Node addon / Electron main / preload

Phase 1:

- only expose V4 capability/version fields.

Phase 2:

- add row-type mask request plumbing.

Keep existing IPC event shapes where possible.

### 32.6 Electron renderer

- treat V4 as lap-distance capable;
- centralize visible history dependencies;
- use selective AL requests when enabled;
- preserve existing Zustand store shapes.

### 32.7 Qt

- load/play V4 through shared reader;
- no format-specific duplicated decompression implementation;
- add selective dependency requests only when its UI is ready.

### 32.8 XLSX

- switch V4 export to chunk iterator;
- preserve workbook output contract.

---

## 33. Migration Plan

### Phase 0 — tests before implementation

Create golden V3 recordings covering:

- short session;
- full race;
- flashback;
- multiple flashbacks;
- incomplete final lap;
- damage changes;
- sparse status;
- positions;
- events;
- V3 lap-distance Analyze;
- seek near lap rollover.

Capture expected:

- row counts by type;
- lap table;
- lap times;
- events;
- state snapshots at fixed times;
- Analyze payloads;
- seek flush contents;
- XLSX logical output.

### Phase 1 — V4 container primitives

Implement:

- fixed header;
- metadata;
- lap table;
- chunk directory;
- ZSTD independent frame helper;
- validation;
- format detection.

No UI changes.

### Phase 2 — V4 writer

Write V4 while retaining V3 reader support.

Initially permit a debug option to keep V3 recording for A/B comparison.

### Phase 3 — V4 reader parity

Implement V4 versions of:

- load;
- streaming;
- seek;
- state snapshot;
- latest-of-types;
- Analyze lap payload;
- AL all-history;
- XLSX.

At this stage, V4 may still decode all historically required row families.

Goal: semantic parity first.

### Phase 4 — default V4 recording

Once parity tests pass:

```text
setLogging() -> V4
```

Keep explicit legacy writer entry points as required.

### Phase 5 — selective history

Add row-family masks and frontend dependency aggregation.

This is the optimization layer.

### Phase 6 — progressive/parallel AL

Optional:

- parallel chunk decompression;
- progressive history publication;
- smarter prefetch/cache tuning.

---

## 34. Test Matrix

### 34.1 Format tests

- valid empty/minimal V4;
- single lap;
- many laps;
- missing optional row family;
- duplicate directory key rejected;
- invalid offset rejected;
- oversized decompressed size rejected;
- truncated chunk;
- bad checksum;
- bad header;
- bad directory;
- partial crash checkpoint;
- finalized file after checkpoints.

### 34.2 Recording tests

- rotation on track/session change;
- SEND closes recording;
- explicit logging toggle;
- directory change;
- dedupe behavior identical to V3;
- <=30 s flashback;
- flashback across lap boundary;
- flashback behind committed chunks;
- repeated flashbacks;
- recording opened for playback while active;
- flush/close barriers.

### 34.3 Playback tests

For fixed timestamps compare V3 and V4 logical results:

- normal stream rows;
- binary rows;
- state snapshot;
- latest sparse state;
- seek flush;
- positions restore;
- damage 10 Hz reconstruction;
- lap transitions;
- end/replay;
- speed multiplier;
- close/live return.

### 34.4 Analyze tests

Compare:

- lap list;
- fastest lap;
- lap boundaries;
- lap times;
- telemetry;
- motion;
- motionEx;
- status;
- damage;
- lap progress;
- player positions;
- PL;
- FL;
- RL;
- delta/distance capability.

### 34.5 AL tests

Layouts requiring:

```text
telemetry only
motion only
motion_ex only
status only
telemetry + motion
telemetry + status + damage
all supported historical families
```

Assert that unrequested chunk families are not decompressed.

### 34.6 XLSX tests

For equivalent V3/V4 recordings:

- same sheets;
- same logical row counts;
- same values;
- same Info metadata;
- progress reaches completion;
- memory does not scale with full uncompressed race size.

---

## 35. Performance Targets

These are design targets, not hard format guarantees.

### File open

V4 open should be proportional primarily to:

```text
header + metadata + lap table + chunk directory
```

not to recording size.

### Temp disk

V4 playback temp usage:

```text
O(1)
```

with respect to recording size, excluding unrelated application caches.

No full decompressed session temp file.

### Normal playback decompression

Approximately proportional to:

```text
required row families × active/prefetched laps
```

### Analyze

Approximately proportional to:

```text
requested laps × Analyze-required row families
```

### AL

Approximately proportional to:

```text
all relevant laps × row families required by visible AL consumers
```

not every row family in the recording.

### Flashback rewrite

No full-session decompress/recompress.

Work proportional to mutable/affected chunks.

---

## 36. Non-Goals

V4 does **not** attempt to:

- replace JSON row schemas;
- redesign `BinaryRows.h`;
- change F1 protocol parsing;
- change chart APIs;
- change Zustand state shapes;
- redesign Analyze UX;
- redesign AL UX;
- remove V1–V3 support;
- remove stale-temp cleanup yet;
- move format logic into Electron;
- create chart-specific storage;
- create one chunk per field;
- introduce database dependencies.

---

## 37. Finalized Implementation Decisions

The V4 implementation fixes the previously open wire-format choices as follows:

1. lap-table session times are IEEE-754 `float32`, matching recorded `session_time` precision;
2. every Zstandard frame has its frame checksum enabled and every directory entry also carries CRC32 of the uncompressed JSONL;
3. V4 writer checkpoints are append-only delta segments on a 30-second cadence after data leaves the 30-second mutable flashback window; explicit flush and close always commit immediately;
4. the fixed header is 96 bytes, lap entries are 24 bytes, chunk-directory entries are 48 bytes (including the 64-bit segment sequence), chunk prefixes are 32 bytes, and the final commit footer is 32 bytes;
5. the exact signature is the eight bytes `TNRD_V4\0`;
6. session metadata is the existing glaze-serialized `HeaderRow` JSON, stored uncompressed;
7. readers reject any one chunk claiming more than 512 MiB uncompressed and reject more than 1,000,000 chunks before allocation;
8. session/global rows use reserved logical lap `0`;
9. all V4 payload chunks contain the existing JSONL row schema; packed hot-row binary remains an in-memory playback transport only.
10. a 16-byte metadata prefix immediately after the fixed header stores metadata length and CRC32 so checkpoint recovery does not depend on a successfully patched header;
11. a small uncompressed control summary precedes each lap-table snapshot and carries session bounds, initial fuel, events, and per-lap tyre/ERS labels needed at open without payload decompression;
12. the 32-byte footer stores lap-table offset, chunk-directory offset, control-plane CRC32 (summary plus both tables), and control-summary size; lap and chunk counts are derived from the contiguous table spans during recovery.

All integer fields are explicitly serialized little-endian. Compiler struct packing is not part of the format contract.

---

## 38. Recommended V4 Contract

The essential contract is:

> A TNRD V4 recording is an indexed telemetry container. Its uncompressed control plane describes the session, laps, and compressed chunks. Each high-volume chunk is independently Zstandard-compressed and addressed by logical scope, lap, and row type. Readers may decompress only the chunks required for the current operation. Existing Track N Race playback and analysis semantics remain unchanged.

Concretely:

```text
TNRD V4
│
├── Header                         UNCOMPRESSED
├── Session metadata               UNCOMPRESSED
│
├── [ZSTD chunk: lap 1 telemetry]
├── [ZSTD chunk: lap 1 motion]
├── [ZSTD chunk: lap 1 motion_ex]
├── [ZSTD chunk: lap 1 status]
├── ...
├── [ZSTD chunk: lap N ...]
│
├── Lap table                      UNCOMPRESSED
│      lap
│      startSessionTime
│      endSessionTime
│      lapTimeMs
│
├── Chunk directory                UNCOMPRESSED
│      scope/lap
│      rowType
│      offset
│      compressedSize
│      uncompressedSize
│      rowCount
│      checksum
│
└── Final commit footer
```

The most important rule for implementation is:

> **Change where data lives and how it is selected. Do not change what existing consumers observe.**

That keeps V4 from stepping on the toes of the current player, Analyze, AL, Qt, Electron, XLSX export, flashback handling, or legacy recordings while removing the multi-gigabyte temporary-file architecture that motivated the redesign.
