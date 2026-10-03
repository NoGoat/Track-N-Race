# TNRD V6 Columnar Lap Builders

Status: implemented 2026-10-03; not yet built or measured (see §7).
Scope: the in-memory lap builders of the V6 writer
(`protocol_parser_library/src/tnrd/TNRD_V6.cpp`, `TnrdV6Writer::Impl`) and the
writer memory report. No reader, frontend or UDP parsing change.
Baseline: `ram_usage.log`, live session recorded 2026-10-02 (FP1, then a second
session), sampled every second.

## 0. Summary

The V6 writer holds every open lap in memory until the lap closes and its
30 s `WRITE_DELAY` passes. Each sample is stored as a `V6Sample`: a `float`
plus its own heap-allocated `std::vector<V6Field>`, where every field is a
56-byte `string_view` + `variant`. A one-value sample such as speed costs
about 100 bytes for 2 bytes of information.

This change replaces that row-of-variants representation with per-type
column builders that hold values at the same narrowed widths the chunk
encoder already writes. Estimated effect on the FP1 recording: open-lap
memory drops by roughly 15x (about 1.1 GB to about 75 MB), and most of the
~750 MB of unattributed heap overhead goes away with the per-sample
allocations.

**Effect on V6 files: none.** The builder emits exactly the bytes the
row-based encoder emitted. Chunk payloads, CRCs, the directory, metadata,
the recovery scan and every reader are unchanged. No version bump.

Two constraints from the product side shape this design:

- No data may be dropped. Every sample `add()` accepts today is stored, at
  full precision.
- Laps stay the only chunk boundary. This design does not cap the memory of a
  long open lap; it makes each sample about 15x cheaper. A lap still grows
  linearly for as long as it is open.

## 1. The problem, measured

`ram_usage.log` attributes the growth to the Electron main process. The V8
heap stays at ~7 MB; the rest is native. The V6 writer's builders
(reported under the `v5` keys in that log, see §8) dominate:

| Time | Process private | Writer retained | Open-lap builder bytes | Laps committed |
|---|---|---|---|---|
| 16:07 before FP1 | 313 MB | 0 | 0 | 0 |
| 16:08 recording opens | 436 MB | 45 MB | 43 MB | 0 |
| 16:13 | 1,545 MB | 881 MB | 824 MB | 0 |
| 16:18 | 2,176 MB | 1,255 MB | 1,188 MB | 20 |
| 16:22 peak | 2,576 MB | 1,544 MB | 935 MB | 48 |
| 16:28 | 2,555 MB | 974 MB | 881 MB | 84 |
| 16:29 FP1 closes | 812 → 394 MB | 0 | 0 | — |
| 16:36–17:27 next session | ~1,250–1,370 MB | 250–500 MB | 150–330 MB | up to 382 |

Three things stand out:

1. **FP1 held ~870 MB before a single lap closed.** In practice and
   qualifying, `garageHold` (`TNRD_V6.cpp:1283`) keeps a car in a lap-0
   interval through the garage and the out-lap. That interval is one open lap
   for as long as the car stays in, so 22 cars' full telemetry piles up.
2. **The in-memory form is far larger than the file.** The whole FP1 session
   encoded to ~270 MB of chunk payload (`plain_bytes_processed`), and the
   writer held up to 1.19 GB at once.
3. **Process private memory runs ~750 MB above the writer's own figure.**
   `memoryStats()` counts `sizeof` and capacity, not allocator headers or
   fragmentation. With one heap block per sample, that overhead is large, and
   the heap does not return it when laps flush (16:27: builder bytes fell to
   685 MB and private memory stayed at 2.54 GB).

## 2. Where the bytes go today

`Impl::Builder` (`TNRD_V6.cpp:714`):

```cpp
struct Builder { std::vector<V6Sample> samples; float first, last; uint32_t count; };
struct V6Sample { float time; std::vector<V6Field> fields; };
struct V6Field  { std::string_view key; std::variant<int64_t,double,bool,std::string> value; };
```

