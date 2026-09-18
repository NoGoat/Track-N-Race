# TNRD V6 — Driver, Lap and Chart Data

Status: proposed for review; not implemented by this document.

## 1. What changes

Store recording data by **driver → lap → data type used by a chart or card**.

For example, a request for driver 7's tyre surface temperatures on lap 12 reads
that driver's lap-12 tyre-temperature chunk. It does not read the other drivers,
other laps, or the rest of the telemetry fields.

Each available `(driver, lap, data type)` has one independently compressed chunk
in the active recording. A completed lap is the chunk boundary; it is written
once session time has advanced 30 seconds beyond its end. This is a write delay,
not a subdivision: each lap/type remains one chunk.

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

### Packet ownership: shared versus per driver

This table maps **decoded packet contents** to storage. It does not reintroduce
whole UDP packets as chunks. An array containing all cars is split by vehicle
index; it is not shared just because it arrived in one datagram.

**Shared** means stored once in session metadata or the timestamped session/event
stream. **Per driver** means stored in that driver's header, summaries or
lap/type chunks. Header/summary data is not duplicated into every lap chunk.

| UDP ID | Packet | Ownership | Destination |
| --- | --- | --- | --- |
| 0 | Motion | Per driver | Split the car array into each driver's Position and G-force data. |
| 1 | Session | Shared | Track, weather/forecast, session rules, phase, safety-car state and session settings. Player-specific settings in this packet retain their player context; they are not attributed to every driver. |
| 2 | Lap Data | Primarily per driver | Split timing, lap progress, sectors, gaps, pits and penalties into each driver's Lap / timing data and lap summaries. The packet's time-trial PB/rival index selectors are shared comparison context. |
| 3 | Event | Shared | Store each event once with its timestamp and any referenced driver indices. Penalties, retirements and collisions remain single events, even when they concern one or two drivers. `FLBK` drives the pending-buffer rewind; formation/safety-car events drive phase state. |
| 4 | Participants | Mixed metadata | Each participant's identity and telemetry restriction setting populate their driver header. Keep timestamped identity/team changes once as shared participant updates, keyed by driver index; restriction changes live in the header's change list. Active-car count and roster context are shared. |
| 5 | Car Setups | Per driver | Each car's setup belongs to that driver. The next-front-wing value belongs only to the player. If retained, setup state uses a driver data type, not a shared all-car payload. |
| 6 | Car Telemetry | Per driver | Split speed, RPM, gear, controls, temperatures and related values into the chart/card types. Player/secondary-player MFD fields and suggested gear retain their actual player ownership if retained. |
| 7 | Car Status | Per driver | Split fuel, ERS, power, compounds, tyre age, brake bias and aero availability into the corresponding driver types. |
| 8 | Final Classification | Per driver, with shared envelope | Store each driver's final result, best lap, penalties and tyre-stint summary in driver metadata. The classification count is shared. A results table reads these summaries without a duplicate all-car payload. |
| 9 | Lobby Info | Shared | Session/lobby roster and readiness context. Lobby slots are not assumed to be race vehicle indices; use race Participants data to establish driver headers. |
| 10 | Car Damage | Per driver | Split the car array into each driver's Tyre wear and Damage data. |
| 11 | Session History | Per driver | Route by the explicit `m_carIdx`, not the header's player index. Update that driver's lap/sector summaries, best-lap information and tyre-stint history. Do not save the repeated full history array in every current-lap chunk. |
| 12 | Tyre Sets | Per driver | Route by explicit `m_carIdx` into that driver's Tyre state. A packet for another car must not be labelled as the player or broadcast to all drivers. |
| 13 | Motion Ex | Player only | Store available extended motion, including ride height, under the recording player. No copies or fabricated equivalents for other drivers. |
| 14 | Time Trial | Shared comparison metadata | Preserve the separate player-session-best, personal-best and rival roles, including their car references. External PB/rival results are comparison records, not recorded laps with telemetry chunks. |
| 15 | Lap Positions | Per driver | Split the lap/vehicle matrix into each driver's lap-position summaries using the packet's lap-start index. Merge history updates without storing a repeated matrix in every lap. |
| 16 | Car Telemetry 2 | Per driver | Split active-aero, overtake and other supplied car flags into their relevant driver types. This packet is available only in the supplied 2026 format. |

Packets 0–14 occur in the supplied F1 24, F1 25 and 2026 formats. Lap Positions
(15) is present in F1 25 and 2026; Car Telemetry 2 (16) is present in 2026.
Decode each format's own layout and car capacity before applying this ownership
mapping. Privacy and player-only restrictions still apply.

The table defines destinations, not a claim that every packet field is already
recorded. Current parsers do not emit recording rows for Car Setups, Final
Classification, Lobby Info, Time Trial or Lap Positions. Current Tyre Sets parsing
filters to the player, Session History exposes only selected summary fields, and
the 2026 Telemetry 2 path currently retains active-aero mode. Driver headers and
all-driver lap summaries require the relevant parser output to be extended.
Unused packet families need no new chunks solely to fill this table; when their
data is exposed to a consumer, use the ownership shown here.

Shared views do not imply shared telemetry storage: the timing tower, results
table and track map combine per-driver data when reading. Session events can be
filtered by driver without copying them into that driver's lap chunks. All
timestamped data uses the same delayed-commit and replay rules below.

