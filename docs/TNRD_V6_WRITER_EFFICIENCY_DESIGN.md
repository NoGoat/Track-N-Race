# TNRD V6 Writer Efficiency

Status: implemented 2026-09-21. Findings 1-3 and the recovery contract are in
the tree; finding 4 is exposed as a setting with the default unchanged.
Scope: the V6 writer path (`protocol_parser_library/src/tnrd/TNRD_V6.cpp`,
`protocol_parser_library/src/TnrdWriter.cpp`) and, for recovery, the V6 reader
in the same file. No frontend or UDP parsing change.
Baseline: São Paulo capture converted 2026-09-21 with `udp_capture_to_tnrd`.

## 0. Measured outcome

Same capture, same level 3, same machine, before and after:

| | Before | After |
|---|---|---|
| Peak file size during conversion | ~3.8 GB | 903,177,602 B (no peak: peak == final) |
| Final file | 903,179,944 B | 903,177,602 B |
| Chunks | 34,784 | 34,784 |
| Wall time | 672 s | 524 s (-22%) |
| Closing compaction | ~900 MB read + ~900 MB write | none |

The file no longer grows and shrinks; it is written once at its final size. The
small size delta is expected rather than a data difference: the converter stamps
rows with wall-clock timestamps, so two runs never produce byte-identical
payloads. Chunk count and session length are identical.

Verified by `protocol_parser_library/tests/TnrdV6Recovery.cpp` (10/10 project
tests pass, including the pre-existing V6 reader cases). The suite asserts that
a recording truncated at its index start rebuilds chunk metadata and lap
summaries identical to what the writer produced, which is the claim in §3.5 that
the reconstruction and `boundary()` agree.

## 1. Purpose

Converting `udp_capture_sao_paulo.bin` (2,518,182,672 B) to V6 takes 672 s and
writes a file that peaks near 3.8 GB before compaction brings it to 903 MB. The
peak is not data. It is discarded index.

This document records four measured inefficiencies and proposes fixes for each.
Three concern writer throughput and I/O; one concerns output size. They are
independent and can land separately.

## 2. Measurement baseline

All figures are from the level-3 run in
`tools/udp_capture_to_tnrd/compression_comparison/`, read back from the
produced file's own footer and metadata.

| Quantity | Value |
|---|---|
| Input capture | 2,518,182,672 B |
| Datagrams converted | 2,498,862 (0 dropped) |
| Session length | 5536.8 s |
| Drivers | 22 |
| Lap entries in metadata | 1,546 |
| Chunks | 34,784 |
| Live chunk payload | 899,342,603 B |
| Chunk directory (34,784 × 56 B) | 1,947,904 B |
| Metadata JSON | 1,889,389 B |
| Final file | 903,179,944 B |
| Conversion wall time | 672 s |

## 3. Finding 1 — checkpoint cost is quadratic in laps

### 3.1 Mechanism

`commit()` calls `snapshot()` whenever it appended chunks
(`TNRD_V6.cpp:636`). `snapshotTo()` (`TNRD_V6.cpp:641`) seeks to end of file and
appends, in full:

1. the entire metadata JSON — every driver header, every lap summary so far,
   every shared record;
2. the entire chunk directory — every chunk so far, 56 B per entry;
3. a 48-byte footer.

It then seeks to offset 0 and rewrites the 128-byte header to point at the new
copy. The previous metadata and directory are left on disk. Nothing truncates or
reuses them.

Checkpoint *k* therefore costs `metadata(k) + 56 × chunks(k) + 48`, and both
terms grow linearly with committed laps. The sum is quadratic.

### 3.2 Magnitude

The final checkpoint blob is 3,837,341 B. Checkpoints grow roughly linearly from
nothing to that size across ~1,546 snapshots, so total checkpoint bytes are
approximately `1546 × 3,837,341 / 2` = **2.97 GB**, against 903 MB of live
chunks. That accounts for the ~3.8 GB peak, and matches the uncompacted
`tools/udp_capture_to_tnrd/udp_capture_sao_paulo_v6.tnrd` still on disk at
3,659,548,029 B.