On MSVC x64, `std::string` is 32 bytes, so the variant is 40, a `V6Field`
56 and a `V6Sample` 32. Each sample also owns one heap block for its field
vector (about 16 bytes of allocator overhead). Per sample, compared with the
width the chunk encoder actually writes:

| Type | Fields | Today (struct + heap) | Columnar at encoded width | Ratio |
|---|---|---|---|---|
| Speed | 1 int (int16) | ~104 B | 4 + 2 = 6 B | ~17x |
| Gear, EngineTemp | 1 int (int8/16) | ~104 B | 5–6 B | ~18x |
| Throttle, Brake, Steering | 1 float32 | ~104 B | 8 B | ~13x |
| RPM | 3 ints | ~216 B | ~9 B | ~24x |
| Tyre surface / inner / brake temp | 4 ints (int16) | ~272 B | 12 B | ~23x |
| Position | 2 float32 | ~160 B | 12 B | ~13x |
| GForce | 3 float32 | ~216 B | 16 B | ~13x |
| LapTiming | 16–17 mixed | ~1,000 B | ~30 B | ~33x |

Averaged over the twelve per-packet telemetry samples, that is ~169 B against
~9.5 B, or about 18x. Against the figure `memoryStats()` reports (which leaves
out allocator overhead) it is about 16x. These are estimates from struct
sizes, not measurements; §7 says how to measure them.

Applied to the log, assuming the same mix of types:

| | Today | Columnar (estimate) |
|---|---|---|
| FP1 open-lap builder bytes, peak | 1.19 GB | ~75 MB |
| Unattributed heap overhead, FP1 | ~750 MB | small: a few blocks per column, not one per sample |
| Next session, open-lap builder bytes | 150–330 MB | ~10–20 MB |

## 3. Design

### 3.1 Column builder

`Impl::Builder` keeps its role and its `first`, `last`, `count` fields. Its
storage becomes a `SampleColumns`, defined above the reader in
`TNRD_V6.cpp`:

```cpp
class SampleColumns {
    struct Column {
        std::string_view key;              // string literal, same as V6Field::key
        Store store;                       // Int, Float32, Float64, Bool, Text, Variant
        uint8_t width;                     // bytes per packed value
        bool sparse;                       // some covered row has no value
        uint32_t covered, count;           // rows decided, values present
        std::vector<uint8_t> packed;       // present values, little endian
        std::vector<std::string> texts;    // Text (tyre-set JSON)
        std::vector<V6Value> variants;     // Variant (mixed kinds; never written today)
        std::vector<uint64_t> present;     // one bit per covered row while sparse
    };
    struct RowOrder { uint32_t row; std::vector<uint32_t> columns; };
    std::vector<float> time_;              // one per row
    std::vector<Column> columns_;          // in order of first appearance
    std::vector<RowOrder> orders_;         // rows not listed in column order
};
```

Values are stored only for rows that have them, just as the file stores them,
so a sparse column costs only its bitmap and its present values.

### 3.2 Append

`add()` takes the fields without building a `V6Sample` for them:

```cpp
void add(uint8_t index, V6DataType type, float time,
         std::initializer_list<V6Field> fields, bool updateState = true);
void addFields(uint8_t index, V6DataType type, float time,
               std::span<const V6Field> fields, bool updateState = true);
```

The call sites in `appendRow` changed from `add(i, T, time, sample(time, {...}))`
to `add(i, T, time, {...})`. The `std::vector<V6Field>` call sites (RPM,
LapTiming, TyreWear, Damage) call `addFields`. Two names, rather than two
overloads, mean a braced list can never be ambiguous between them. `sample()`
and `retime()` are gone, and so is the heap allocation per sample.

