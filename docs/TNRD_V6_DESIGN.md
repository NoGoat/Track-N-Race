# TNRD V6 — Driver, Lap and Chart Data

Status: proposed for review; not implemented by this document.

## 1. What changes

Store recording data by **driver → lap → data type used by a chart or card**.

For example, a request for driver 7's tyre surface temperatures on lap 12 reads
that driver's lap-12 tyre-temperature chunk. It does not read the other drivers,
other laps, or the rest of the telemetry fields.

Each available `(driver, lap, data type)` has one independently compressed chunk
in the active recording. A completed lap is the chunk boundary; it is written
after leaving the three-lap replay buffer. There are no 30-second subdivisions
of a lap.

Keep one `.tnrd` file, Zstandard compression, a small directory, and the existing
background writer. No database, sidecar files, or new storage framework.

## 2. Current code and the gap

The current implementation already stores parsed JSON rows, rather than raw UDP
datagrams. V5 and the existing V6 group these rows by a single lap timeline and
broad row families such as telemetry, status, damage and timing.

The recorder checkpoints about every 30 seconds. Checkpointing writes and clears
the current builders, so one lap/family can have several physical chunks. There
is also a separate 30-second rolling buffer for flashbacks.

Existing V6 rows contain all-car arrays. Playback reads those arrays and projects
the selected car into the rows expected by the UI. This proposal separates the
cars and chart data before writing them.

Relevant code:

- [Recording pipeline](../protocol_parser_library/src/TnrdWriter.cpp)
- [Current V6 archive](../protocol_parser_library/src/tnrd/TNRD_V6.cpp)
- [Playback and driver projection](../protocol_parser_library/src/TnrdReader.cpp)
- [Parsed data](../protocol_parser_library/include/tnrp/rows.h)
- [Chart/card dependencies](../electron-frontend/src/renderer/src/lib/historyDependencies.ts)
- [Analysis metrics](../electron-frontend/src/renderer/src/lib/analyzeMetrics.ts)

## 3. Driver and lap ownership

Use the game's vehicle index to identify a driver within the session. Record all
valid participating vehicles, independent of the driver selected in the UI.
Do not hard-code a 20-car grid: supported packet formats have capacities of 22 or
24, while the actual number of participants varies.

Track each driver's lap independently using their lap data. Drivers can be on
different laps at the same session time. Never assign everyone to the player's
lap, or use race position as driver identity.

Each driver has an uncompressed header in the committed metadata, containing:

- Vehicle index, driver name, team and race number when known.
- Whether this is the recording player's car.
- Telemetry setting: `unknown`, `restricted` or `public`, initially as observed.
- Timestamped changes to that setting, stored only when it changes.
- Available data types and a reference to that driver's lap-summary table.

Read these headers and lap summaries when opening the file. Listing drivers,
showing restriction status and displaying their lap times must require no scan
or decompression of telemetry chunks. A header describes its driver; it does not
require that driver's chunks to be physically contiguous in the file.

The participant field `m_yourTelemetry` supplies the restriction setting. Keep
`unknown` until it is received. The setting can change during a session, so use
the header's small change list to answer availability at playback time. A
restricted recording player can still have their own private telemetry; the
setting alone is not a reason to discard it. Per-sample availability remains
authoritative for missing fields.

Each entry in the driver's lap-summary table contains:

- Driver index and a file-local lap ID.
- Game lap number for display.
- Start and end session times, and lap time when known.
- Sector times when known.
- Completion and validity flags.

This is the lap table used by the directory, not a duplicate catalogue. Populate
it during recording from lap/timing and available session-history updates;
unknown times remain unknown. The reader can find the fastest valid completed
lap by comparing these small summaries. No separate telemetry scan is needed.
Commit driver headers, lap summaries and the chunk directory together for the
saved timeline. Summaries and restriction changes for buffered time remain
editable in memory; flashbacks discard their abandoned future along with samples.

The file-local ID distinguishes separate attempts when the game reuses a lap
number, such as after returning to the garage. It is just an ID in the lap table.

Use session timestamps to assign samples to the driver's lap interval. Keep the
pending boundary open until the corresponding lap update has been processed;
samples already buffered after that boundary move to the new lap. Preserve the
precision of the received lap data rather than claiming an exact crossing time
when the game did not supply one.

