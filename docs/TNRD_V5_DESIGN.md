# TNRD V5 — Read Metadata Design

Status: implemented
Scope: indexed reads and append-only wall-clock rewind branches
Compatibility: V5 is pre-release; no older V5 layout is supported

## 1. Purpose

TNRD V5 currently uses the same basic storage model as V4:

- uncompressed session metadata;
- a lap table;
- a chunk directory;
- independently Zstandard-compressed JSONL chunks grouped by lap and row type;
- append-only checkpoint directories and a commit footer.

This design keeps that model.

The change is to store metadata the writer already knows, but the reader
currently has to discover by decompressing and scanning chunk payloads.

The intended result is:

- faster file opening;
- faster selection of chunks for a time range;
- faster latest-at-time state lookup;
- less JSONL scanning after a selected chunk is decompressed;
- no cumulative row-index copy at every checkpoint;
- no payload deletion or target-lap recompression during a rewind;
- no change to telemetry values, row schemas, playback behavior or UI behavior.

V5 is pre-release, so its current directory entry and checkpoint table may be
changed directly. V1–V4 remain untouched.

## 2. Explicit scope boundary

This design does **not** introduce:

- a new telemetry payload encoding;
- packed binary telemetry stored on disk;
- a new page/container architecture;
- new strategy serialization;
- a new superblock or commit protocol;
- changes to the 30-second flashback buffer;
- chart-specific data;
- decimation or dropped samples.

The existing compressed JSONL chunks remain the source data, including chunks
from branches superseded by a later wall-clock rewind.

## 3. Avoidable reader work addressed by this design

The earlier V5 chunk directory stored:

```text
lap number
row type
flags
payload offset
compressed size
uncompressed size
row count
checksum
segment sequence
```

It does not store the chunk's time bounds. `chunkTimeBounds()` therefore becomes
known only after the chunk has been decompressed and every JSONL row has been
scanned for `session_time`.

That causes several avoidable costs:

1. A range query cannot reject a chunk by time until it has previously been
   opened and scanned.
2. Normal playback does not initially know exactly where a chunk begins and
   ends on the timeline.
3. `latestRows()` may decompress and scan multiple chunks to find one sparse
   state row.
4. After decompression, the reader repeatedly scans for newline boundaries and
   parses `session_time` from JSON.
5. The binary-playback loading pass walks all chunks to build information that
   could have been written into the file directory.

The writer already has each row's type, session time, lap ownership and byte
position while building a chunk. V5 should persist those facts.

## 4. Design summary

Add three serialized metadata structures to the existing V5 control plane and
derive one small in-memory lookup while opening:

1. expanded chunk directory entries containing time and optional distance
   bounds;
2. a per-chunk row seek index containing row times and uncompressed JSONL byte
   offsets;
3. a wall-clock-ordered rewind-branch table, with each chunk stamped by the
   wall-clock branch that wrote it;
4. an in-memory row-family directory built from the type-sorted chunk table.

The compressed chunk payload itself remains unchanged.

```text
+-------------------------------+
| Existing fixed V5 header      |
+-------------------------------+
| Existing session metadata     |
+-------------------------------+
| Existing ZSTD JSONL chunks    |
| ...                           |
+-------------------------------+
| Existing control summary      |
+-------------------------------+
| Wall-clock rewind branches    |
+-------------------------------+
| Existing lap table            |
+-------------------------------+
| Expanded chunk directory      |
+-------------------------------+
| Optional row seek-index table | final snapshot only
+-------------------------------+
| Existing commit footer        |
+-------------------------------+
```

Checkpoint directory entries contain zero index offsets/counts. The final
snapshot locates the one row-index table written at clean close. Existing
header, footer, checksum and recovery rules continue to apply.

## 5. Expanded chunk directory entry

The current 48-byte chunk entry should be extended with metadata used before
decompression.

Illustrative entry:

```cpp
struct V5ChunkEntry {
    uint32_t lapNumber;
    uint16_t rowType;
    uint16_t flags;

    uint64_t payloadOffset;
    uint64_t compressedSize;
    uint64_t uncompressedSize;
    uint32_t rowCount;
    uint32_t checksum;
    uint64_t sequence;

    float    firstSessionTime;     // new
    float    lastSessionTime;      // new
    float    maxLastTimeThroughEntry; // new; per row-family prefix maximum
    float    minLapDistanceM;      // new; NaN when unavailable
    float    maxLapDistanceM;      // new; NaN when unavailable

    uint64_t rowIndexOffset;       // new
    uint32_t rowIndexCount;        // new
    uint16_t rowIndexStride;       // new
    uint16_t rowIndexEntrySize;    // new
    uint64_t branchWallClockMs;    // wall clock of the branch that wrote it
};
```

