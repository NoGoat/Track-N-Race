# Stint Page Design

Status: Reworked in source; native build and application preview pending  
Last updated: 2026-10-05  
Scope: Electron first, with shared data semantics suitable for a later Qt port

## 1. Purpose and confirmed decisions

Add a page provisionally named **Stint** for reviewing the selected driver's
current tyre stint in live telemetry and recorded-session playback. Follow the
supplied sketch and the existing Electron design language.

- The top strip shows Stint Laps, Wear/Lap, Rec/Lap, ERS/Lap, Fuel/Lap, FL, PL,
  and the current tyre.
- FL is the fastest valid completed lap in the current stint.
- PL is the previous completed lap's time, even if invalid; identify invalidity.
- Lap times are a graph, not a table.
- ERS usage and recharge are separate graphs with one value per lap of the stint.
- Recharge is total harvested MGU-K plus MGU-H energy. Do not split the sources.
- Consumption averages include completed invalid laps and in/out laps when
  measurements are available. Exclude the unfinished lap from averages.
- Reuse the Tyres page's four-wheel wear/life graph presentation.
- The right sidebar arranges body damage spatially, with Engine and Gearbox
  percentages below. These are presented as wear percentages using the existing
  engine/gearbox damage fields; individual engine-component wear is deferred.
- All panels follow the streamed driver: the player in live, and the driver
  selector's car in V6 playback. There is no driver selector in live.
- The page updates in realtime, as rows arrive; nothing polls.
- No Qt UI implementation is part of the first delivery.

## 2. Layout and visual language

The summary strip spans the full page width. Below it, use a broad main column
and a narrower right sidebar, separated by the existing panel border treatment.

```text
Stint Laps | Wear/Lap | Rec/Lap | ERS/Lap | Fuel/Lap | FL | PL | Tyre
-----------------------------------------------------------------
Lap times: current stint                         | Wing L  Wing R
                                                 |     Floor
-------------------------------------------------| Sidepod Diffuser
ERS usage: per lap      | Recharge: per lap       |    Rear wing
                        |                        |---------------
-------------------------------------------------| Engine
Tyre wear / life: current stint or all laps       | Gearbox
```

Use Cascadia Code, tabular numeric values, compact uppercase labels, existing
theme variables, compound colours, and thin dividers. Support dark, midnight,
and light themes. Use the chart wrappers, axes, tooltips, and colour adaptation
already used by other Electron pages. Avoid adding a separate visual system.
Place clickable line-swatch legends at the top right of each chart header.
Keep chart controls beside the title and use the shared animated select,
select styles, and indicators with menus portalled outside the panel.

Fit the available window height without page scrolling. Keep the summary in
one row, the condition sidebar beside the charts, and the two ERS charts side
by side. Chart rows and condition cells share the remaining height using
shrinkable grid tracks rather than fixed minimum heights.

## 3. Metric definitions

| Metric | Definition | Display |
|---|---|---|
| Stint Laps | Number of completed laps assigned to the current stint; not the tyre set's lifetime age | Integer |
| Wear/Lap | Existing fitted tyre set's session wear-per-lap calculation, as on the Tyres page | Percentage points/lap, two decimals |
| Rec/Lap | Arithmetic mean of measured harvested energy for completed stint laps | MJ/lap, two decimals |
| ERS/Lap | Arithmetic mean of measured deployed energy for completed stint laps | MJ/lap, two decimals |
| Fuel/Lap | Arithmetic mean of measured fuel consumption for completed stint laps | kg/lap, two decimals |
| FL | Minimum valid completed lap time within the current stint | Existing lap-time format |
| PL | Most recent completed lap time within the current stint | Existing lap-time format |
| Tyre | Current visual compound using format-aware labels and colours | Existing compound presentation |

### Wear/Lap compatibility

`ApplyTyreSetSessionWear` already calculates `avg_wear_per_lap` per car and tyre
set from `(current wear - baseline wear) / (baseline life span - current life span)`.
The baseline resets when first seen, when compound changes, when wear falls
below baseline, or when life span increases. Merely refitting a previously used
set does not explicitly reset it.