Every one of those bytes is written and then discarded.

### 3.3 What the current design buys

The header at offset 0 always points at a complete, checksummed index, and it is
rewritten only after the new index has been flushed. An interrupted recording is
therefore always readable up to the last checkpoint. This is a real property of
the format and any replacement must keep it. The cost is not the crash-safety;
it is rewriting the *whole* index to get it.

### 3.4 Proposed fixes

**Fix 1a — write the index once, at end of session (preferred).** The periodic
checkpoint exists only to bound crash loss. Where a crash cannot lose anything,
it is pure overhead, and where it can, §3.5 makes it unnecessary. Writing one
index when the stream closes reduces checkpoint cost from ~2.97 GB to 3.84 MB,
reduces metadata serialization from ~1,546 passes to one, and leaves zero dead
bytes — which removes compaction by construction rather than by threshold
(§4). Total write I/O for the baseline conversion falls from ~4.7 GB to
~907 MB.

This is unconditionally correct for `udp_capture_to_tnrd`: the input is a file
on disk, so an interrupted conversion is re-run, not lost. It should be adopted
there first and independently of everything else in this document.

For live recording it requires §3.5, because a session cannot be re-run.

**Fix 1b — checkpoint cadence (fallback, if 1a is not adopted for live).**
Snapshot on a budget — whichever comes first of *N* seconds of session time or
*M* new chunks — rather than on every chunk-producing commit. At one checkpoint
per 30 s of session time this run takes ~185 snapshots instead of 1,546, cutting
dead index roughly 8x with an unchanged layout and an unchanged reader. It does
not remove the quadratic term, only its constant, and ~370 MB of dead index on a
903 MB file still exceeds any sensible compaction threshold. Treat it as a
stopgap.

`TnrdV6Writer::checkpoint()` (`TNRD_V6.cpp:1142`) remains available for callers
that want a checkpoint on demand under either fix.

### 3.5 Making index-at-end safe for live recording

The index is not the only home for this data. Almost all of it is already in the
append-only chunk stream, so a crashed recording can be rebuilt by scanning
rather than by checkpointing.

**The checkpoint protects less than it appears to.** Chunks reach disk in
`commit()`, and `snapshot()` fires immediately afterwards in the same call, so
the laps in the index and the laps on disk are always the same set. A crash
loses the in-flight lap — whose builders are still in memory — under periodic
checkpointing exactly as it would under index-at-end. The checkpoint buys no
extra data. It buys only the time a rebuild would take.

**`V6LapSummary` is fully derivable from `LapTiming` chunks.** The samples
written at `TNRD_V6.cpp:915` carry `lap_num`, `last_lap_ms`, `s1_ms`, `s2_ms`
and `lap_invalid`:

| Field | Source |
|---|---|
| `lapId`, `driverIndex`, `phase` | chunk prefix |
| `lapNumber` | `lap_num` |
| `startSessionTime`, `endSessionTime` | sample time range within the chunk |
| `lapTimeMs`, `s1Ms`, `s2Ms` | next lap's first sample |
| `s3Ms` | `total - s1 - s2` |
| `isValid` | `lap_invalid` |
| `isCompleted`, `isPartial` | `lapNumber > previous && total > 0` |

This is exactly the derivation `boundary()` performs live at
`TNRD_V6.cpp:908-911`. Driver headers are equally derivable: `availableTypeMask`
is which chunk types exist per driver, which is what `rebuildLiveMetadata()`
already computes; tyre stints come from `TyreState` chunks; restriction changes
come from participants data, already appended inline as `SHR6` records.

**One genuine gap: the session `HeaderRow`.** It is set at `open()`
(`TNRD_V6.cpp:1120`) but the 128-byte on-disk header carries only offsets
(`TNRD_V6.cpp:195`), so track, session type and protocol exist solely in the
trailing metadata JSON. It is known at open and essentially immutable, so
writing it once near the front of the file closes the gap. This is the only new
bytes-on-disk requirement in this section.

**Required work:**