Required semantics:

- `firstSessionTime` is the minimum valid time in the chunk.
- `lastSessionTime` is the maximum valid time in the chunk.
- Both values are written even if limited packet reordering means the first
  physical row is not the minimum-time row.
- `maxLastTimeThroughEntry` is the maximum `lastSessionTime` from the beginning
  of this row family's directory range through this entry. It permits correct
  interval binary search even when adjacent chunk bounds overlap slightly.
- Distance bounds are present only for row families containing usable lap
  distance; otherwise both are NaN.
- `rowIndexOffset == 0` and `rowIndexCount == 0` mean no index is available.
- Otherwise `rowIndexOffset` points into the final snapshot's row-index table.
- `branchWallClockMs` identifies the recording branch by its wall-clock start.

The reader validates finite time bounds, `first <= last`, sizes, offsets and
index counts before accepting the directory.

### 5.1 Why time bounds matter most

Persisted time bounds allow these operations without touching payload data:

- reject chunks entirely before or after a requested range;
- find the first playback chunk at a seek target;
- identify the next playback frontier;
- select only chunks intersecting the current lap;
- skip chunks after an All Laps seek target;
- choose candidate chunks for latest-at-time lookup.

This removes the current lazy bound-discovery path.

## 6. Optional row seek-index table

The row seek index stores information about rows, not another copy of row data.
The writer retains it in memory and serializes it once at clean close. The JSON
remains inside the compressed chunk. A recoverable checkpoint contains no row
index; the reader scans a selected decompressed chunk when the index is absent.

All entries use one fixed 24-byte representation. Sparse families encode one
row per entry (`firstSessionTime == lastSessionTime`, `rowCount == 1`):

```cpp
struct V5RowSeekEntry {
    float    firstSessionTime;
    float    lastSessionTime;
    uint32_t uncompressedOffset;
    uint32_t byteLength;
    uint32_t firstRowOrdinal;
    uint16_t rowCount;
    uint16_t reserved;
};
```

`uncompressedOffset` and `byteLength` identify the JSON object inside the
decompressed JSONL chunk. They do not attempt random access into a Zstandard
frame. The selected chunk is still decompressed as a unit, after which the
reader can jump directly to indexed rows instead of scanning newlines and
parsing times again.

### 6.1 Index density

Use two index forms:

- sparse state families: index every row;
- high-frequency families: index fixed physical blocks, initially 64 rows per
  entry, recording the minimum and maximum time of every block.

Sparse families include status, damage, lap, session, race events, timing,
participants, all-status, tyre sets and session-history rows.

Telemetry, motion, motion-ex and positions use the block index. A boundary read
scans JSON only in blocks whose stored time range intersects the request. This
remains correct when a few physical rows arrive out of time order.

This keeps index size bounded while eliminating whole-chunk scans for boundary
selection. The block size is stored per chunk so it can be tuned without
changing the reader contract.

### 6.2 Ordering

Exact sparse entries are sorted by:

```text
(sessionTime, uncompressedOffset)
```

The offset resolves equal timestamps and preserves physical order. Hot block
entries remain in physical row order; their min/max bounds determine whether a
block must be inspected.

## 7. Row-family directory

The serialized chunk directory is sorted by row type, time and sequence. During
open, the reader records the chunk indices for every `(lap, rowType)` family in
an in-memory lookup:

```cpp
map<pair<uint32_t, uint16_t>, vector<size_t>> chunkIndex;
```

This derived lookup adds no serialized table or compatibility surface. It allows
the reader to:

- determine row-family presence immediately;
- reserve exact or conservative output storage;
- search only the directory range belonging to a requested type;
- avoid building a map by walking unrelated directory entries.

The finalized chunk directory should be sorted by:

```text
rowType ASC, firstSessionTime ASC, sequence ASC
```

Within each row-family range, `maxLastTimeThroughEntry` is monotonic even if
individual chunk bounds overlap.

