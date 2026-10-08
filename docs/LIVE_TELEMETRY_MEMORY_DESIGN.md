# Live Telemetry Memory Design

## Purpose

Hold a live UDP session in memory the way a TNRD V6 file holds a recording, so
live mode and playback share one data model, one reader and one renderer
decode path, and so the session is processed once whether it is recorded or
not.

The first version of this store (`LiveHistoryStore`, 2026-09-11) predated the
V6 overhaul. It kept the player's laps only, by the old row families, as packed
records and JSON strings, and compressed old laps as opaque zstd blobs, while
the recorder ran its own V6 writer beside it. Both jobs are now done by one V6
writer.

## Storage model

The recorder, `TnrdWriter`, owns the session's one `TnrdV6Writer`. On hosts
that read live history it is opened with `openMemory()` for the whole session
(`TnrdWriter::setRetainSession`), recording or not. It is the V6 file
architecture of [TNRD_V6_DESIGN.md](TNRD_V6_DESIGN.md), held in memory:

```text
Driver (every participating car, by vehicle index)
└─ Lap (file-local lap ID, game lap number, phase, summary)
   └─ Data type (Speed, RPM, … LapTiming)
```

- Open laps, and completed laps still inside the 30 s write delay, are
  `SampleColumns` builders: typed columns at their encoded widths.
- When a lap's delay passes it is committed exactly as a file commits it: lap
  clock, V6C1 column chunks, Zstandard level 9. The frames stay in memory,
  indexed by `V6ChunkInfo::offset`.
- Shared records (session, participants, race events) are deduplicated and
  delayed as in a file. Committed ones stay plain JSON in memory rather than
  zstd-chained, so a deep rewind can drop them without re-decoding the chain.
- Driver headers and lap summaries are the writer's own committed and live
  metadata.

## Recording

Recording attaches a file to that same writer (`TnrdV6Writer::attachFile`)
when the session packet that starts a recording arrives, exactly where a file
used to be opened:

- The file first receives everything committed so far, frame for frame, and
  the shared records compressed and chained as a file holds them. A recording
  switched on part-way through a session therefore starts at the session's
  beginning.
- Each later commit is compressed once and kept in memory, and the same frame is
  appended to the file.
- `detachFile` (recording switched off, session end, a new session, playback
  opening the file) writes the laps still being built and the pending shared
  records to the file alone, open laps as partial, then the index, as
  `finish()` does. The session carries on in memory.
- A failed write closes the file where it stands (recovery scan still reads it)
  and is reported; the session is unaffected, and the next session packet
  starts another file holding all of it.
- Checkpoints (`flushToDisk`, before playback opens the active recording) index
  the file as before.

Hosts that do not read live history (Android, the minimal frontend, the capture
converter) keep the previous behaviour: one file writer per recording, nothing
held while not recording.

## Ingestion

Each datagram's rows reach `TnrdWriter` once, with its timeline time, when it
records or retains the session. The engine then parses every packet family
with the hot rows also serialised as JSON, because the all-car arrays exist
only there.

## Rewinds and sessions

- Only an FLBK event rewinds the writer, as for any recording. A bare clock
  reset, such as the formation lap ending, is a phase change for the writer.
  The engine still invalidates in-flight reads for it.
- A rewind into laps still being built is the writer's normal rewind.
- A rewind that reaches committed laps is refused by a plain file writer.
  Memory can take them back: `TnrdV6Writer::Impl::uncommit` decodes every
  committed lap of the phase with a sample at or after the target back into
  builders, returns it to its driver's pending laps, and drops committed shared
  records and restriction changes from the target on. The normal rewind then
  reopens or drops those laps, and each affected driver's committed state is
  replayed from the laps that remain. An attached file is then rewritten from
  memory beside the original and swapped in, so it never holds the abandoned
  timeline.
- A new session UID drops the retained session (`TnrdWriter::resetSession`),
  finishing its file.

## Reads

`LiveV6Store` is the read side. It asks the writer thread for a
`V6MemoryImage` (committed chunk frames shared by pointer, laps still being
built encoded for the image, lap summaries, driver headers and the requested
shared records), built after every event queued before the request. Its own
read thread opens the image with `TnrdV6Archive::openMemory()` and reads it as
playback reads a file.

| Consumer | Read |
| --- | --- |
| Chart backfill (`applyDataRequirements`) | `columnarHistory` for the player, seeded at the range start, plus race events as JSON lines |
| Host restore (`issueLiveRestoreLocked`) | The same, with separate chart and event starts |
| Live fastest lap (`liveGetFastestLap`) | The player's fastest completed lap from its lap summaries, its chart families as V6H1. The caller's callback receives a `live_fastest_lap_data` JSON header and the payload bytes beside it |
| Live Previous / Fastest (`liveGetLapData`) | One lap by number, read the same way (`live_lap_data`). Electron asks on every lap change, restore, rewind and whenever a chart shows Previous or Fastest, and replaces its own snapshot, which has holes for anything received while the window was hidden |
| Deep Strategy rollback | `TnrdReader::loadV6ArchiveForStrategy` on an image of the Strategy types, then `strategySnapshotAt`, as a V6 recording's playback rebuild |

A read requested before the timeline moved reports stale, as before.

The archive normally exposes the race phase only. An image taken while the
session is still on its formation lap exposes that phase instead.

## Frontends

- **Electron:** live backfills and restores arrive as V6H1, decoded by
  `decodeV6History`. A backfill is merged by time with the rows held (laps
  missed while hidden sit between held ones, so a prefix install would leave
  them empty); a restore replaces its range. Race events are read from the
  cold JSON beside the blocks. Live Previous and Fastest arrive on their own
  `live-lap-data` IPC channel as a JSON header plus a V6H1 Buffer, and are
  decoded from the store. History decodes run in a worker
  (`historyDecodeClient.ts`) that stops when idle. The engine delivers a backfill whose families are
  still subscribed even when newer requirements arrived meanwhile, since those
  would not ask for them again.
- **Qt:** live backfills already pass through `TnrdPlayer::decodeHistory`,
  whose V6H1 path (`decodeColumnarHistory`) now serves them too. Qt shows no
  race events from history.
- The store's bytes are counted once, under the recording writer's diagnostics.
  `live_history` / `native_live_history` show its figures with a zero
  `retained_bytes`.

## Threads

- The UDP thread queues each packet's events on `TnrdWriter`, as recording did.
- The writer thread owns the V6 writer: rows, the write delay, compression,
  the attached file, rewinds and image building.
- `LiveV6Store`'s read thread decodes images and runs read callbacks. A long
  All Laps read never holds up ingestion.
- Strategy's deep rollback blocks the Strategy worker until its image is built.

## Costs

- On the desktop hosts, live parsing always produces every family and the hot
  JSON rows, recording or not.
- Every car is held, not only the player.

These were accepted without measurement.

## Status

Implemented 2026-10-07; not built or run. `LiveHistoryStore` and its test were
removed.