Sources: packet ID tables and the corresponding `Packet*Data` structures in the
supplied `f1-24-v27.2x.md`, `f1-25-v3.md` and
`f1-25-2026-season-8.md` references. In the 2026 document, packet IDs are on PDF
pages 2–3, Session History/Tyre Sets on pages 14–15, and Time Trial/Lap Positions/
Telemetry 2 on pages 16–18.

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
For each driver, keep the current lap uncompressed. When that lap ends, keep its
chunks uncompressed and unwritten for another **30 seconds of session time**.

For a lap ending at `lapEndTime`, it becomes eligible to write when:

```text
currentSessionTime >= lapEndTime + 30 seconds
```

Use the accepted recording timeline, not wall-clock time or a timer callback.
Pausing the game must not expire the delay. Process a flashback before evaluating
pending writes, and evaluate deadlines against the rewound session time.

Once eligible, compress the complete lap into its per-type chunks, append them,
and commit their metadata using the existing footer/checksum mechanism. Release
the uncompressed samples only after the write succeeds. Any retained compressed
copy uses the bounded cache; the recorder need not keep all saved laps in memory.

The buffer contains the current lap and however many completed laps are still
waiting for their deadlines. There is no fixed three-lap rotation, additional
rolling packet buffer, V5 branch table, branch clipping, or replacement of written
laps. Recording all drivers remains independent of the UI's selected driver and
visible charts.

On a replay to session time `T`:

1. Check that `T` does not overlap already committed samples for any driver.
2. Apply the same timestamp to every driver's buffers, even though their lap
   numbers and boundaries differ.
3. Discard buffered samples at or after `T` and any abandoned newer laps.
4. Reopen the buffered lap containing `T` and cancel its old write deadline.
   Assign a new deadline when it finishes again. Earlier pending laps retain
   their end times, but eligibility uses the current, rewound timeline.
5. Restore buffered lap summaries, initial state and shared state to the retained
   timeline, discarding future events and restriction changes as well.

Crossing a lap boundary is therefore only an in-memory operation while the lap
is pending. Nothing in the file is rewritten, and no committed lap is read back
to rebuild the buffer.

Keep shared events and restriction changes pending for the same session-time
window. Commit only records strictly older than `currentSessionTime - 30`, so a
rewind to the exact boundary can still replace records at that timestamp.
Lap sample intervals exclude the next lap's start. Header summaries committed
with a lap must describe its saved data, not subsequent buffered state. Live
headers and summaries may include newer information in memory.

If a target overlaps any committed driver data or shared record, stop recording
with a clear unsupported-rewind message rather than silently corrupting the
timeline or introducing branches. This also covers repeated replays that reach
progressively older data. Before the recording's first sample, there is simply
no earlier data to restore.

Normal stop or session end writes the remaining buffered laps, preserving
completed-lap flags and marking unfinished laps partial. Opening the active
recording for file playback finalizes it first; do not save provisional laps and
then continue changing them in that same file. Ordinary playback seeking does
not edit the recording.

**Durability tradeoff:** a crash can lose the current partial lap, completed laps
still awaiting their 30-second deadline, pending writes and pending shared
records. This reduces exposure compared with retaining three entire laps, but
does not limit total data loss to 30 seconds: the current lap may be much longer.
Closed unassigned lap-0 intervals follow the same delay; open intervals remain
pending until they close or recording ends. There is no periodic snapshot scheme.

Measure memory use with a 24-car grid, long laps and a long garage interval. Do
not split laps into time-based chunks to reduce the pending buffer.

### Evidence from the raw capture

The inspected `udp_capture.bin` and `udp_capture_sao_paulo.bin` under
`tools/TNR stuff/udp sender and recorder` are byte-for-byte identical: 2,518,182,672
bytes containing 2,498,862 packets. The scan found:

- 50 explicit `FLBK` events.
- A longest rewind of exactly 15 seconds, twice, on player laps 24 and 59.
- 14 rewinds longer than 10 seconds; none longer than 30 seconds.
- Four player lap-boundary crossings, including lap 44 → 43 at 12.824 seconds.

Durations use the event header's session time minus its `flashbackSessionTime`,
cross-checked against subsequent lap packets. This supports a 30-second delay
for the observed flashbacks; it does not establish a universal game limit.

### Formation lap → race start

The same capture resets session time from 128.327972 to 0 at the end of the
formation lap. This is not a replay:

- Before the reset, `m_safetyCarStatus = 3` (formation lap).
- At the reset, `SCAR` reports `safetyCarType = 3` (formation-lap safety car)
  and `eventType = 3` (resume race).
- Start-light events follow and safety-car status becomes 0. The session UID
  remains unchanged and frame numbers continue forward.

These meanings are defined by `PacketSessionData` and `EventDataDetails.SafetyCar`
in the supplied `f1-25-2026-season-8.md` specification (PDF pages 4 and 8).

Recognise this transition from phase/event context before treating a backward
timestamp as a replay. Close the formation interval separately from race lap 1;
do not truncate it as abandoned future data. Mark formation and race intervals
with their phase in metadata, including shared timestamped records, so their
overlapping raw session times stay distinguishable. File-local lap IDs remain
unique. Playback orders formation before race and evaluates ranges within phase.

Start race timing and lap tracking at the new zero. The closed formation interval
becomes eligible to save after 30 seconds of race session time (or on normal
stop). Do not compare its old 128.328-second endpoint with the race clock. A
confirmed phase transition needs no file rewrite or rewind branch.

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
- Completed laps stay pending until session time advances 30 seconds beyond
  their end; pauses do not expire that delay.
- Replays within pending data restore the timeline entirely in memory, including
  across driver-specific lap boundaries; no file rewrites or V5 branches.
- Formation-lap clock resets preserve formation data separately from race lap 1
  and do not trigger flashback handling.
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