Lap queries use the lap table's time interval plus the same per-type directory
range. `lapNumber` remains in each chunk entry for validation and direct
lap-specific filtering.

## 8. Writer behavior

The writer collects the new metadata while it already appends rows to a chunk
builder.

Each builder tracks:

- minimum and maximum session time;
- minimum and maximum valid lap distance;
- row count;
- current uncompressed JSONL byte offset;
- exact seek entries for sparse rows;
- min/max time and byte bounds for each configured hot-row block.

No second scan of the builder is required at checkpoint time.

When the builder is compressed:

1. write the existing chunk prefix and Zstandard JSONL payload;
2. retain its collected row-index entries in writer memory;
3. write checkpoint directory entries with zero index offsets/counts.

On clean finish, serialize all retained row-index entries once and write the
final directory/footer with their locators. No file compaction or payload rewrite
is performed.

### 8.1 Wall-clock rewind branches

The first branch uses the recording header's wall-clock `start_time`. Every
committed rewind appends a branch entry containing a strictly newer wall-clock
value and the authoritative rewind target in session time. Subsequent chunks
carry that new branch wall clock.

A branch is valid only before every newer branch's rewind target. Therefore a
newer branch supersedes all older data from its target onward even when the new
branch is shorter and never produces replacement rows for the old tail. Exact
target-time collisions belong to the newer wall-clock branch.

The writer flushes pending builders, appends the branch cut, resets logical lap
and summary state, and commits a checkpoint. It does not delete, decompress,
filter, or recompress old payload chunks.

## 9. Reader behavior

### 9.1 Open

Opening V5 reads and validates:

- the existing header and metadata prefix;
- session metadata and control summary;
- lap table;
- expanded chunk directory;
- row-family lookup derived from the sorted directory;
- the optional row seek-index table;
- the wall-clock rewind-branch table;
- footer/checksums.

It immediately derives the logical validity ceiling of every chunk from newer
wall-clock branch cuts. Fully superseded chunks are excluded from the active
directory; partially superseded chunks keep their payload but expose only rows
before their logical ceiling. Opening does not decompress a chunk merely to
populate `chunkTimeBounds()`.

The existing full binary warm cache should no longer be required for metadata
discovery. Payload data can be loaded for the requested initial view using the
same indexed range path used by later seeks.

### 9.2 Range selection

For a request `[fromTime, toTime]` and row-type mask:

1. use the derived row-family lookup to visit only requested directory ranges;
2. binary-search `maxLastTimeThroughEntry` for the first possible chunk;
3. walk entries until `firstSessionTime > toTime`;
4. reject entries whose lap/distance bounds do not intersect when applicable;
5. decompress only the remaining chunks;
6. enforce the chunk's wall-clock-derived logical time ceiling;
7. use the row seek index when present, otherwise scan the decompressed JSONL;
8. parse or return the relevant rows.

The unavoidable unit of decompression remains one existing V5 chunk. This
proposal reduces incorrect chunk selection and post-decompression scanning; it
does not claim random access inside a Zstandard frame.

### 9.3 Latest-at-time lookup

For every requested sparse row type:

1. use its directory range and chunk bounds to find the last chunk that may
   contain a row at or before the target;
2. binary-search that chunk's exact row index when present, otherwise scan it;
3. if no indexed row qualifies, move to the previous chunk;
4. group results that use the same chunk;
5. decompress each unique selected chunk once;
6. extract the exact JSON row by offset and length.

This replaces scanning all rows in every candidate chunk.

### 9.4 Normal playback

The reader knows each chunk's first and last time before it is loaded. It can:

- choose the correct initial chunk immediately;
- prefetch the next required chunk using persisted bounds;
- avoid decoding chunks entirely beyond the playhead;
- jump near the first due row using the seek index after decompression, or scan
  the chunk when reading a checkpoint without an index.

### 9.5 All Laps

All Laps remains lossless and necessarily reads every requested historical row
up to the target. The metadata still avoids:

- reading unrequested row families;
- reading chunks wholly after the target;
- scanning full boundary chunks to discover times;
- repeated allocation growth when row totals are already known.

It does not make full-history output constant-time.

## 10. Metadata size

The added metadata must remain substantially smaller than the source data.

Approximate initial costs:

- expanded directory: about 32 additional bytes per chunk;
- exact sparse index: one 24-byte entry per sparse row;
- 24-byte hot block index at 64 rows: about 0.375 bytes per hot row;
- derived row-family lookup: negligible.

Checkpoint growth is the current directory and summary, never the cumulative
row index. A finished file adds the complete index exactly once.

The writer should record actual metadata bytes in the control summary so tests
can report index overhead as a percentage of total file size.

Latest-at-time state families remain exactly indexed. A sparse family that is
never queried as state may use the hot block form if profiling shows its exact
index is unreasonably large.

## 11. Recovery and validation

The existing V5 checkpoint/footer recovery model remains in place. A checkpoint
commits complete chunk payloads with no row index. The final snapshot includes
the optional index table in the existing control checksum.

Validation includes:

- all offsets and sizes within the actual file;
- overflow-safe table size calculations;
- known row types and supported flags;
- finite, ordered chunk bounds;
- either zero row-index offset/count, or offsets within the final index table;
- monotonic exact sparse index times and offsets at equal times;
- valid hot block bounds, offsets, ordinals and row counts;
- indexed JSON offsets within the chunk's uncompressed size;
- exact sparse index count matching the chunk row count;
- hot block indexes covering every physical row exactly once;
- directory family ordering and derived family ranges agreeing;
- no active payload/control range overlap.
- strictly increasing branch wall clocks and finite rewind targets;
- every chunk references the header branch or a known rewind branch;

A missing index is valid and selects the linear JSONL scan path. A present but
invalid index is corrupt because its snapshot checksum or structural validation
failed.

## 12. Expected improvement

| Operation | Earlier V5 | Metadata-enhanced V5 |
|---|---|---|
| Open chunk directory | Bounds unknown | All bounds immediately available |
| Reject disjoint chunks | May require first decompression | Directory-only |
| Find range boundary row | Scan JSONL and parse times | Binary-search metadata, short local scan |
| Find latest sparse row | Decompress/scan candidate segments | Exact index locator plus one selected chunk |
| Prepare playback frontier | Discover bounds lazily | Bounds known at open |
| Reserve range output | Estimate/grow allocations | Use stored counts and sizes |
| All Laps boundary work | Scan complete boundary chunks | Jump near exact boundary |

The main remaining costs are deliberately unchanged:

- decompression of chunks that actually contain requested rows;
- JSON parsing/encoding of requested data;
- linear output cost when the caller requests the complete history.

Those are separate optimization topics and are not part of this metadata design.

## 13. Acceptance criteria

- `TnrdV5Archive::open()` performs zero chunk decompressions.
- `chunkTimeBounds()` succeeds for every valid chunk immediately after open.
- A range query never decompresses a chunk with disjoint stored time bounds.
- A finalized, indexed latest-at-time lookup decompresses only the selected
  candidate chunk; an index-free checkpoint scans the candidate chunks selected
  from directory time bounds.
- The row seek index preserves exact row ordering at equal timestamps.
- All Laps returns every requested row without decimation.
- Seek results match a full sequential scan at chunk and lap boundaries.
- A shorter new branch never exposes the superseded tail of an older branch.
- Rewind commits do not rewrite old payload chunks.
- Recovery rejects a snapshot whose control checksum is invalid.
- Repeated checkpoints contain no row-index bytes.
- Clean finish writes the complete row index exactly once.
- Metadata overhead is measured on short, 30-minute and 113-minute recordings.
- V1–V4 tests remain unchanged and passing.

## 14. Implementation order

1. Expand the V5 chunk entry and header/footer table locators.
2. Track time/distance bounds in each writer builder.
3. Generate exact sparse and coarse hot-block seek entries during append.
4. Write index-free checkpoints and the complete row index at finalization.
5. Initialize all chunk bounds directly during `TnrdV5Archive::open()`.
6. Change range selection to use directory binary searches.
7. Change `latestRows()` to use exact sparse indexes.
8. Use row indexes inside decompressed boundary chunks.
9. Stop using a whole-file payload walk merely to discover V5 seek metadata.
10. Add corruption, flashback, range-boundary and long-recording benchmarks.

## 15. Final constraint

This V5 change is successful if the reader can decide **which existing
compressed JSONL chunks and which rows within them are relevant** using only the
uncompressed control metadata.

Anything that changes the payload representation, strategy model, container
commit architecture or UI data model belongs in a separate proposal.