1. **Session header at the front of the file**, written at `open()`.
2. **Reader recovery scan.** When the header index is absent, stale or fails its
   checksum, walk the file by `CHK6`/`SHR6` magic, rebuild directory entries
   from the prefixes, and replay the `LapTiming` chunks to rebuild lap summaries
   and driver headers.

**Optional, for recovery speed only:** the chunk prefix carries sizes but not
`firstTime`, `lastTime` or the CRC, so a scan must decompress to recover them.
Widening the prefix from 32 B to 48 B (16 B x 34,784 chunks, ~557 KB) makes
recovery a single pass with no decompression. At zstd decompression speeds a
full-file rebuild is a few seconds either way, so this is a convenience, not a
prerequisite. It is the only item here that changes the on-disk chunk layout and
can be deferred indefinitely.

**The real risk is duplicated semantics, not lost data.** A reconstruction path
that re-derives lap boundaries independently will drift from `boundary()` as
that logic evolves, and the drift would be silent — reconstructed recordings
would differ subtly from live-written ones. Mitigate by factoring the
lap-derivation predicate into one function that both the live writer and the
recovery scan call, and by testing recovery against a truncated copy of a known
recording, asserting the rebuilt metadata matches the original byte for byte.

An earlier draft of this section proposed inline `LAP6` lap-summary records and
a mandatory prefix widening. Both are unnecessary: the data is already present.
A segmented directory was also considered and rejected — it removes the
quadratic term but keeps compaction and keeps the index on the critical path for
correctness.

### 3.6 Recovery contract: the reader repairs itself

Recovery is not a tool, a prompt, or a separate mode. A recording interrupted by
a crash, a power loss or a force-quit must open normally on the next attempt,
with the reader rebuilding whatever the writer did not get to. The user should
see a recording that opens, not an error.

**3.6.1 Detection.** `TnrdV6Archive::open()` (`TNRD_V6.cpp:1301`) currently has
six gates that each do `fail(); close(); return false`:

| Gate | Line | Meaning | Action |
|---|---|---|---|
| File shorter than header + footer | 1305 | no index written yet | recover |
| Bad magic / version / header CRC | 1310 | not a V6 file | hard fail |
| Control-plane ranges inconsistent | 1321 | index absent or stale | recover |
| Footer mismatch | 1329 | index torn mid-write | recover |
| Control-plane checksum | 1337 | index corrupt | recover |
| Metadata JSON unparseable | 1344 | index corrupt | recover |

Only the magic/version/CRC gate stays a hard failure: it is the one that means
"this is not a TNRD V6 file", and a scan would be meaningless. Every other gate
means "this is a V6 file whose index is unusable", which is exactly the case
recovery handles. The distinction matters — recovery must never be attempted on
an arbitrary file, and must always be attempted on a V6 one.

**3.6.2 A sentinel header makes an in-progress file identifiable.** Under
index-at-end, a file being written has no index at all. Write the 128-byte
header at `open()` with correct magic, version and CRC but a sentinel index
(`metadataSize = 0`, `chunkCount = 0`), alongside the session `HeaderRow` from
§3.5. The file is then self-identifying from the first byte: the magic gate
passes, the ranges gate sees the sentinel and routes to recovery, and the reader
knows the track and session before scanning a single chunk.

**3.6.3 The scan.** Walk forward from the end of the front matter:

1. Read a 4-byte magic. `CHK6` and `SHR6` are the only valid values; anything
   else ends the scan.
2. Read the prefix, take `compressedSize`, and record a directory entry at the
   current offset with `sequence` assigned in scan order.
3. Skip `compressedSize` bytes to the next record.
4. Decompress `LapTiming` chunks as they are found and replay them through the
   shared lap-derivation predicate (§3.5) to rebuild lap summaries and driver
   headers. Decompress other chunks only if `firstTime`/`lastTime` are needed
   and the prefix has not been widened.
5. Parse `SHR6` records for participants, which restores driver names and
   telemetry-restriction state.

**3.6.4 The torn tail.** A crash during `writeChunk()` leaves a prefix with no
payload, or a partial payload, at the end of the file. Two checks handle it
without ambiguity:

- If the declared `compressedSize` exceeds the bytes remaining to EOF, the
  record is truncated. Stop; keep everything before it.
- If the length fits but the payload is corrupt, zstd's own frame checksum
  catches it — `writeChunk()` already sets `ZSTD_c_checksumFlag` to 1
  (`TNRD_V6.cpp:456`), so decompression self-validates.

The scan stops at the first record that fails either check and keeps every
record before it. A torn tail costs the partial chunk, never the recording.

**3.6.5 Repair is non-destructive, and by default is not written back.** The
rebuild happens in memory and the file is not modified. That is the default for
a reason: the live recorder may still hold the file open, and a reader that
writes to a file another process is appending to would corrupt both. Persisting
a rebuilt index must therefore be opt-in, taken only by a caller that knows the
writer is gone, and implemented as an append of a fresh index plus a header
rewrite — never an edit of existing bytes. Then the repair is itself
crash-safe and idempotent: interrupt it and the next open simply scans again.

Under no circumstances does recovery rewrite, reorder or discard chunk payload
bytes. The worst outcome of a failed recovery is the error the reader would
have returned anyway.

**3.6.6 What is lost, stated precisely.** A crash loses the laps still held in
memory builders — at most the in-flight lap per driver, plus any completed lap
inside its 30 s `WRITE_DELAY` window. This is identical to what periodic
checkpointing loses today (§3.5) and is a property of the write delay, not of
the index scheme. Nothing that reached disk is lost.

**3.6.7 Reporting.** `open()` should report that a recording was recovered and
how much was rebuilt, through the existing error/callback channel, so the
frontend can note it rather than silently presenting a recording that may be
missing its final lap. Recovery that succeeds silently is worse than recovery
that succeeds loudly: the user needs to know the last lap may be absent.

**3.6.8 Tests.** Recovery is only real if it is exercised:

- truncate a known-good recording at many offsets — inside a chunk prefix,
  inside a payload, between records, inside the trailing index — and assert
  `open()` succeeds at every one;
- assert that a recording truncated exactly at its index start rebuilds
  metadata identical to the original;
- assert the magic gate still hard-fails on a non-V6 file;
- assert a recovered archive answers chunk reads identically to the original for
  every lap that survived.

## 4. Finding 2 — closing compaction is unconditional

`finish()` always calls `compactAndReplace()` (`TNRD_V6.cpp:686,1159`), which
opens `<path>.compact.tmp`, copies every live chunk and shared record by offset,
writes one final checkpoint, and renames over the original. That is a full
~900 MB read plus ~900 MB write at close, on top of the ~3.8 GB already written.

Compaction exists only to reclaim what §3 abandons. Under Fix 1a there are no
dead bytes to reclaim and compaction can be deleted from the close path
entirely; the final index write already produces the compacted layout.

If Fix 1b is taken instead, compaction must stay, and should become conditional:
track dead bytes as checkpoints are superseded — the writer knows each
superseded blob's size when it writes the replacement — and compact at close
only above a threshold. This is strictly worse than Fix 1a, which makes the
question moot.

## 5. Finding 3 — per-packet work that is almost always wasted

`TnrdWriter.cpp:411` calls `v6Writer_->advanceSessionTime()` on every
`NotePacket`, which calls `commit(false)` — 2,498,862 times in this run.

`commit()` scans all pending laps for eligibility, calls `commitControl()` to
scan pending shared records, restriction changes, and tyre history, then
unconditionally calls `rebuildLiveMetadata()` (`TNRD_V6.cpp:531`) *even when
nothing was committed*. `rebuildLiveMetadata()` deep-copies all 22 committed
driver headers — including their `lapIds`, `restrictionChanges` and `tyreStints`
vectors, which grow all race — and re-parses every pending participants row from
JSON with Glaze.

So the writer performs ~2.5 M copies of a structure whose size grows
monotonically with the session, to produce a value that changes only when
something is actually pending. This does not affect file size, which is why it
did not surface in the size investigation.

**Proposed fixes, cheapest first.**

