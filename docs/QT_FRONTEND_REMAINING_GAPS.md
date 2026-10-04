# Qt Frontend — Remaining Gaps vs Electron

## Purpose

This document lists behaviour and presentation that `electron-frontend/` has
and `qt_frontend/` does not, as found by a page-by-page source comparison. It
is a follow-up to `QT_FRONTEND_PARITY_TODO.md` (whose items are all checked off)
and to the **Qt ↔ Electron Parity Plan** decision doc of 2026-10-02.

Audit date: 2026-10-04
Compared: `electron-frontend/src` at `e758899` with `qt_frontend/src` at
`e758899` **plus the staged, uncommitted Qt changes in the working tree**.

Method: read-only source review. Nothing was built or run, so every item below
comes from reading code, not from watching either app. Line numbers are as of
the audit and will drift.

## Ground rules

- Electron is the reference. Where Qt is changed, it should match Electron's
  wording, colours and rules rather than approximate them.
- Anything the parity plan marks **Skip** is genuinely not needed. Those items
  are listed once under [Excluded by decision](#excluded-by-decision) and are
  otherwise left out of this document.
- The guardrails in `QT_FRONTEND_PARITY_TODO.md` still apply: no changes to
  `protocol_parser_library/` without explicit owner permission, and no builds,
  tests or app launches as part of implementing an item.
- Qt-only features (widget style, contrast threshold, graphics API / MSAA,
  toolbar labels, settings search, persisted chart-window overrides, persisted
  window geometry, the Data Comparison toggle) are not gaps and should be kept.

Priority:

| Priority | Meaning |
| --- | --- |
| P1 | Behaviour or displayed data differs from Electron |
| P2 | Same data, but layout or formatting visibly differs |
| P3 | Wording or small cosmetic difference |

---

## 1. Status of the parity plan

| # | Plan item | Decision | State in the working tree |
| --- | --- | --- | --- |
| 3 | Session-type badge in the titlebar | Port | **Not done.** See [GAP-001](#gap-001--session-type-badge-in-the-toolbar). |
| 4 | UDP error in the titlebar | Port | Done — `AppToolbar::setUdpError`, `MainWindow::showUdpListenerStatus`; the startup message box is gone, the engine-startup dialog stays. |
| 6 | Safety-car banner phases | Change | Done — `ToastEvents.cpp` `safetyCarBanner()`, `MainWindow::refreshSafetyCarBanner()`. |
| 7 | Slow-seek "Loading" overlay | Port | Done — `SeekLoadingOverlay` armed on `seekStarted`, disarmed in `resetSeekGate()`. |
| 10 | Analysis Split Cursors toggle | Port | Done — `splitCursorsAction_`, `analyze/splitCursors`, listed in Analysis Help. |
| 12 / 19 | Session events list 1:1 | Change | Done — `SessionPage.cpp` `formatEvent()`, density layouts, "No events yet", retirement merge, playback catalog cursor; toasts read "DRS Enabled/Disabled" and add "Resume Race". |
| 13 | Forecast accuracy on the NOW card | Port | Done — `sp_weatherNowAccuracy` at Normal and Spacious. |
| 14 | Graph Table freeze + Scroll to Bottom | Port | Done — `GraphTable` pinned state, 1.5-row threshold, re-pin on column/axis change. |
| 15 | Spacious density sub-lines | Maybe | Still open. See [GAP-033](#gap-033--spacious-density-sub-lines-plan-item-15-maybe). |
| 16 | Analysis map DRS/SLM overlay | Investigate | Done — `AnalysisPage::setAeroMode()` is fed from both `protocol_status` and playback entry. |

All of the "done" rows are staged but not committed.

One item from `QT_FRONTEND_PARITY_TODO.md` has regressed: **ANALYZE-007**
(collapsible Current/Compare/Lap A/Lap B groups) is checked off there, but the
current `analysis/AnalysisLapSlot` has no collapse. See
[GAP-027](#gap-027--collapsible-comparison-groups-regressed-analyze-007).

---

## 2. Summary

| ID | Area | Pri | Gap |
| --- | --- | --- | --- |
| GAP-001 | Toolbar | P1 | No session-type badge (plan #3) |
| GAP-002 | Playback | P1 | Toolbar Open and command-line files ask for confirmation; Electron loads directly |
| GAP-003 | Toolbar | P2 | "Delta Updates" throttles only the delta, not the session clock |
| GAP-004 | Toolbar | P2 | Page order, "Analyze" label and page icons differ |
| GAP-005 | Playback | P3 | Loading/export overlay and load-failure wording |
| GAP-006 | Playback | P3 | Playback bar control order and empty lap selector |
| GAP-007 | Notifications | P2 | Toasts stack (up to 4) instead of queueing one at a time |
| GAP-008 | Settings | P3 | Detected Protocol text lacks "No data yet" / "(manual)" / "(last session)" |
| GAP-009 | Charts | P2 | No "No data" empty state on any chart |
| GAP-010 | Input charts | P2 | Axes, tooltips and tables show raw 0–1 values; no zero line, hints or glyphs |
| GAP-011 | Misc charts | P2 | G-force ticks/reference lines, direction hints, ride-height range |
| GAP-012 | Tyre charts | P3 | Temperature midpoint ticks; table precision |
| GAP-013 | Overview | P2 | Tyre graphs are a fixed 200 px instead of a proportional split |
| GAP-014 | Tyres | P1 | RESERVED vs RETURNED decided differently |
| GAP-015 | Tyres | P1 | "Rec." column uses different session labels |
| GAP-016 | Tyres | P1 | Δ Lap shown for fitted and unavailable sets |
| GAP-017 | Tyres | P2 | Allocation table: section titles, placeholder rows, row styling, status pill |
| GAP-018 | Tyres | P2 | Wheel cards do not auto-compact on short windows |
| GAP-019 | Tyres | P3 | Graphs header caption |
| GAP-020 | Standings | P2 | Tower driver cell lacks team stripe and 3-letter code; no Spacious tyre age |
| GAP-021 | Standings | P2 | Sidebar cards use one key/value layout for every density |
| GAP-022 | Standings | P3 | Sidebar text details (sector format, "vs finish", "Age:", "Mix:") |
| GAP-023 | Session | P1 | Right panel width does not follow the window or the % setting |
| GAP-024 | Session | P2 | Header "Time Left" caption, empty clock text, Spacious zones caption |
| GAP-025 | Session | P2 | Proximity placeholders and Spacious extras |
| GAP-026 | Session | P3 | Weather skeleton, rain % colour, compact captions, red marshal flag |
| GAP-027 | Analysis | P1 | Comparison groups cannot collapse (regressed ANALYZE-007) |
| GAP-028 | Analysis | P1 | Chart double-click buttons are swapped |
| GAP-029 | Analysis | P2 | Secondary-file dialog ignores the shared last directory |
| GAP-030 | Analysis | P2 | Live "Current" lap time does not tick |
| GAP-031 | Analysis | P3 | Colour-picker visibility, split-handle keys, Delta "not supported" row |
| GAP-032 | Track map | P3 | Empty-state text; follow selector not searchable/clearable |
| GAP-033 | Density | Maybe | Spacious sub-lines (plan #15) |

---

## 3. Details

### A. Toolbar, playback and notifications

#### GAP-001 — Session-type badge in the toolbar

P1 · Parity plan item 3 (Port).

Electron:
- `renderer/src/app/components/AppHeader.tsx:134-138` shows
  `SESSION_TYPES[session_type]` in uppercase, coloured by `sessionAccent()`
  (`components/SessionPanel.tsx:41-46`: practice orange, qualifying/shootout
  gold, race blue, other grey) on a 13% tint of the same colour (`accent + '22'`).
- Before a session row arrives it shows a grey **Offline** badge.

Qt today:
- `AppToolbar.cpp:124-157` (the page-dropdown group that never overflows) has
  the UDP error button but no badge.
- The session type only reaches the OS window title
  (`MainWindow.cpp:2068-2077`, `updateWindowTitle()`).

To match: add the badge to the page-control group as the plan describes; feed
it from `MainWindow::emitLiveData` on session rows and reset to Offline where
`titleSessionType_` is reset today (SEND, playback enter/exit).

#### GAP-002 — Opening a recording asks for confirmation when Electron does not

P1.

Electron:
- Toolbar Open loads straight after the file dialog — no confirmation
  (`app/hooks/usePlayback.ts:169-175`).
- A file passed on the command line at startup also loads directly
  (`main/application.ts:585-592`).
- Only a second-instance launch or a macOS open-file event while the window
  exists shows the **Open Session File** confirmation
  (`application.ts:147-177` → `PlaybackDialogs.tsx`).

Qt today:
- Toolbar Open (`MainWindow.cpp:540-546`) and the startup argument
  (`main.cpp:291-299`) both go through `offerRecordingFile()`, which always
  shows the confirmation (`MainWindow.cpp:819-840`).

To match: load directly for toolbar Open and the startup argument; keep the
confirmation for single-instance and `QFileOpenEvent` deliveries.

#### GAP-003 — "Delta Updates" should also throttle the session clock

P2.

Electron: with an interval other than Realtime, `ThrottledSessionTimer` reads
both `session_time` and the delta from one snapshot on the interval
(`app/components/SessionTimer.tsx:33-43, 55-69`). The setting is described as
"How often the session timer and lap-comparison delta refresh"
(`components/Settings.tsx:551`).

Qt today: the interval gates only the delta (`MainWindow.cpp:289-298`); the
clock still updates on every packet (`AppToolbar.cpp:372-386`). The Qt hint
says "How often the toolbar's lap-comparison delta updates"
(`SettingsDialog.cpp:486-497`).

Note: `QT_FRONTEND_PARITY_TODO.md` RUNTIME-003 deliberately left the clock
unthrottled. This is flagged so the owner can decide; Electron throttles both.

#### GAP-004 — Page dropdown order, label and icons

P2.

Electron (`app/appConfig.ts:173-178`, `app/components/TabOptionLabel.tsx`):
Overview, **Analysis**, Session, Strategy, Standings, Input, Power, Tyres, Misc —
each entry with an icon.

Qt (`MainWindow.cpp:238-240`, `MainWindow.h:83`): Overview, **Analyze**,
Standings, Session, Tyres, Strategy, Input, Power, Misc — text only.

To match: reorder the `Page` enum, the toolbar list and the stack together (the
enum comment already explains they move as one), rename to "Analysis", and add
icons to the combo items.

#### GAP-005 — Overlay and load-failure wording

P3.

| Situation | Electron | Qt |
| --- | --- | --- |
| Loading a recording | "ANALYZING SESSION DATA" + scanning bar (`StatusOverlays.tsx:14-19`) | "Loading recording…" (`MainWindow.cpp:477`) |
| Exporting | "EXPORTING TO EXCEL" heading, bar, `NN%`, then the stage on its own line (`StatusOverlays.tsx:21-27`) | the stage text replaces the heading; the percentage is the progress bar's own text (`MainWindow.cpp:487-506, 919-922`) |
| Load failure | title **Recording Load Failed** (`PlaybackDialogs.tsx:98`) | **Load Failed** (`MainWindow.cpp:557`) |
| Export failure | export button turns to an error state for 4 s with the error as its tooltip (`usePlayback.ts:149-162`, `PlaybackBar.tsx:213`) | modal "Export Failed" (`MainWindow.cpp:936-937`) |

#### GAP-006 — Playback bar control order and empty lap selector

P3.

Electron (`app/components/PlaybackBar.tsx:198-207`): Speed select, then Lap
select; the Lap select is not rendered until the recording has laps.
Qt (`PlaybackController.cpp:173-186`): Lap before Speed, and the Lap combo is
always shown with "—".

#### GAP-007 — Event notifications are shown one at a time

P2.

Electron: transient banners are queued and shown one after another, each for
the configured duration (`app/hooks/useRaceBanners.ts:30-44`); the setting
reads "How long each race event notification is shown before the next one"
(`Settings.tsx:867`). The safety-car banner cannot be dismissed and gives way
to a transient banner while one is showing (`useRaceBanners.ts:77`).

Qt: up to four toasts are on screen at once (`ToastHost.cpp:13-18`), and the
persistent safety-car toast has a close button (`ToastHost.cpp:53`).

The toast presentation itself was accepted by the plan; this item is only
about concurrency and dismissal.

### B. Settings

#### GAP-008 — Detected Protocol text

P3.

Electron (`app/AppShell.tsx:454-467`): "No data yet", the detected format,
`<format> (manual)` when an override is active and nothing is detected, or
`<format> (last session)`.
Qt (`SettingsDialog.cpp:1255-1257`): the detected format number or "—".

Everything else in Settings was checked and matches (see
[Verified at parity](#4-verified-at-parity)).

### C. Charts

#### GAP-009 — "No data" empty state

P2.

Electron shows a centred "No data" message when a chart has no samples:
`GearChart.tsx:84`, `InputsChart.tsx:173`, `SteeringChart.tsx:85`,
`GForceChart.tsx:129`, `RideHeightChart.tsx:142`, `PowerBreakdownChart.tsx:153`,
`TyreTrendCharts.tsx:263`, and "No data — start driving to see telemetry" on the
Overview chart (`SpeedRpmChart.tsx:123`).

Qt: no chart has an empty-state message; panels draw empty axes. `ChartView`
already paints per-panel titles and notes, so a per-panel empty message fits
there.

#### GAP-010 — Input charts formatting

P2. Qt reference: `InputChartsWidget.cpp:49-66` (axes/series), `:171-193`
(tables).

| Element | Electron | Qt |
| --- | --- | --- |
| Pedal Y axis | `100% / 50% / 0%`; Combined shows the brake half as positive percent too (`InputsChart.tsx:27-28, 76, 177`) | `0.00…1.00` / `-1.00…1.00` |
| Pedal tooltip / table | `45%` (`InputsChart.tsx:76, 106, 127`) | `0.45` (Fixed2) |
| Steering Y axis | `L100 L50 0 R50 R100` (`SteeringChart.tsx:25-28`) | `-1.00…1.00` |
| Steering tooltip | `45% L`, `45% R`, `0%` (`SteeringChart.tsx:21-24`) | `0.45` |
| Steering table | `45%` (`SteeringChart.tsx:17`) | `0.45` |
| Zero reference line | solid y=0 on Combined pedals and Steering (`InputsChart.tsx:178`, `SteeringChart.tsx:89`) | none (only Gear 2/4/6 dashed) |
| Legend | `▲ Accelerator` / `▼ Brake` on Combined, `—` on Combined 2 (`InputsChart.tsx:156-169`); Steering `— Input` plus note "L = left / R = right" (`SteeringChart.tsx:79-81`) | generic legend; title "STEERING TELEMETRY ( - Left / + Right )" |
| Titles | Gear, Accelerator / Brake, Accelerator, Brake, Steering | "GEAR INDICATOR", "STEERING TELEMETRY …" |

`ChartView::setAxisLabelMap`, `setAxisNumberSuffix`, `addReferenceLine(..., false)`
and `setPanelNote` already exist, so this is configuration rather than new
rendering.

#### GAP-011 — Misc charts

P2. Qt reference: `MiscChartsWidget.cpp:34-63`.

- **G-force ticks and lines:** Electron uses fixed ticks −6…6 in steps of 2
  labelled `Ng`, a solid line at 0 and dashed lines at ±4
  (`GForceChart.tsx:26, 142-147`). Qt uses automatic ticks with a ` G` suffix
  and no reference lines.
- **Direction hints:** Electron shows "+ve = right / accel" (combined),
  "+ve = right" (lateral), "+ve = accel" (longitudinal) and
  "plank edge above road" (ride height) next to the legend
  (`GForceChart.tsx:33-37, 124`, `RideHeightChart.tsx:137`). Qt has none;
  `setPanelNote` is used this way on the Power page already.
- **Ride-height range:** Electron starts at 0–50 mm and only expands outward
  with 2 mm / 5 mm padding, with five rounded ticks
  (`RideHeightChart.tsx:30-33, 53-58, 155-163`). Qt uses a fixed 0–100 mm and
  never fits the axis.

#### GAP-012 — Tyre trend chart details

P3.

- Electron adds a tick halfway through each of the first two intervals on the
  temperature axes (`TyreTrendCharts.tsx:99-106`).
- Electron's tyre tables show one decimal with the unit, e.g. `95.3°C`
  (`TyreTrendCharts.tsx:206-209`); Qt shows integers (`TyreChartsWidget.cpp:157-169`).

### D. Overview

#### GAP-013 — Tyre graph height

P2.

Electron gives the Speed/RPM/ERS chart and the tyre graphs a proportional split
of the remaining height: 13 : 7, or 8 : 4 when the damage cards wrap to two rows
(`app/components/TabContent.tsx:170-176`).
Qt fixes the tyre graphs at 200 px (`OverviewPage.cpp:410-415`).

### E. Tyres page

#### GAP-014 — RESERVED vs RETURNED

P1.

Electron (`components/TyresPanel.tsx:89-122`): an unavailable set is
**RESERVED** only when its `recommended_session` is later than the current
session's slot (FP1=1 … Q3=6, Race=7, with Short/One-Shot/Sprint Shootout
sessions mapped onto those slots); without a session it falls back to
`recommended_session >= 4`. Otherwise it is **RETURNED**.

Qt (`TyreHelpers.h:78-83`): any unavailable set with `recommended_session > 0`
is RESERVED, regardless of the current session.

To match: pass the session type into `setStatusText()`/`setStatusColor()` and
port `getSessionOrder()`.

#### GAP-015 — "Rec." column labels

P1.

Electron maps `recommended_session` 0–7 to `—, FP1, FP2, FP3, Q1, Q2, Q3, Race`
(`TyresPanel.tsx:71-73, 188`). Qt indexes the 19-entry session-type list, so
for example 4 reads "Short P" instead of "Q1", and 7 reads "Q3" instead of
"Race" (`TyresPage.cpp:323-328, 400-401`).

The two clients disagree about what the field means; since Electron is the
reference, Qt should use Electron's table. Column headers also differ:
Electron `Wear · Wear/Lap · Life · Rec. · Δ Lap` (`TyresPanel.tsx:200-208`) vs Qt
`WEAR · WEAR/LAP · LIFE · SESSION · DELTA` (`TyresPage.cpp:57`).

#### GAP-016 — Δ Lap visibility

P1.

Electron shows the delta only for sets that are available and not fitted
(`TyresPanel.tsx:150-153`). Qt shows it for every set with a non-zero delta
(`TyresPage.cpp:403-409`).

#### GAP-017 — Allocation table presentation

P2. Electron `TyresPanel.tsx:155-245, 374-381`; Qt `TyresPage.cpp:54-85, 262-269, 344-398`.

- Section titles **Dry Sets (Slicks)** and **Wet / Inter Sets** above each table.
  Qt has two untitled tables.
- Before any `tyre_sets` row, Electron shows 13 dry and 7 wet placeholder rows
  (`#1 — —`). Qt shows empty tables.
- The fitted row has a blue tint and returned rows are drawn at 40% opacity.
  Qt has neither.
- Status is a bordered pill with a 10% tint of its colour; Qt uses coloured text.
- Life always reads `x/yL`; Qt shows "—" when both values are 0.

#### GAP-018 — Wheel cards auto-compact

P2.

Electron switches the right-hand wheel cards to the compact column layout when
the column is shorter than 720 px (`TyresPanel.tsx:272`). Qt always uses the
full layout (`TyresPage.cpp:116-135`).

#### GAP-019 — Graphs header caption

P3. Electron "Tyre Conditions" (`TyresPanel.tsx:328`); Qt "TYRE GRAPHS"
(`TyresPage.cpp:156`).

### F. Standings

#### GAP-020 — Timing tower driver cell and Spacious tyre age

P2.

Electron (`components/TimingTower.tsx:121-123, 157-172`): a team-colour stripe,
the race number, a bold 3-letter code (last name, uppercase), the full name in
secondary text (hidden on narrow windows), and the YOU chip. In Spacious the
tyre cell adds the age, e.g. `S (5L)` (`TimingTower.tsx:204`).

Qt (`StandingsPage.cpp:264-267, 1074-1100`): a separate `#` column and the full
name drawn in the livery colour (contrast-guarded); no stripe, no code, no
tyre age.

#### GAP-021 — Sidebar card layouts

P2.

Electron renders each sidebar card differently per density
(`components/RacePanel.tsx`):
- Timing (`:646-1106`): large `P3` with "Lap N", status badges
  (Pitting / In pit lane / INVALID / +Ns), large Current, Last lap, then an
  S1/S2/S3 grid; the selected driver's name and team stripe sit in the card
  header.
- Energy Recovery (`:63-324`): large ERS % with the mode name beside it in the
  mode colour, a bar, `x.xx MJ / 4.00 MJ`, then a Deployed/DRS grid; Compact is
  a 3-column Store/Deployed/DRS grid with "AVAIL"; Spacious adds Harvested and
  ERS Mode.
- Strategy (`:326-546`): large coloured fuel figure, laps, "Mix: …", then a
  large tyre compound with age and brake bias.

Qt (`StandingsPage.cpp:435-606`) uses one list of label/value rows for every
density, with the driver name centred above the Timing section.

#### GAP-022 — Sidebar text details

P3.

| Element | Electron | Qt |
| --- | --- | --- |
| Sector times (Normal/Spacious) | `0:28.123`, placeholder `–:––.–––` (`RacePanel.tsx:862, 1010`) | `28.123`, placeholder "—" (`StandingsPage.cpp:88-93`) |
| Current / Last lap placeholder | `--:--.---` | "—" |
| Fuel laps, Normal | `+1.2 laps vs finish` (`RacePanel.tsx:497`) | `+1.2 laps` ("vs finish" only in Spacious, `StandingsPage.cpp:824-827`) |
| Tyre age | `Age: 5 laps` (Normal/Spacious), `5L` (Compact) | `5L` |
| Mix | `Mix: Standard` | row "Mix" → `Standard` |

### G. Session

#### GAP-023 — Right panel width

P1.

Electron sizes the map/weather column and the proximity/events column as
percentages of the page width using `sidebarPct` (default 28%, 15–60%)
(`components/SessionPanel.tsx:507-510, 708-711`).

Qt computes a pixel width in `applyLayout()` from the page's `width()` at that
moment and clamps it to 180–600 px (`SessionPage.cpp:1451-1460`). The first call
happens in the constructor, before the page has its real size, and
`SessionPage` has no `resizeEvent`, so the panel keeps that width when the
window is resized; it only changes when the layout is edited.

To match: recompute the width in a `resizeEvent`, as `StandingsPage` already
does (`StandingsPage.cpp:341-366`), and drop the pixel clamp.

#### GAP-024 — Header details

P2.

- Electron shows a **Time Left** caption above the clock at Normal and Spacious
  header levels (`SessionPanel.tsx:469-471`); Qt has no caption
  (`SessionPage.cpp:576-587`).
- With no session the clock reads `--:--` (Electron) vs `—:——` (Qt).
- At the Spacious header level the zones caption reads **Marshal Zones**
  (`SessionPanel.tsx:437-441`); Qt always reads "ZONES".

#### GAP-025 — Proximity placeholders and Spacious extras

P2.

- Electron shows "No timing data" before timing arrives and "No position data"
  when the player is not classified (`SessionPanel.tsx:221, 730-733`). Qt hides
  the rows (`SessionPage.cpp:1238-1263`).
- At Spacious, each proximity row adds a team-colour stripe and `#N` race number
  (`SessionPanel.tsx:262-272`). Qt does not.

#### GAP-026 — Weather and card details

P3.

- With no session, Electron draws five placeholder forecast cards with "—" and
  a grey icon (`SessionPanel.tsx:592-639`); Qt leaves the labels blank.
- At the Normal weather level Electron draws the forecast rain % in secondary
  text (`SessionPanel.tsx:696`); it is coloured only at the other levels. Qt
  colours it at every level (`SessionPage.cpp:1077-1079`).
- Compact stat-card captions stay "Track Length" and "Time of Day" in Electron;
  Qt shortens them to "LENGTH" and "TIME" (`SessionPage.cpp:697-698`).
- Electron's marshal strip has no colour for flag 4 (red), so it draws grey
  (`SessionPanel.tsx:35-39`); Qt draws red (`SessionPage.cpp:98-105`).

### H. Analysis

#### GAP-027 — Collapsible comparison groups (regressed ANALYZE-007)

P1.

Electron (`components/AnalyzeScreen.tsx:523-596`): Current, Compare, Lap A and
Lap B are separate sections, each with a +/− button. Collapsed, the header
shows `driver · compound (in compound colour) · Lap N · lap time`; a running
lap's time keeps updating. Collapse state is per group.

Qt (`components/analysis/AnalysisLapSlot.cpp:47-101`): plain `QGroupBox`es with
no collapse. `QT_FRONTEND_PARITY_TODO.md` marks ANALYZE-007 as implemented, so
this appears to have been lost when Analysis moved into `components/analysis/`.

#### GAP-028 — Chart double-click buttons are swapped

P1.

Electron (`charts/AnalyzeTimeChart.tsx:694-707, 730-732`; help text
`AnalyzeScreen.tsx:2089-2092`):
- double **left** click → show that point on the map (switching Graphs to Split);
- double **right** click → reset zoom.

Qt:
- double **right** click → show on map (`ChartView.cpp:2290`);
- double **left** click → reset zoom, and only while navigation is enabled
  (`ChartView.cpp:2331-2334`);
- Analysis Help documents the Qt mapping (`AnalysisPage.cpp:1414-1419`).

To match: swap the two in `ChartView::eventFilter` and update the help rows.

#### GAP-029 — Secondary recording dialog directory

P2.

Electron opens the secondary file with the shared TNRD picker, which starts in
`dialogs.lastDirectory` and updates it (`AnalyzeScreen.tsx:1438-1440`,
`main/application.ts:277-290`).
Qt starts in the recording output directory and does not remember the choice
(`AnalysisPage.cpp:1242-1246`). `MainWindow::lastDialogDirectory()` /
`rememberDialogDirectory()` already exist for this.

#### GAP-030 — Live "Current" lap time does not tick

P2.

Electron writes the running lap time into the Current lap field on every
telemetry update (`AnalyzeScreen.tsx:398-431, 1183-1199`).

Qt builds the running time from `currentTime_` (`AnalysisPage.cpp:808-821`),
which `MainWindow` only updates during playback (`MainWindow.cpp:688-701`). In a
live session the field shows "—" and is refreshed only when `applyState()` runs.

#### GAP-031 — Smaller Analysis differences

P3.

- **Colour pickers:** Electron shows the lap colour swatch in each group only
  while a map is visible (Split or Map) (`AnalyzeScreen.tsx:558-560`); Qt always
  shows it (`AnalysisLapSlot.cpp:83-97`).
- **Split handle keyboard:** Electron's divider takes focus and moves 1% per
  arrow key (5% with Shift) (`AnalysisSplitHandle.tsx:73-81`); `AnalysisSplitter`
  has no keyboard handling.
- **Delta when unsupported:** Electron's Delta row reads "Not supported in this
  file." and hides its move/axis/visibility/reset controls
  (`AnalyzeScreen.tsx:1854-1869`); Qt's subtitle reads "Needs lap distance data"
  (`AnalysisSeriesModel.cpp:311-313`) and the context menu stays available.

### I. Track map

#### GAP-032 — Empty state and follow selector

P3.

- Electron shows "No map data" when there is no map (`TrackMap.tsx:1247-1253`);
  Qt shows "Waiting for session…" or "No map for this track"
  (`TrackMapWidget.cpp:843-844`).
- Electron's follow-driver selector is searchable and clearable
  (`TrackMap.tsx:802-818`); Qt's combo uses a "Follow driver…" item to clear and
  is not searchable (`TrackMapWidget.cpp:233-249`).

### J. Undecided

#### GAP-033 — Spacious density sub-lines (plan item 15, Maybe)

Still not implemented. If picked up, the strings are:

- Overview stat cards (`components/LiveStats.tsx:178-199`):
  ERS `"<mode> · X.XX MJ"` (or "FAULT"); Fuel `"+X.X laps vs finish"`;
  Tyre `"NL age · <Lean|Std|Rich|Max> mix"`; Position `"Lap N"`;
  DRS `"Active (Open)"` / `"Available"` / `"Closed"`; SLM `"Active (Open)"` / `"Closed"`.
  Qt `OverviewPage::refreshCards()` (`OverviewPage.cpp:733-829`) only applies the
  Compact shortenings.
- Power cards (`components/PowerStatsBar.tsx:92-100`): "Combined ICE + MGU-K",
  "Combustion Engine", "Kinetic Motor Generator", "ICE % : ERS % Ratio",
  `"NN% of 4.00 MJ"`, `"X.XX MJ Stored"`, `"+X.X laps vs finish"`.
  Qt `PowerPage.cpp` has no Spacious sub-lines.
- Playback bar (`app/components/PlaybackBar.tsx:199, 204, 209-218`): "SPEED" and
  "LAP" captions and an "Export (.xlsx)" text button at Spacious. Qt shows the
  `(NN%)` progress already (`PlaybackController.cpp:321-324`) but not these.

---

## 4. Verified at parity

These were compared in this pass and need no action:

- **Settings:** Appearance (tyre view, wear mode, Reduce Animations, FPS in/out
  of focus), Team Colours (2024/2025/2026, livery/fixed, resets), Layout (Input
  grid/vertical and pedal modes, Misc combined/split, Power and Tyres
  arrangement, secondary crosshairs), Graphs (chart/table per section and Set
  All), Y Axis Behavior, Density (all sections, specialised levels, three Set
  All actions), Notifications (duration choices, update checks), Map (driver
  markers, hide-static choices, sector colours; Qt's opacity slider covers the
  dim toggle), Network (port, bind address, forwarding channels, validation,
  Apply & Restart states), Paired Devices, Protocol (override, mismatch
  warning), Data Storage, Debug (Additional logging, Memory log), About (links,
  creator, non-affiliation, diagnostics folder).
- **Layout editors:** Overview, Input, Misc, Power, Tyres, Session, Standings.
- **Chart behaviour:** global and per-chart windows, lap modes, Selected-lap
  picker, sector boundaries, synced tooltips and secondary crosshairs, clickable
  legends, fixed/dynamic Y axes, comparison-lap tooltip section and Delta row,
  graph tables including freeze/Scroll to Bottom.
- **Overview:** stat cards and colours, damage cards (including Spacious
  Clean/Minor/Critical), tyre cards at all seven levels with blisters.
- **Power:** chart titles, notes, Total/MJ tooltip rows, 4/8 MJ harvest scale,
  MGU-H visibility, fuel upper limit.
- **Strategy:** non-race message, waiting/rebuilding states, plan columns,
  sidebar sections (SC/VSC decision, race call, next pit window, weather window,
  race battle, tyre condition, tyre alerts), Required pit stops.
- **Session:** events list and colours, forecast accuracy, marshal strip,
  proximity logic, density levels.
- **Track map:** DRS/SLM zones, overtake markers, speed traps, follow + zoom,
  label modes, idle timeout, sector colours.
- **Standings:** columns, badges, retirement labels, fastest-lap tint, sector
  freeze, lap-times dialog.
- **Analysis:** Graphs/Split/Map, Individual Graphs, Synced Tooltip, Sector
  Boundaries/Delta, Split Cursors, fixed comparison, secondary recording and
  circuit-mismatch prompt, metric catalog and picker (filter, tyre corners,
  combine), Delta colours, delta readout, map data-comparison card with ERS,
  map replay transport, split ratio with badge and double-click reset, help.
- **Desktop/runtime:** UDP error, recording-error dialog, bridge-failure dialog,
  update dialog, single instance, startup temp-file sweep, launch diagnostics,
  V6 playback driver selector, XLSX export, safety-car and leader toasts, seek
  overlay.

## Excluded by decision

Marked **Skip** in the parity plan and intentionally left out above:

- App fullscreen with auto-hiding header
- Background mode / tray icon
- Protocol-mismatch banner in the main window (the Settings warning is enough)
- Midnight theme
- Taskbar/tray icon following the Windows theme
- Standings reorder slide and gain/loss flash
- Page and view transition animations
- Loaded filename in the header
- Native Titlebar, React Scan, WebGL monitor, Show Node-API exceptions, debug
  map picker

Also not treated as gaps: Electron's DevTools shortcut and the unused
`ControllerVisualizer.tsx` (not mounted anywhere).