For each field the builder finds its column. Every field list is built in a
fixed order, so the column after the previous field's column is checked first,
with a linear search as the fallback. A new key appends a column. Presence is
lazy: a column records how many rows it has covered, and only builds a bitmap
at its first gap. A column with no gaps has no bitmap.

**Integers** are stored at the narrowest of 1/2/4/8 bytes that holds every
value so far, including 0, matching `intWidth()`. A value outside the current
width widens the column in place, which happens at most three times per column
per lap.

**Floats** are stored as float32 while every value survives
`double → float → double` unchanged, or is non-finite. This is the test the
encoder always applied. The first value that fails it widens the column to
float64.

**Bools** take one byte. **Strings** (`rawJson`, tyre sets only) go in
`texts`. **A value whose kind differs from the column's first kind** turns the
column into `Variant` storage, which keeps the encoder's lossless Json
fallback. No current call site triggers it.

**A key repeated within one sample** keeps the last value, as the old encoder
did. No call site does this either.

State types (Aero, TyreState, BrakeBias) still need a `V6Sample` for the
`lastState` dedupe. `addFields()` builds one only for those types. They are
edge-encoded, so this is rare.

### 3.3 Row order

The encoder orders columns by first appearance: by row, then by position
within the row. For a whole lap, the order the columns were created in is
exactly that. After a split, though, the order is taken over a subset of rows,
and that subset can start with a row that lists two keys in the opposite order
to the column order. For example, a non-player Damage sample can carry
`tyre_dmg_fr` without `tyre_dmg_fl`, and a later one carry both.

To reproduce the encoder's choice exactly, `append()` checks whether each row
lists its fields in increasing column order. A row that does needs no extra
storage. A row that doesn't is recorded in `orders_` with its column sequence.
The add() call sites list their keys in a fixed order, so these exceptions are
rare.

With this, `forEachRow()` gives back every row's fields in the exact order
they were appended.

### 3.4 Encode

`SampleColumns::encode()` replaces `encodeColumnar(const std::vector<V6Sample>&)`
and writes the V6C1 layout directly from the columns:

- the time column, byte-planed;
- descriptors in column order;
- each body: the bitmap when not dense, then the present values byte-planed at
  the encoded width.

To produce the same bytes as before, the encoded width is **recomputed from
the present values at encode time**, not taken from the storage width. After a
split the half that stays may fit a narrower width than the column was widened
to. The same applies to Float32 against Float64, and to a Variant column whose
remaining values all have one kind. Int width includes 0 in its range. Json
columns render through `appendValue`, unchanged.

This also removes a transient cost: the old encoder allocated a
`std::vector<const V6Value*>(rows)` per column (8 bytes × rows × columns) at
every chunk write.

### 3.5 Split

`SampleColumns::splitAt(boundary)` moves rows with `time >= boundary` into a
new builder and keeps the rest, **preserving relative order in both**. Rows
are not guaranteed to be in time order: a timed lap's boundary is set
`current_lap_ms` in the past, and rewind splits reopened laps.

- **Nothing moves:** return an empty builder.
- **Everything moves:** swap.
- **The moved rows are a suffix** (the usual lap-boundary case): replay the
  suffix rows into the new builder, then truncate this one. Columns are created
  in order of first appearance, so the columns a truncation leaves empty are a
  suffix, and dropping them does not renumber the rest.
- **Otherwise:** replay every row into a kept or a moved builder. Replaying
  through `append()` recomputes widths, kinds, presence and row order for each
  side.

`Impl::splitAt` then recomputes `first`, `last` and `count` for both sides from
the time column, as before.

### 3.6 Row view for state types

`commit()` and `rewind()`'s `consider` walk the rows of state types to rebuild
`committedState` and `lastState`, using `forEachRow()`. Rows come back in
their appended order (§3.3), so the rebuilt `V6Sample` compares equal to the
one `addFields()` received, and the order-sensitive dedupe is unchanged.

### 3.7 Lap seeding and the unavailable marker