Recording can begin or end halfway through a lap. Store the available portion and
mark it partial. Invalid laps still retain their data. Disconnecting, retiring,
or ending the session closes the driver's remaining partial lap.

Data before a known lap or between attempts belongs to an unassigned interval
for that driver, shown as lap 0. It must not be attached to a completed lap or
silently discarded. These intervals close on the next lap or recording stop.

## 4. Data types follow the consumers

A type is a small, fixed set of values used together by a chart or card. It is
independent of UDP packet IDs and frontend component names.

Store a measurement once. A card reads its latest value; a chart reads its
samples. Overview and Tyres can share the same temperature data. Changing a
chart's layout does not change the file format.

Proposed initial groups:

| Stored type | Values / consumers |
| --- | --- |
| Speed | Speed chart and speed card |
| RPM | RPM chart and rev information |
| Gear | Gear chart and card |
| Throttle | Accelerator chart |
| Brake | Brake chart |
| Steering | Steering chart |
| Aero | DRS / active aero state and availability |
| Tyre surface temperature | Four wheels; charts and tyre cards |
| Tyre inner temperature | Four wheels; charts and tyre cards |
| Brake temperature | Four wheels; charts and tyre cards |
| Engine temperature | Engine temperature card |
| Tyre wear | Four wheels; wear and derived remaining-life views |
| Tyre state | Compounds, age and available tyre-set information |
| Damage | Component damage and fault cards, excluding separately stored wear |
| Fuel | Fuel mass, remaining laps and fuel mode |
| ERS store | Stored energy, percentage and deployment mode |
| ERS harvest | MGU-K / MGU-H harvested energy where available |
| ERS deployment | Deployed energy |
| Engine power | ICE and MGU-K power |
| Brake bias | Brake-bias card |
| G-force | Lateral, longitudinal and vertical acceleration |
| Ride height | Front and rear ride height where available |
| Position | World position for maps and proximity views |
| Lap / timing | Lap progress, distance, sectors, position, gaps, pits and penalties |

Combined charts request several types. Analysis delta continues to be calculated
from lap progress. Strategy continues to use its underlying inputs; this change
does not introduce a second stored copy of every calculated strategy result.

The implementation must map every currently consumed field to a type before
replacing the old writer. This table establishes the grouping, not permission to
drop fields that are absent from the examples above.

## 5. Chunk contents and directory

Retain Zstandard-compressed JSONL for the first implementation. Each sample stores
its session time and that type's values for one driver. Driver, lap and type are
stored in the directory rather than repeated as all-car arrays in every sample.

Keep the existing units and useful sample precision. Do not downsample as part
of this format change. Types keep their natural update rates; a combined chart
does not require fuel and speed to be recorded at the same frequency.

For state that is recorded only when it changes, carry its last known value into
the new lap as an initial state. This makes a lap readable without scanning all
earlier laps. Never invent an initial value when none is known. A transition to
unavailable is also a state change and must clear any previously available value.

The directory identifies each chunk with driver index, lap ID and type ID, plus
its time bounds, file offset, compressed size, uncompressed size, sample count
and checksum. The lap table provides the driver's lap boundaries and summary.
Opening a file reads these tables and the driver headers without decompressing
sample chunks.

Use explicit type IDs and a list of requested types. The current row-family bit
mask is not the new type registry; do not keep extending its 32-bit capacity to
fit the new groups.

Retain a shared session area for session settings, weather, participant updates
and race events. These records keep their timestamps where applicable and are
stored once, independently of driver laps. They are not duplicated 24 times.

### Compression level

The current V6 writer explicitly uses **Zstandard level 3** in
[`TNRD_V6.cpp`](../protocol_parser_library/src/tnrd/TNRD_V6.cpp). Keep it as the
baseline and evaluate levels **5 and 7** on the proposed driver/lap/type chunks.
Level 5 is the first candidate, not an already proven replacement.