For a set used continuously since its observed baseline, this serves the usual
stint-rate use case. If the same set is used in multiple stints, it can retain
earlier session usage. The first implementation should reuse the existing value
and semantics so this page agrees with the Tyres page. Do not introduce an
independent competing wear formula. If strict stint-only wear is later desired,
change the shared definition deliberately for both pages.

### Lap measurements

Harvested energy is the sum of the two reported lap counters; deployed energy
uses the reported deployment counter. Convert joules to MJ by dividing by
1,000,000. These measure energy harvested and deployed, not the battery's net
change. Respect the selected format/Formula's capabilities without hard-coded
F1 24/25 harvest limits.

Associate status samples with each driver's lap timing. Preserve the last
reliable pre-reset counter sample for the completed lap. Do not interpret the
next lap's reset value as a zero-energy completed lap. UDP does not supply a
separate final lap-energy record: sampled totals can slightly undercount, and
missing boundary samples must not be described as exact totals.
Because ERS fields are cumulative lap totals, measuring them does not require
an early-lap sample. Process deployment and harvest resets independently, and
accept a reset immediately after lap timing advances as the new lap's counter.

Fuel consumption is the difference between fuel mass at consecutive lap
boundaries. Refuelling, resets, missing boundaries, and discontinuities invalidate
the affected measurement; do not convert negative differences to zero use.

Each metric averages only its own measurable completed laps. Missing data is
not zero. Tooltips should report the contributing lap count when coverage is
partial. Include invalid and pit laps when measurements are available.

## 4. Graph behaviour

- **Lap times:** completed laps in the current stint, with session lap numbers
  on the X axis and formatted lap time on the Y axis. Show invalid lap points
  with an explicit marker and tooltip, and highlight the stint's fastest valid
  lap. Leave missing lap values as gaps.
- **ERS usage and recharge:** completed stint laps on the X axis and MJ on the
  Y axis, one sampled total per lap, plus the lap in progress as a provisional
  point that rises live. Keep matching lap domains between the two graphs. Each
  Y axis starts at zero and ends at its current maximum plus 0.5 MJ. The lap in
  progress never counts towards the averages.
- **Tyre wear/life:** reuse `TyreTrendCharts` presentation, four-corner colours,
  wear/life transform, tooltips, and 0–100% range. Default to current stint;
  provide the existing Stint Laps / All Laps range semantics. The range switch
  affects this graph only; summary metrics and the other graphs remain scoped
  to the current stint. Preserve tyre-change discontinuities in All Laps view.
- **Sidebar:** latest values at the live time or playback cursor. Use wing
  left/right, floor, sidepod, diffuser, and rear wing in the sketch's positions,
  then Engine and Gearbox. Reuse percentage formatting and colour rules.

## 5. Driver, stint, and timeline model

The page shows the streamed driver: the player in live, the V6 driver
selector's car in playback. Its rows already arrive as the main status, damage,
lap and tyre-set rows, so no per-car aggregation exists for this page.

Stint boundaries come only from Session History tyre stints (live: the packet;
V6 playback: the driver header, clamped to the lap in progress at the cursor).
The in-lap belongs to the outgoing stint. `tnrp/TyreStints.h` is the single
definition, also used by TnrdReader's tyre-state reconstruction.

At playback time, the current stint means the stint in force at the cursor.
Exclude future completed laps from counts, FL, PL, averages, and graphs. A driver
switch must replace the whole page's snapshot atomically after the required
history is ready. Seek, flashback, restart, and session change must rebuild or
trim measurements for the surviving timeline, rather than retain stale totals.

## 6. Data ownership and integration

Reuse the existing pipelines; the page adds no transport of its own.

- **Completed laps and stint start:** the `driver_lap_history` push, extended
  with `stint_start_lap`. The engine pushes it when Session History changes
  (live), and on load, seek and each lap end (playback). The page claims the
  lap-history car through `lib/lapHistoryCar.ts`, shared with the laps dialog.
