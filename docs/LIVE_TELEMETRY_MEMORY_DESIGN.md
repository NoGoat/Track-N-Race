# Live Telemetry Memory Design

## Purpose

Reduce telemetry memory growth during a live UDP session without changing the
existing chart and lap-selector behaviour.

The TNRD Monaco experiment represented about 6.29 GiB of decompressed rows. The
current realtime retention path held about 1.1 GiB of telemetry data near the
end of the race. The proposed model keeps recent and selected laps immediately
available and stores older history compressed in memory.

In this document, a **family** means one telemetry row/packet family such as
Telemetry, Status, Damage, Lap, Motion, or Motion Ex.

## Storage model

Split live data by lap and family, following the same useful boundary as the
TNRD container:

```text
Lap
├─ Telemetry
├─ Status
├─ Damage
├─ Lap
├─ Motion
└─ Motion Ex
```

Keep these lap roles uncompressed:

- Current lap
- Previous lap
- Previous-previous lap
- Fastest lap

Roles reference laps rather than owning copies. If the fastest lap is also the
previous lap, there is only one uncompressed lap in memory for both roles.

Older laps are stored compressed by family. Compression happens away from the
UDP thread after a lap is no longer part of the uncompressed working set.

## Normal lap rollover

Before crossing from lap `N` to lap `N+1`:

```text
Current             N
Previous            N-1
Previous-previous   N-2
```

After the boundary:

```text
Current             N+1
Previous            N
Previous-previous   N-1
```

Lap `N-2` can be compressed unless it is also the fastest lap or is currently
decompressed for an onscreen All Laps graph.

## Rewind behaviour

A rewind is short enough that crossing at most the immediately preceding lap
boundary is the case this design needs to keep ready.

Before a rewind from lap `N` into lap `N-1`:

```text
Current             N
Previous            N-1
Previous-previous   N-2
```

After the rewind:

```text
Current             N-1   (truncate to the rewind target)
Previous            N-2
Previous-previous   empty
```

Discard invalid future data from lap `N`. Do not decode lap `N-3` merely to
refill Previous-previous. That role exists only so the Previous selector remains
available immediately when a rewind crosses the lap boundary.

When the session rolls forward into lap `N` again, the normal roles are restored
from the laps already in memory:

```text
Current             N
Previous            N-1
Previous-previous   N-2
```

If a rewind invalidates the recorded fastest lap, select the previous valid
fastest from lap-time metadata and decode that lap only if it is not already in
the uncompressed working set.

## All Laps mode

All Laps decompression is driven only by onscreen graph requirements.

- Build the requested-family mask from the visible graphs.
- Decompress only those families across historical laps.
- Share one decompressed family between every visible graph that requests it.
- Reuse the already-uncompressed Current, Previous, Previous-previous, and
  Fastest laps rather than creating copies.
- When no onscreen graph requests a family, release its decompressed historical
  data while retaining the compressed copy.
- A hidden or disabled graph must not keep its family decompressed.

For example, an All Laps tyre-wear graph decompresses Damage history only. It
does not also decompress Telemetry, Status, Motion, or Motion Ex.

## Performance requirements

- UDP parsing and delivery must not wait for compression or decompression.
- Seal a completed lap quickly and perform compression on a worker.
- Decompression requests must be cancellable or safely ignored when the visible
  graph requirements change before the work finishes.
- Compressed data remains the canonical historical copy so decoded All Laps
  data can be released immediately.
- Existing hot-row batching and chart rendering behaviour remains unchanged.

## Scope

This is an in-memory retention change for live UDP sessions. It does not require
a new TNRD file-format version and does not change normal playback storage.

## Implementation status

The first implementation slice is in place:

- `LiveHistoryStore` owns live rows by lap and family.
- Current, Previous, Previous-previous, and Fastest are role references to lap
  segments; older eligible laps are compressed with Zstandard on a worker.
- live range backfills are asynchronous and family-selective.
- stale range results are ignored after a reset, rewind, or newer renderer
  requirements request.
- the renderer trims ordinary live source buffers to three laps and keeps the
  existing Previous/Fastest snapshots for selectors.
- rewind truncates the target lap, promotes the two recent roles, leaves
  Previous-previous empty, and invalidates a future Fastest role.

Runtime memory counters and fixture-driven end-to-end profiling remain follow-up
validation work; they are not part of the storage mechanism itself.