Higher levels generally exchange compression speed for smaller files;
decompression speed is usually similar across levels. This makes a modest
increase worth investigating for playback, but does not establish that recording
will remain equally fast. See the [upstream Zstandard performance guidance](https://github.com/facebook/zstd).

The new grouping changes chunk sizes and repetition, so results from current
all-car chunks cannot establish the best setting for this layout. Compare the
same representative new-layout payloads at levels 3, 5 and 7, using the same
library, checksum setting, reused compression context and writer thread count.
Include high-frequency charts, sparse state, partial laps and a full 24-car grid.

Measure total file bytes, compression time and CPU, peak writer memory, queue
delay during clustered lap completions, stop/flush time, and cold-cache chart
read/seek latency. Repeat runs to distinguish changes from timing noise. Verify
decompressed data matches the input exactly.

Proposed decision rule: adopt the lowest higher level that saves at least 10%
of total file size against level 3 while keeping playback and stop/flush latency
within 5% of baseline and producing no sustained writer backlog or dropped
updates on the minimum supported hardware. These are review targets, not measured
results. Report extra compression CPU and memory even if the application stays
responsive. If neither candidate meets the targets, retain level 3.

Use one fixed level for V6. No per-type tuning, dictionaries, adaptive levels or
additional compression workers are needed for this decision. The reader uses
the same Zstandard decode path regardless of the chosen level.

**Current conclusion:** a modest increase may be worthwhile, especially if disk
I/O dominates reads, but a performance-neutral improvement has not been measured.
The proposed layout is not implemented yet; this document does not claim benchmark
results or change the running writer's compression level.

## 6. Recording, saving and replay handling

Here, replay means an in-game flashback that rewinds the recording timeline.
Follow the UI's rolling-lap principle. For every driver, keep these laps
**uncompressed and unwritten**:

| Slot | Purpose |
| --- | --- |
| Current | The lap receiving new samples |
| Previous | The immediately preceding lap |
| Previous−1 | One additional lap as a replay safety net |

When a fourth lap starts for that driver:

1. Compress the old Previous−1 into its per-type chunks, append them to the file,
   and commit their metadata using the existing footer/checksum mechanism.
2. Retain those compressed bytes in the bounded memory cache and release their
   uncompressed samples. Cache eviction does not remove the file's copy.
3. Move Previous to Previous−1 and Current to Previous.
4. Make the arriving lap Current.

Only evict the old uncompressed lap after its write succeeds. Before three laps
exist, leave the unused slots empty. Recording all drivers remains independent
of the driver and charts selected in the UI.

There is no additional 30-second rolling buffer, periodic flushing of these lap
slots, V5 branch table, branch clipping, or replacement of already written laps.
Previous−1 exists to keep recent data editable across a lap boundary.

On a replay to session time `T`:

1. Check that `T` does not overlap already committed samples for any driver.
2. Apply the same timestamp to every driver's buffers, even though their lap
   numbers and boundaries differ.
3. Discard buffered samples at or after `T` and any abandoned newer laps.
4. Make the buffered lap containing `T` Current again. Retained earlier buffered
   laps occupy Previous and Previous−1 where available; resume normal rotation
   as new laps arrive.
5. Restore buffered lap summaries, initial state and shared state to the retained
   timeline, discarding future events and restriction changes as well.

Crossing from Current into Previous is therefore only an in-memory operation.
Nothing in the file is rewritten, and no committed lap is read back to rebuild
the buffer. The same rule works when the target is in the safety-net lap.

Keep shared timestamped records pending until they precede every driver's oldest
editable interval. This prevents a replay from leaving already saved future
events or restriction changes behind. Commit only the corresponding metadata;
live headers and summaries may include newer buffered information in memory.

The design targets the roughly 10-second replays described for this workflow.
Three laps are a lap-based safety margin, not an unlimited rewind guarantee. If
a target overlaps committed data, stop recording with a clear unsupported-rewind
message rather than silently corrupting the timeline or introducing branches.
Before the recording's first sample, there is simply no earlier data to restore.

Normal stop or session end writes the remaining buffered laps, preserving
completed-lap flags and marking unfinished laps partial. Opening the active
recording for file playback finalizes it first; do not save provisional laps and
then continue changing them in that same file. Ordinary playback seeking does
not edit the recording.

**Durability tradeoff:** a crash can lose Current, Previous and Previous−1 for
each driver, plus pending shared records. Only successfully committed laps are
durable. Unassigned lap-0 intervals also remain pending until safely outside the
editable window or recording ends. There is no periodic recovery snapshot scheme.

Measure the memory cost of three uncompressed laps per driver with a 24-car grid,
long laps and a long garage interval. Older cached laps stay compressed within
the existing cache budget. Do not add time-based chunks to reduce this buffer.

## 7. Reading and playback

The reader supports three straightforward requests:

- Read selected types for one driver's lap.
- Read selected types across that driver's laps overlapping a time range.
- Read latest state at a session time for selected drivers and types.

For a card, locate the relevant chunk and choose the last sample at or before the
requested time. For a chart, return the requested range. Whole-lap compression
means even a short range decompresses its selected lap/type chunk. Reuse the
existing bounded cache; no extra cache hierarchy is proposed.

A timing tower reads timing and required state for all participating drivers.
A map reads their positions. These requests use session time because drivers may
be on different laps. Driver headers supply initial names and teams; timestamped
participant updates and shared metadata supply changes and session context.

Update frontend dependencies from broad row families to these data types. The
reader/bridge must deliver independently updated fields without resetting other
values to zero. Rebuilding complete broad telemetry rows for every request would
undo the selective-read benefit. Apply this contract to Electron and Qt, including
seek history, lap analysis and driver switching.

Sequential playback merges requested samples by session time. Retain stable
ordering for equal timestamps. Export enumerates the stored types and preserves
driver/lap ownership instead of assuming the old row-family layout.

## 8. Availability and compatibility

All participating drivers receive storage for the data actually available for
them. This does not promise every chart for every driver.

The supplied F1 24, F1 25 and 2026 specifications describe player-only Motion Ex
data and restricted multiplayer fields. Ride height can therefore be unavailable
for other drivers; private fuel, ERS or damage values may also be unavailable.
Preserve availability explicitly. A restricted value must not appear as a genuine
zero, and a previous public value must not remain visible after access changes.

Protocol references checked: `PacketLapData`, `PacketParticipantsData`,
`PacketMotionExData`, and “Restricted data (Your Telemetry setting)” in
`f1-24-v27.2x.md`, `f1-25-v3.md`, and `f1-25-2026-season-8.md` under the project's
F1 UDP specification skill. In the 2026 reference, Motion Ex is on PDF page 16
and restricted data spans PDF pages 19–20. Packet capacities and active
participants are distinct.

Keep V1–V5 readers. Existing files cannot gain measurements they never recorded.

V6 is a development-only format and has not been released, including in a closed
beta. Replace its existing layout directly and retain the `TNRD_V6` name. Existing
development V6 files may stop loading; no legacy V6 reader, migration or revision
compatibility layer is required. Normal structural and checksum validation still
applies. This permission does not change V1–V5 compatibility.

## 9. Implementation scope and review criteria

Implementation touches the recorder, V6 directory/payload handling, reader,
frontend data requests, playback delivery and export. Existing parsers supply
most of the required values; ensure their all-driver output includes every field
needed by the consumers. This is not only a writer change.

The result is acceptable when:

- Every valid driver has their own lap catalogue, including partial attempts.
- Driver identity, restriction history and lap/sector times are available from
  headers and summaries without scanning telemetry chunks.
- A normal completed lap has one active chunk per available type, regardless of
  lap duration.
- A single chart reads only its required types, driver and laps, plus necessary
  shared context.
- Cards, multi-driver views, analysis, strategy, seek and export retain their
  required values and correct timestamps.
- Replays inside the three-lap buffer produce the retained timeline entirely in
  memory, including across driver-specific lap boundaries; no file rewrites or
  V5 branch handling are involved.
- Replays overlapping committed data stop recording explicitly instead of
  appending a contradictory timeline.
- Missing/restricted data stays distinguishable from zero.
- V1–V5 recordings remain readable; compatibility with old development V6 files
  is intentionally not required.
- A higher compression level is selected only if measured savings justify its
  recording cost and it meets the stated performance targets; otherwise use 3.
- Measurements cover file size, writer memory, chart reads, seeks and whole-grid
  views. Selective reads should improve; total file size and every workload are
  not assumed to improve automatically.

This document proposes the format and its tradeoffs. It does not authorize or
include implementation changes, builds, or new test cases.