`startLap()` re-adds each `lastState` value at the new lap's start time
through `addFields()`. `unavailable()` adds `{available: false}` to nine types
from a one-element array. No behaviour changes.

### 3.8 Memory report

`TnrdV6Writer::memoryStats()` takes builder bytes from
`SampleColumns::bytes()`:

- used bytes: time, column records, packed values, bitmap words, strings and
  row-order exceptions;
- capacity bytes: the same sums over capacity.

`retainedBytes` keeps its definition. With far fewer allocations, the gap
between `retainedBytes` and process private memory should shrink. That gap is
the check on §1 point 3.

## 4. What does not change

- The chunk payload format (V6C1), flags, prefix, directory, metadata, CRCs
  and recovery. Encoded bytes are identical.
- Lap boundaries, `garageHold`, phases, `WRITE_DELAY`, commit order, rewind
  semantics.
- Which samples are stored. Nothing is thinned or dropped. State-type dedupe
  stays exactly as it is.
- The reader, live history, Strategy, frontends.

## 5. Costs and risks

| Risk | Mitigation |
|---|---|
| Memory still grows with the length of an open lap. A car held in the garage for an hour still accumulates an hour of telemetry. | Accepted. At ~10 B per sample this is ~15x slower growth. |
| `splitAt`, `commit` and `rewind` are the paths behind lap edges, flashbacks and state restore. A bug shows up as wrong playback, not a crash. | Splits other than the suffix case go through the same `append()` as live data, and the suffix case only truncates. The tests in §7.1 cover them. |
| Field lookup on every `add()` is on the per-datagram path. | Positional hint makes the common case one `string_view` compare. The per-sample heap allocation it replaces cost more. |
| Column vectors grow geometrically: up to 2x capacity slack, and a copy at each reallocation. | Columns are small. At 60 Hz a 20-minute hold is 72,000 rows, so a 2-byte column is 144 KB. Reallocation spikes stay small. Slack is reported as capacity. |
| Encode must reproduce the old width choices exactly, including the 0 in the integer range and the non-finite floats rule. | Encode recomputes widths from present values with the same rules (§3.4). |

## 6. Implementation

Implemented 2026-10-03 in `protocol_parser_library/src/tnrd/TNRD_V6.cpp`, in
one step, with no compile flag and no parallel old path:

1. `SampleColumns` replaces `encodeColumnar(const std::vector<V6Sample>&)`.
2. `add()` / `addFields()` replace `add(…, V6Sample, …)`, and every call site
   passes its fields directly. `sample()` and `retime()` are removed.
3. `splitAt`, the `commit` and `rewind` state walks, and `memoryStats` use the
   column builder.

## 7. Validation

Not yet built or run. Shadow verification, which would have kept the old
builders alongside the new ones and compared every chunk, was dropped by
decision.

### 7.1 Tests

The existing `TnrdV6Recovery` suite must pass unchanged. New cases worth
adding:

- integer widening across 1/2/4/8, and the encoded width shrinking back after
  a split;
- float32 widening to float64, including NaN, infinity and -0.0;
- sparse columns: a column first appearing mid-lap, and Damage's optional
  fields;
- a row listing its keys out of column order, split so that the row starts
  the subset (§3.3);
- a mixed-kind column falling back to Json;
- split with out-of-order times, with every row on one side, and with a suffix;
- row view equal to the original `V6Sample` for every state-type group (§3.6).

### 7.2 Memory

Repeat a practice session and compare `ram_usage.log` against §1, looking at:

- open-lap builder bytes;
- process private memory;
- the gap between them.

## 8. Writer stat names

When this log was recorded, `TnrdWriter::memoryStats()` copied the V6 writer's
statistics into fields named `v5*`, and the addon reported them under `v5` and
`v5_append_activity`. Those names are now `v6*`, `v6` and
`v6_append_activity`, so logs recorded after this change use the V6 names.