1. **Early-out in `commit()`.** Keep the earliest pending deadline across laps,
   shared records, restrictions and tyre history. When session time has not
   reached it and no phase change occurred, return immediately. This skips the
   scans and the rebuild for the overwhelming majority of packets.
2. **Make `liveHeaders` lazy.** It is read by metadata consumers, not by the
   write path. Mark it dirty when a pending collection mutates and rebuild on
   read rather than on advance.
3. **Stop re-parsing participants JSON.** `rebuildLiveMetadata()` runs
   `glz::read` over pending participants rows on every call; cache the parsed
   row alongside the pending record when it is queued.

Item 1 alone should remove most of the cost and is contained to one function.

## 6. Finding 4 — compression level

Measured over four full conversions, all validated as V6 / F1 2026 / São Paulo /
5536.8 s with 2,498,862 datagrams and 0 dropped. Full table and caveats in
`tools/udp_capture_to_tnrd/compression_comparison/RESULTS.md`.

| Level | Output | vs level 3 | Convert time |
|---|---|---|---|
| 3 (current) | 903,179,944 B | baseline | 672 s |
| 5 | 881,491,331 B | −2.40% | 1018 s |
| 7 | 809,186,337 B | −10.40% | 1065 s |
| 9 | 746,190,435 B | −17.38% | 1136 s |

Level 5 is strictly a bad trade: 2.4% smaller for 52% more time. The curve is
flat from 3 to 5 and then drops sharply; 7 and 9 are where the gains are, and 7
to 9 costs only ~7% more time than 7 itself for another 7 points of size.

**Proposed fix.** Make the level a writer setting rather than a literal. A
temporary `TNRD_ZSTD_LEVEL` environment hook exists in
`tnrp::detail::zstdLevel()` (`TNRD_V6.cpp:32`), added to run this comparison; it
defaults to 3 and should be replaced by a proper setting or removed.

Do **not** raise the live-recording default on the strength of this table. These
are bulk offline conversions; live recording compresses one chunk at a time on
the writer thread, so the number that decides it is per-chunk latency at level
7/9 against the 30 s write delay, which is not measured here. Offline conversion
is free to default higher.

## 7. Staged plan

1. **Index-at-end for the offline converter only** (Fix 1a). No format change,
   no reader change, no crash-safety argument to settle — the input is a file.
   Removes ~2.97 GB of dead writes and the ~1.8 GB compaction round trip on its
   own. Do this first.
2. **`commit()` early-out** (§5, item 1). Contained, no format impact, likely
   the largest CPU win per line changed.
3. **Compression level as a setting** (§6), with a separate per-chunk latency
   measurement before any live-path default moves off 3.
4. **Session header at front, sentinel header at open, and automatic reader
   recovery** (§3.5, §3.6). This is what lets index-at-end apply to live
   recording and lets compaction be deleted outright. Factor the lap-derivation
   predicate so the live writer and the scan share it. Recovery is automatic on
   open, non-destructive, and reported to the caller.
5. **Delete compaction from the close path** (§4), once 4 has landed and
   recordings no longer accumulate dead bytes.

Fix 1b (checkpoint cadence) and conditional compaction are the fallback path if
step 4 is not taken. They are cheaper to build and strictly weaker.

## 8. Measurement

Re-run `tools/udp_capture_to_tnrd/convert_capture.py` against the same São Paulo
capture after each stage and record: wall time, peak file size during the run,
final size, and whether compaction ran. Peak size is the headline number for
stages 1–3 and is not visible in the final file, so it must be sampled during
the run. The level sweep in `compression_comparison/` is the size baseline; the
672 s / 3.8 GB peak of the level-3 run is the throughput baseline.

Correctness gate for every stage: the converter's own `--validate` pass must
still report V6, F1 2026, São Paulo, 5536.8 s, and the chunk count and live
payload size must be unchanged for stages 1–3, which alter when and how the
index is written but not what data is stored.

From stage 4 onward the gate also includes the recovery suite in §3.6.8. Index-at-end
moves crash resilience from the writer to the reader, so a stage that ships
index-at-end without passing recovery tests has removed a guarantee rather than
relocated it.