- **Per-lap ERS and fuel:** `lib/stintMeasures.ts`, an incremental scan of the
  full-session status history (the existing All Laps history pipeline) against
  the store's lap boundaries. A counter drop closes the lap whose line is
  nearest; MGU-K and MGU-H are tracked separately and summed per lap. Fuel is
  the difference at the two lap lines, discarded if fuel rises in between. The
  same code runs live and in playback.
- **Wear/Lap, tyre, damage:** current `tyreSets`, `status` and `damage` rows.
- **Tyre graph:** the Tyres page wear chart over the store's damage history,
  cropped by Stint Laps / All Laps with the Session History stint start.

AppShell requests full-session status and damage history while the page is
open, regardless of the title-bar chart window.

## 7. Availability and protocol evidence

The feature is supported by the supplied F1 24, F1 25, and 2026 Season Pack
specifications, with these protocol limits:

- Restricted multiplayer opponents have fuel, ERS, tyre wear, and requested
  damage values zeroed. Use participant telemetry access plus local-player
  exemption to distinguish unavailable values from real zero values.
- Lap times and tyre compound remain available under the documented restriction
  list. Keep those panels useful when private metrics are unavailable.
- Joining mid-stint cannot recover historical fuel/ERS totals from Session
  History. Show averages over measured completed laps with partial coverage.
- Session History contains up to 100 laps and eight tyre stints. Retained native
  history must not assume this packet is an unlimited archive.
- Older recordings only support another driver's metrics when those values were
  actually captured. Never synthesize private history from the recorded player.
- Treat unknown access and packet gaps as unavailable until evidence establishes
  availability. Stop extending graphs through restricted intervals.

Verified source sections:

| Supplied reference | Relevant sections | Physical pages |
|---|---|---|
| [F1 24 v27.2x](../.agents/skills/f1-udp-verify-spec/references/f1-24-v27.2x.md) | Car Status / `CarStatusData`; Car Damage / `CarDamageData`; Session History / `LapHistoryData`, `TyreStintHistoryData`, `PacketSessionHistoryData`; Restricted data | DOCX extraction; no inferred pagination |
| [F1 25 v3](../.agents/skills/f1-udp-verify-spec/references/f1-25-v3.md) | Same structs and Restricted data | PDF 11, 13–14, 17–18 |
| [2026 Season Pack, Season 8 v1.2](../.agents/skills/f1-udp-verify-spec/references/f1-25-2026-season-8.md) | Same structs and Restricted data; additional harvest-limit field | PDF 11–15, 19–20 |

This verifies the feature's data feasibility and relevant semantics, not complete
binary parser conformance. No new UDP fields or recording-format changes are
required by this design.

## 8. Implementation sequence and acceptance review

First establish the shared snapshot and history selection, then implement the
Electron page and integrate its subscriptions/settings. Keep Qt UI work deferred.

When implementation is authorized, review these behaviours using available
fixtures or recordings and appropriate existing checks:

- Player and public selected opponents display their own values in live and V6
  playback; restricted opponents show unavailable private metrics.
- Completed invalid and pit laps contribute to consumption averages; invalid
  laps cannot become FL. A new stint starts with zero completed laps and empty
  completed-lap averages until its first measured lap completes.
- Same-compound tyre changes and reused sets preserve correct stint boundaries;
  Wear/Lap agrees with the Tyres page for the same fitted set and cursor.
- ERS charts use MJ and recharge combines both harvest sources. Counter resets
  and refuelling never generate artificial zero or negative consumption points.
- Backward/forward seeking, flashbacks, driver switches, and page revisits do not
  leak future laps, stale stint metrics, or another driver's data.
- Tyre graph range/life controls and all themes behave consistently with existing
  pages, including a small window and partial/no telemetry.

## 9. Implementation notes

Validation performed: Electron `npm run typecheck`. Native compilation and an
application preview remain unverified. No new test cases were added.
