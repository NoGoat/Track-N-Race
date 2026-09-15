# Qt Frontend Parity TODO

## Purpose

This document lists user-visible behavior that is implemented and reachable in
`electron-frontend/` but is missing or materially incomplete in
`qt_frontend/`.

It is written as an implementation backlog for a later AI model or engineer.
It is not permission to redesign either frontend.

Audit date: 2026-09-02
Audited revision: `c681a2b`

## Inclusion rule

An item belongs in this file only when all of the following are true:

1. Reachable Electron code implements the behavior.
2. Qt does not currently provide equivalent behavior.
3. The task describes the Electron behavior, not a proposed new product feature.

Every task below names the Electron source that proves the feature exists.
If that source is removed or becomes unreachable, re-check the task before
implementing it.

## Protocol parser library freeze — explicit owner constraint

Every task in this document must be completed without changing
`protocol_parser_library/`. The Electron frontend already implements every cited
feature without requiring a parity-specific change to that library, so the Qt
frontend must do the same by using the library's existing public APIs and handling
frontend-specific adaptation inside `qt_frontend/`.

- Do not edit, add, delete, rename, reformat, or add tests under
  `protocol_parser_library/` without first asking the repository owner and
  receiving their explicit permission for that specific library change.
- A missing `AnyRow` alternative, convenience parser, callback, helper, or test
  does not grant permission to change the library. Handle the data at the Qt host
  boundary in the same way Electron handles frontend-specific bridge data.
- Do not modify the TNRD format, protocol parsers, shared row variants, or shared
  engine behavior as part of any task in this backlog.
- If a task appears impossible using the existing library surface, stop and ask
  the repository owner. Do not silently broaden the task into a library change.
- Permission to implement a TODO item is not permission to modify
  `protocol_parser_library/`.

## Build and test freeze — repository owner responsibility

Building, running, and testing the Qt frontend are the repository owner's job.
Permission to implement any task in this document does not include permission to
execute or verify the result.

- Do not run a Qt, Electron, shared-library, installer, or repository-wide build.
- Do not run automated tests, test binaries, linters, type-checkers, benchmarks,
  sanitizers, profilers, formatters, or coverage tools.
- Do not launch the application, a development server, a debugger, an installer,
  or a packaged executable.
- Do not install dependencies or invoke CMake, MSBuild, Ninja, npm build scripts,
  CTest, or equivalent verification commands.
- Read-only source inspection and diff review are allowed. Execution-based
  verification is not.
- After editing, report exactly what changed and give the repository owner any
  manual build/test steps they may choose to run themselves.
- If execution appears necessary, stop and ask the repository owner for separate,
  explicit permission. Do not infer build/test permission from an implementation
  request.

## Other scope guardrails

- This is a Qt frontend backlog.
- Where Electron handles a control message in its main-process bridge, implement
  the Qt equivalent at the Qt host boundary using existing data/API surfaces.
- Do not add behavior merely because it seems safer, cleaner, or more complete.
  Match the reachable Electron behavior described here.
- Do not copy React or Electron window-chrome implementation details. Native Qt
  controls are acceptable when the behavior is equivalent.
- Preserve existing Qt-only features such as widget-style selection, contrast
  controls, and native window behavior.
- Do not add dependencies unless the task genuinely requires one and the
  repository dependency/attribution process is followed.

## Existing Qt behavior that is not a TODO

Do not rebuild these features:

- Live UDP ingest and automatic TNRD recording.
- F1 24, F1 25, and F1 26 automatic/forced protocol selection.
- The existing 15 s, 30 s, 1 m, 2 m, 5 m, and 10 m chart windows.
- Chart/table selection for the graph sections already represented by
  `GraphViewSettings.h`.
- Per-section compact modes already represented by `CompactSettings.h`.
- Basic per-chart hover readouts and crosshairs.
- Overview, Input, Misc, Power, and Session layout editors.
- Analysis metric add/remove/reorder, per-metric colors, visibility, fixed
  playback Lap A/Lap B comparison, Y-axis visibility, zoom, and pan.
- TNRD open/close, play/pause, seek, ±5 seconds, speed selection, lap selection,
  and XLSX export.
- Track-map labels, sector colors, opacity, idle timeout, follow-driver behavior,
  and the map-only enlarged view.
- Toast notifications and their duration setting.

## Priority summary

| Priority | Items |
| --- | --- |
| P0 | SAFE-001, SAFE-002, NET-001, NET-002 |
| P1 | CHART-001 through CHART-006, ANALYZE-001 through ANALYZE-005 |
| P2 | LAYOUT-001 through LAYOUT-005, DENSITY-001, STRATEGY-001, NOTIFY-001 |
| P3 | RUNTIME-001 through RUNTIME-003, PLAY-001, PLAY-002, DESKTOP-001, DESKTOP-003, DESKTOP-004 |

---

## A. Recording, protocol, and network behavior

### SAFE-001 — Recording failure dialog

Electron evidence:

- `electron-frontend/src/main/bridgeManager.ts`:
  `handleRecordingError()` detects `recording_error` rows before normal
  telemetry forwarding and sends the `recording-error` IPC event.
- `electron-frontend/src/preload/index.ts`: `recordingBridge.onError()`.
- `electron-frontend/src/renderer/src/app/AppShell.tsx`: stores the latest
  `RecordingErrorMsg` and renders `RecordingErrorDialog`.
- `electron-frontend/src/renderer/src/app/components/RecordingErrorDialog.tsx`:
  the visible dialog.

Electron behavior to match:

- A recording write failure opens a modal titled **Session Recording Failed**.
- The dialog says that Track N Race could not write the recording.
- It displays the operation and error message.
- It displays the destination path when one exists.
- The only action is **OK**/close.
- A newer error replaces the single current error state; Electron does not
  implement a queue, deduplication system, or output-directory recovery action.

Qt gap:

- The Qt typed-row path drops `recording_error` because it is not part of
  `tnrp::AnyRow`.
- Qt has no equivalent failure dialog.

TODO:

- [x] Detect the raw `recording_error` control message at the Qt host boundary,
      before `tnrp::parseRow()` discards it.
- [x] Transfer the operation, message, and optional path onto the GUI thread.
- [x] Show a native Qt modal with the Electron fields and a single acknowledgement
      action.
- [x] Do not add `RecordingErrorRow` to `AnyRow` and do not modify the shared
      parser library for this task.

Done when:

- A forced recording open/write/finalize error produces the dialog with the same
  information Electron exposes.
- Normal telemetry routing remains unchanged.

### SAFE-002 — Protocol mismatch warning in Settings

Electron evidence:

- `electron-frontend/src/renderer/src/stores/telemetryStore.ts` stores
  `protocol_warning`.
- `electron-frontend/src/renderer/src/components/Settings.tsx`,
  `renderProtocol()`, conditionally displays **Protocol mismatch detected** and
  the detected/forced formats.

Electron behavior to match:

- The warning is shown in the Settings **Protocol** page.
- It appears only when both detected and forced warning formats exist.
- It reads: receiving one format while the override is set to another.
- A clear `protocol_warning` removes it.
- Electron does not display this as a permanent application-wide banner.

Qt gap:

- Qt parses `ProtocolWarningRow`, but does not expose its state in the Protocol
  settings UI.

TODO:

- [x] Retain the latest `ProtocolWarningRow` in the Qt frontend.
- [x] Add the conditional mismatch warning to the existing Protocol settings
      section.
- [x] Remove the warning on the explicit clear row.
- [x] Do not add a global banner as part of this parity item.

### NET-001 — UDP Forward Mode

Electron evidence:

- `electron-frontend/src/renderer/src/components/Settings.tsx`,
  `renderNetwork()`.
- `electron-frontend/src/renderer/src/app/hooks/useAppConfiguration.ts` stores
  the forwarding setting and targets.
- `electron-frontend/src/main/bridgeManager.ts` sanitizes and passes forwarding
  configuration to the engine.

Electron behavior to match:

- A **UDP Forward Mode** toggle.
- Zero to fifteen forwarding channels.
- Each channel has an IPv4 destination and port.
- Add/remove channel actions.
- IPv4 and 1–65535 port validation.
- A destination that loops back to the active listener is rejected.
- Forwarding settings are applied with the same **Apply & Restart** operation as
  the receive port and bind address.

Qt gap:

- Qt exposes receive port and bind address only.

TODO:

- [x] Add the toggle and channel editor to Qt Network settings.
- [x] Match the Electron validation and 15-channel limit.
- [x] Persist the enabled state and targets in Qt settings.
- [x] Pass valid targets through the existing public engine configuration API.

### NET-002 — Staged network changes with Apply & Restart

Electron evidence:

- `electron-frontend/src/renderer/src/components/Settings.tsx` keeps draft
  port/address/forwarding values, reports **Unsaved changes**, validates them, and
  applies them through `applyUdp()`.
- The action reports **Restarting…**, **Applied**, listener success, or the
  returned error.
- `electron-frontend/src/main/application.ts` handles `udp-restart` and replies
  with success/error.

Qt gap:

- `SettingsDialog.cpp` applies port/address edits immediately.
- Qt has no shared Apply action or inline restart result state.

TODO:

- [x] Keep Network form edits local until **Apply & Restart**.
- [x] Disable apply while the form is invalid or a restart is active.
- [x] Display the same dirty/applying/success/error states.
- [x] On apply, normalize forwarding targets, persist the draft values, and then
      request the restart, matching Electron's current order.
- [x] On failure, retain the edited values and show the returned error; do not add
      rollback behavior under this parity task.

---

## B. Ordinary chart controls

### CHART-001 — Lap-based chart windows

Electron evidence:

- `electron-frontend/src/renderer/src/app/appConfig.ts`:
  `ChartMode`, `getChartWindowOptionGroups()`.
- `electron-frontend/src/renderer/src/lib/chartCoordinates.tsx` implements the
  coordinate modes.
- `electron-frontend/src/renderer/src/app/components/AppHeader.tsx` exposes the
  global selector.

Electron behavior to match:

- Existing time windows plus:
  - **Current** (`CL`)
  - **Previous** (`PL`)
  - **Fastest** (`FL`)
  - **Selected** (`RL`, playback only)
  - **Stint Laps** (`SL`)
  - **All Laps** (`AL`)
- Current/Previous/Fastest/Selected use lap distance on the X axis.
- Previous/Fastest/Selected overlay the comparison lap.
- Stint Laps and All Laps use lap-number boundary ticks.
- The Selected mode exposes a playback lap selector.

Qt gap:

- Qt's global chart selector contains only the six time windows.

TODO:

- [x] Add the six Electron lap modes to the global Qt chart-window selector.
- [x] Reuse Qt's existing lap/session data; do not add another parser or recording
      reader.
- [x] Match mode availability exactly: Current/Previous/Fastest are offered as
      soon as lap coordinates are supported, without waiting for completed laps;
      Selected additionally requires playback. A missing comparison lap leaves
      the comparison trace empty and must not disable the mode or force 30 s.
- [x] Match the Electron X-axis meaning and comparison overlay.

### CHART-002 — Per-chart window overrides

Electron evidence:

- `electron-frontend/src/renderer/src/lib/chartWindowOverrides.tsx`.
- `ChartWindowOverrideSelect` appears in ordinary chart headers.
- Overrides and per-chart selected reference laps are owned/persisted by
  `AppShell.tsx` and `useAppConfiguration.ts`.

Electron behavior to match:

- Every supported graph section inherits the global window by default.
- A chart header can override that chart with any currently available time/lap
  window.
- A per-chart Selected mode can choose its own reference lap.
- Clearing the override returns the chart to the global value.

Qt gap:

- All ordinary Qt charts use the one global time window.

TODO:

- [x] Add an inherit/override selector to each supported Qt graph header.
- [x] Persist overrides by the stable graph-section key.
- [x] Add the per-chart reference-lap choice for Selected mode.
- [x] Preserve the global selector as the default for charts without overrides.

### CHART-003 — Sector-boundary X axes

Electron evidence:

- `electron-frontend/src/renderer/src/app/components/AppHeader.tsx` exposes
  **Sector Boundaries** when lap-coordinate modes are available.
- `electron-frontend/src/renderer/src/lib/chartCoordinates.tsx` resolves sector
  split distances and generates S1/S2/S3 ticks.

Electron behavior to match:

- In lap-distance chart modes, the toggle replaces ordinary distance ticks with
  sector boundary ticks labelled S1, S2, and S3.
- It applies across ordinary synchronized charts.

Qt gap:

- Qt ordinary charts have no sector-boundary axis mode.

TODO:

- [x] Add the global toggle with the same availability rules.
- [x] Resolve sector split positions from existing lap metadata/progress.
- [x] Apply consistent S1/S2/S3 ticks to every participating chart.

### CHART-004 — Synchronized tooltips and secondary crosshairs

Electron evidence:

- `electron-frontend/src/renderer/src/lib/chartCursorSync.tsx`.
- `electron-frontend/src/renderer/src/components/Settings.tsx` exposes
  **Secondary Vertical Crosshair** and **Secondary Horizontal Crosshair**.
- Ordinary chart components participate through the cursor-sync hooks.

Electron behavior to match:

- Hovering one participating chart publishes its X coordinate.
- Other visible participating charts show values at the same coordinate.
- Settings independently control vertical and horizontal crosshair lines on
  secondary charts.
- The directly hovered chart retains its normal crosshair.

Qt gap:

- Qt hover readouts are local to each chart.

TODO:

- [x] Add one Qt chart-cursor coordinator shared by visible ordinary charts and
      render one combined tooltip at the directly hovered chart.
- [x] Synchronize hover values using the active chart coordinate mode: preserve
      the actual hovered X for peers on the same axis kind, and translate through
      nearest-row session time only when crossing time/distance axes.
- [x] Add the two crosshair settings.
- [x] Clear synchronized state when the pointer leaves the chart group or the
      coordinate mode changes.

### CHART-005 — Clickable chart legends

Electron evidence:

- Legend clicks toggle series in:
  - `SpeedRpmChart.tsx`
  - `GearChart.tsx`
  - `InputsChart.tsx`
  - `SteeringChart.tsx`
  - `PowerBreakdownChart.tsx`
  - `TyreTrendCharts.tsx`
  - `GForceChart.tsx`
  - `RideHeightChart.tsx`

Electron behavior to match:

- Clicking a legend entry temporarily hides/shows that series.
- Hidden legend entries are visually muted.
- This is runtime chart state, separate from layout-editor section visibility.

Qt gap:

- Qt graph legends are display-only.

TODO:

- [x] Make legend entries interactive for the equivalent Qt charts.
- [x] Hide/show only the selected series and retain the chart section.
- [x] Keep legend visibility as local runtime state; do not invent persistence.

### CHART-006 — Fixed/dynamic Y-axis settings

Electron evidence:

- `electron-frontend/src/renderer/src/lib/graphSections.ts`:
  `ChartYAxisState`, `TYRE_Y_AXIS_SECTIONS`,
  `POWER_Y_AXIS_SECTIONS`.
- `electron-frontend/src/renderer/src/components/Settings.tsx`:
  **Y Axis Behavior** settings.
- `ThermalPanel.tsx`, `TyreTrendCharts.tsx`, and
  `PowerBreakdownChart.tsx` consume the settings.

Electron behavior to match:

- Fixed/Dynamic selection for Overview and Tyres:
  Surface Temp, Inner Temp, Brake Temp, and Tyre Wear/Life.
- Fixed/Dynamic selection for Power: ERS Harvest.
- A set-all Fixed/Dynamic action.
- Fixed ranges are the ranges declared in `graphSections.ts`; temperature and
  harvest fixed modes can expand above their nominal maxima as implemented there.

Qt gap:

- Qt has no user-facing Y-axis behavior settings.

TODO:

- [x] Add the same per-section settings and set-all action.
- [x] Apply them to the equivalent Qt charts.
- [x] Persist them by page and section.

---

## C. Analysis page

Qt already has metric configuration, colors, visibility, ordering, Y-axis
visibility, current-vs-selected overlay, fixed Lap A/Lap B comparison, zoom, and
pan. The tasks below are the Electron Analysis features still missing.

### ANALYZE-001 — Distance alignment and Delta metric

Electron evidence:

- `electron-frontend/src/renderer/src/lib/analyzeLapData.ts`.
- `electron-frontend/src/renderer/src/lib/lapDelta.ts`.
- `electron-frontend/src/renderer/src/components/AnalyzeScreen.tsx` includes the
  non-removable **Delta** metric.
- `electron-frontend/src/renderer/src/components/charts/AnalyzeTimeChart.tsx`
  renders distance-aligned comparisons and positive/negative delta.

Electron behavior to match:

- Analysis can plot by lap distance when progress data is available.
- Lap A/current and Lap B/comparison are aligned by distance rather than packet
  timestamp.
- Delta is current elapsed time minus comparison elapsed time at a distance.
- Positive and negative delta have independently configurable colors.
- Delta can be moved, hidden, and have its Y axis hidden, but cannot be removed.
- Unsupported recordings show the Electron unsupported-state message rather than
  fabricating distance data.

TODO:

- [x] Add distance-coordinate Analysis rendering.
- [x] Add the Delta metric and its exact controls.
- [x] Keep the existing Qt elapsed-time overlay as the fallback when distance
      data is unavailable.

Implemented in `qt_frontend/src/components/AnalyzePage.{h,cpp}`,
`AnalyzeChart.{h,cpp}`, and `qt_frontend/src/SessionModel.{h,cpp}`. Analysis uses
lap progress for distance alignment and signed delta, retains the elapsed-time
fallback and unsupported-file state, and keeps Delta non-removable while exposing
its order, visibility, axis, and positive/negative colors.

### ANALYZE-002 — Combined and Individual Graphs modes

Electron evidence:

- `electron-frontend/src/renderer/src/components/AnalyzeScreen.tsx`:
  **Individual Graphs** and **Synced Tooltip** toggles.
- `electron-frontend/src/renderer/src/components/charts/AnalyzeTimeChart.tsx`
  renders combined mode.
- `electron-frontend/src/renderer/src/components/charts/AnalyzeStackedTimeCharts.tsx`
  renders one panel per metric.

Electron behavior to match:

- Combined mode overlays selected metrics in one chart.
- Individual Graphs mode gives each visible metric its own stacked panel.
- Individual panels share X navigation.
- Synced Tooltip is available only in Individual Graphs mode.
- Per-metric Y-axis visibility continues to apply.

Qt gap:

- Qt has combined overlay only.

TODO:

- [x] Add the Individual Graphs toggle and stacked layout.
- [x] Synchronize X range/navigation across panels.
- [x] Add the Analysis-only synced-tooltip toggle with Electron's availability
      rule.

Implemented in `qt_frontend/src/components/AnalyzePage.{h,cpp}` and
`AnalyzeChart.{h,cpp}` using panels of the existing shared `ChartView`. Individual
panels link their X axes and navigation, and the tooltip option is enabled only
while Individual Graphs is active.

### ANALYZE-003 — Secondary recording source

Electron evidence:

- `electron-frontend/src/main/application.ts`:
  `analysis:load-file`, `analysis:get-lap-data`,
  `analysis:close-file`.
- `electron-frontend/src/main/bridgeManager.ts`:
  `analysisLoadFile()`, `analysisGetLapData()`, `analysisCloseFile()`.
- `electron-frontend/src/renderer/src/components/AnalyzeScreen.tsx`:
  Open/Replace/Clear Secondary File and file-aware lap selectors.

Electron behavior to match:

- While a primary playback file is open, Analysis can open a second TNRD.
- The secondary filename is shown.
- The user can replace or clear it.
- Lap A, Lap B, and the ordinary comparison selector can select laps from either
  file.
- A circuit mismatch prompts before accepting incompatible files.
- Clearing the file clears selections that referenced it.

Qt gap:

- Qt Analysis reads only the primary playback model.

TODO:

- [x] Add an independently owned secondary reader/controller in the Qt frontend.
- [x] Add Open/Replace/Clear Secondary File controls.
- [x] Add source-qualified lap options and circuit-mismatch confirmation.
- [x] Keep the primary playback transport independent of the secondary reader.

Implemented in `qt_frontend/src/AnalysisFileReader.{h,cpp}` and
`qt_frontend/src/components/AnalyzePage.{h,cpp}`. The secondary TNRD reader owns a
separate worker and lazy lap cache, selectors qualify both files, incompatible
circuits require confirmation, and clear/replace invalidates secondary choices
without changing the primary playback transport.

### ANALYZE-004 — Map comparison view

Electron evidence:

- `electron-frontend/src/renderer/src/components/AnalyzeScreen.tsx` exposes Graph
  and Map views; Map is playback-only.
- `electron-frontend/src/renderer/src/components/AnalyzeMapComparison.tsx`.

Electron behavior to match:

- Map view overlays current/Lap A and comparison/Lap B paths.
- Each path has its own configurable color.
- The map animates the two cars from the primary playback cursor.
- Fixed comparison mode and ordinary current-vs-compare mode both work.
- Existing sector-color and map-dimming settings are honored.

Qt gap:

- Qt Analysis has no map view.

TODO:

- [x] Add the playback-only Graph/Map view selector.
- [x] Render two comparable paths and markers with configurable colors.
- [x] Drive marker positions from the existing primary playback cursor.

Implemented in `qt_frontend/src/components/AnalyzePage.{h,cpp}`,
`AnalyzeMapComparison.{h,cpp}`, and `TrackMapWidget.{h,cpp}`. The map view is
playback-only, preserves the map appearance settings, and supports primary-cursor
animation plus the fixed-lap local transport.

### ANALYZE-005 — Sector comparison and chart-to-map inspection

Electron evidence:

- `AnalyzeScreen.tsx`: **Sector Boundaries** and **Sector Delta** toggles.
- `AnalyzeTimeChart.tsx`: sector ticks, sector-local delta calculation, and
  `onInspectMap`.
- `AnalyzeMapComparison.tsx`: focused map inspection.

Electron behavior to match:

- Sector Boundaries labels S1/S2/S3 on Analysis charts.
- Sector Delta resets the delta basis at sector starts and is disabled unless
  Sector Boundaries is enabled.
- Inspecting a plotted location can switch/focus the map comparison at the
  corresponding lap position.

Qt gap:

- Qt Analysis has none of these three connected behaviors.

TODO:

- [x] Add the two dependent toggles.
- [x] Add sector-local delta behavior.
- [x] Connect chart inspection to the Analysis map focus state.

Implemented in `qt_frontend/src/components/AnalyzePage.{h,cpp}` and
`AnalyzeChart.{h,cpp}`. Sector Delta depends on Sector Boundaries, resets at
resolved sector starts, and chart inspection switches the Analysis view to the
corresponding focused map position.

---

## D. Page layouts and density

### LAYOUT-001 — Input page arrangement and pedal modes

Electron evidence:

- `electron-frontend/src/renderer/src/app/appConfig.ts`:
  `InputPageLayout = 'grid' | 'vertical'` and
  `InputPedalLayout = 'combined' | 'combined2' | 'split'`.
- `electron-frontend/src/renderer/src/components/Settings.tsx` exposes both
  settings.
- `inputChartLayout.ts`, Input rendering in `TabContent.tsx`, and
  `LayoutEditor.tsx` consume them.

Qt gap:

- Qt can hide Input sections but does not expose Electron's page arrangement and
  three pedal presentations.

TODO:

- [x] Add Grid/Vertical Input layout.
- [x] Add Combined/Combined 2/Split pedal layouts with the same resulting chart
      sections as Electron.
- [x] Make the existing Input layout editor reflect the active presentation.
- [x] Persist both choices.

Implemented in `qt_frontend/src/components/InputPage.{h,cpp}`,
`InputChartsWidget.{h,cpp}`, `InputLayout.h`, `EditInputLayoutDialog.{h,cpp}`,
`qt_frontend/src/{GraphViewSettings.h,MainWindow.h,MainWindow.cpp}`, and
`SettingsDialog.cpp`. Input now persists Grid/Vertical and all three pedal modes,
migrates the legacy combined visibility into independent Accelerator/Brake state,
and exposes stable Combined 2 and split graph settings while retaining one shared
chart render target. Source/diff review only; owner runtime checks: exercise all
six presentation combinations, hide each split/combined pedal, mix chart/table
and per-chart windows, reopen the layout editor, and restart to verify migration
and persistence.

### LAYOUT-002 — Misc combined/split series

Electron evidence:

- `appConfig.ts`: separate Combined/Split settings for G-Force and Ride Height.
- `Settings.tsx`: the two controls.
- `miscChartLayout.ts`, `GForceChart.tsx`, `RideHeightChart.tsx`, and
  `LayoutEditor.tsx` consume them.

Electron behavior to match:

- G-Force can be one combined chart or separate Lateral and Longitudinal charts.
- Ride Height can be one combined chart or separate Front and Rear charts.
- The layout editor exposes the sections produced by the selected mode.

Qt gap:

- Qt exposes only one combined G-Force section and one combined Ride Height
  section.

TODO:

- [x] Add the two independent Combined/Split choices.
- [x] Extend the existing Misc layout editor for the generated split sections.
- [x] Persist split-section visibility separately from combined-section
      visibility, as Electron does.

Implemented in `qt_frontend/src/components/MiscPage.{h,cpp}`, `MiscLayout.h`,
`MiscChartsWidget.{h,cpp}`, `EditMiscLayoutDialog.{h,cpp}`, `SettingsDialog.cpp`,
and `qt_frontend/src/{MainWindow.h,MainWindow.cpp,GraphViewSettings.h}`.
Settings → Graphs → Misc offers separate Combined/Split controls, defaulting to
Combined. The editor follows the active mode; combined and split visibility
settings remain independent. Split sections have their own persisted chart/table
and window/reference-lap settings. All panels share the existing ChartView,
coalesced scheduler, incremental sample cursors, and bulk history installation.
Playback requirements follow the active sections. Source/diff review only; owner
runtime checks: try all four mode combinations, toggle each section, switch back
to confirm independent visibility, mix chart/table and time/lap overrides, seek
during playback, and restart to verify persistence.

### LAYOUT-003 — Power Grid/Vertical arrangement

Electron evidence:

- `appConfig.ts`: `PowerPageLayout = 'grid' | 'vertical'`.
- `Settings.tsx`: **Power Layout**.
- `PowerBreakdownChart.tsx` consumes the layout.

Qt gap:

- Qt has the Power section editor but uses one fixed chart arrangement.

TODO:

- [x] Add Grid/Vertical Power arrangement.
- [x] Keep the existing per-card and per-chart visibility controls.
- [x] Persist the arrangement.

Implemented in `qt_frontend/src/components/PowerPage.{h,cpp}`,
`PowerChartsWidget.{h,cpp}`, `EditPowerLayoutDialog.cpp`, `SettingsDialog.cpp`,
and `qt_frontend/src/MainWindow.{h,cpp}`. Settings → Graphs → Power offers
Grid/Vertical, stored in `pageLayouts/power` with Grid as default. Visible sections
pack in Electron order with the final odd grid section spanning the width; the
editor reflects the arrangement. The existing single ChartView and table overlays
are reused. Source/diff review only; owner runtime checks: switch arrangements,
hide nonadjacent charts, mix chart/table views, and restart to confirm persistence.

### LAYOUT-004 — Tyres Grid/Vertical arrangement and layout editor

Electron evidence:

- `appConfig.ts`: `TyresPageLayout = 'grid' | 'vertical'`.
- `Settings.tsx`: **Tyres Layout**.
- `LayoutEditor.tsx`: Edit Tyres Layout with four chart visibility controls.

Qt gap:

- Qt has no Tyres layout editor and no user-selectable Grid/Vertical arrangement.

TODO:

- [x] Add the arrangement setting.
- [x] Add Edit Tyres Layout for Surface Temp, Inner Temp, Brake Temp, and Tyre
      Wear/Life.
- [x] Preserve the existing per-corner card/table settings.

Implemented in `qt_frontend/src/components/TyresPage.{h,cpp}`,
`TyreChartsWidget.{h,cpp}`, `SettingsDialog.cpp`, and
`qt_frontend/src/MainWindow.{h,cpp}`. Settings → Graphs → Tyres offers
Grid/Vertical, persisted in `pageLayouts/tyres` with Grid as default. The Tyres
toolbar's Edit Layout action opens four immediate visibility controls stored
under `tyresLayout/charts`. These affect only the Tyres graphs; allocation cards
and the Overview strip retain their settings. Source/diff review only; owner
runtime checks: edit all four sections in both arrangements, hide all and restore,
mix chart/table views, and restart to verify saved visibility and arrangement.

### LAYOUT-005 — Standings layout editor

Electron evidence:

- `appConfig.ts`: `StandingsLayout`.
- `LayoutEditor.tsx`: timing-tower visibility, Timing/Energy
  Recovery/Strategy card visibility, and draggable sidebar percentage.

Qt gap:

- Qt has no Standings layout editor.

TODO:

- [x] Add Edit Standings Layout.
- [x] Add timing-tower visibility.
- [x] Add the three card visibility controls.
- [x] Add and persist the sidebar width percentage.

Implemented in `qt_frontend/src/components/StandingsPage.{h,cpp}` and
`qt_frontend/src/MainWindow.cpp`. The toolbar editor provides Timing Tower,
Timing, Energy Recovery, and Strategy toggles plus a draggable sidebar split.
All changes apply immediately and persist under `standingsLayout`; sidebar width
defaults to 28% and is clamped to 15–60% as in Electron. The driver header belongs
to Timing, separators follow visible cards, and the remaining side fills the page
when its counterpart is hidden. Existing row selection, data updates, and table
column sizing are retained. Source/diff review only; owner runtime checks: toggle
each section, hide all and restore, drag to both width limits, resize the window,
select drivers, and restart to confirm saved layout.

### DENSITY-001 — Spacious density and missing density sections

Electron evidence:

- `electron-frontend/src/renderer/src/lib/graphSections.ts`:
  `DensityMode = 'compact' | 'normal' | 'spacious'`,
  `CompactState`, and all density option lists.
- `electron-frontend/src/renderer/src/components/Settings.tsx`:
  per-section controls plus **Set All Compact**, **Set All Normal**, and
  **Set All Spacious**.

Electron behavior to match:

- Compact/Normal/Spacious for:
  Overview Stats/Damage; all four Standings sections; Session Cards/Proximity/
  Events; Power Cards; Strategy Summary; Playback Bar.
- Multi-level controls exactly as declared for Overview Tyre Cards, Session
  Weather, and Session Header.
- The three set-all actions.

Qt gap:

- Qt's compact model is mostly Boolean.
- It has no Spacious mode.
- It lacks Electron's Standings and Playback Bar density sections.

TODO:

- [x] Extend Qt density state to represent the Electron options.
- [x] Add the missing section controls and set-all actions.
- [x] Migrate existing Qt Boolean compact values to Compact/Normal without losing
      the user's choices.
- [x] Match the visible information/padding behavior of each Electron density
      option; do not invent additional levels.

Implemented in `qt_frontend/src/CompactSettings.h`, `MainWindow.{h,cpp}`,
`PlaybackController.{h,cpp}`, and the Overview, Standings, Session, Power,
Strategy, Tyre Cards, and Settings components. Ordinary values migrate from the
former Boolean representation and persist as Compact/Normal/Spacious strings.
The specialized tyre/weather/header controls expose the exact Electron values,
including Spacious, and all three bulk actions map to the declared special
levels. Standings and Playback now participate in density settings. Source/diff
review only; owner runtime checks: exercise every segment and all three bulk
actions, inspect compact/spacious information and padding, then restart with old
Boolean and new string settings.

---

## E. Strategy and notifications

### Strategy page restoration — pre-PR #12 layout

Restored `StrategyPage.{h,cpp}` from `f9c80b8`, the first parent of PR #12's
merge commit `80f64bd`, then adapted that page against the current Electron
`StrategyPanel.tsx` and the public `tnrp::StrategySnapshotRow` interface.

- Retains the full-width summary, Defensive/Attacking plan cards, nested stint
  cards, and proportional sidebar from that Qt revision.
- Tables sit flush with their columns: no outer gutters, plan/stint container
  margins, inter-card spacing or table frames. Text padding belongs to labels.
- Continuous vertical separators divide the two complete plan columns and the
  sidebar. Horizontal separators sit below plan headings, between stints and
  above the required-stop footer. Waiting and ready sidebars have no card frame.
- The summary displays average wear, tyre age, wear rate and cliff, with
  Compact/Normal/Spacious variants; Spacious also shows distance and limiting tyre.
- Plan rows display Required, Actual, lap delta, stint delta and total delta
  directly from the snapshot, with Electron's precision, missing-value handling,
  delta colours and pit-lap highlighting. The plan scrolls the complete stint.
- Preserves target, confidence, legality and compound-change information.
- Sidebar order follows Electron: SC/VSC decision, race call, pit windows,
  applicable weather window, rivals, tyre condition and wear alerts. Waiting
  placeholders and the required-stop footer follow Electron's state rules.
- No frontend-adjusted lap targets or additional position section. The existing
  engine signal connection and visible-page refresh scheduling are retained.
- Snapshot refreshes preserve the pit-stop editor and scroll positions; palette
  changes refresh the page colours, including intermediate green and wet blue.

Verification is limited to source/diff review and a syntax-only compiler check.
There are no Qt strategy-page tests. Visual layout, scrolling, density changes,
live/playback updates and editing during refresh still require runtime validation;
the repository's `build-or-run` skill prohibits building or launching the app.

### STRATEGY-001 — Required pit stops

Electron evidence:

- `electron-frontend/src/renderer/src/components/StrategyPanel.tsx`:
  `MinimumStopsControl`.
- `electron-frontend/src/preload/index.ts`:
  `strategyBridge.setMinimumStops()`.
- `electron-frontend/src/main/application.ts` forwards
  `strategy-set-minimum-stops`.

Electron behavior to match:

- **Required pit stops** number control from 0 through 8.
- Default value 1 when the Strategy component is created.
- Visible in waiting and ready Strategy sidebars.
- Changes immediately call the strategy minimum-stops API.
- Electron does not persist this value.

Qt gap:

- Qt Strategy has no minimum-stops control.

TODO:

- [x] Add the 0–8 control with default 1.
- [x] Show it in the waiting and ready states.
- [x] Apply changes immediately through the existing public engine API.
- [x] Do not add persistence as part of parity with the current Electron
      implementation.

Implemented in `qt_frontend/src/components/StrategyPage.{h,cpp}` and
`qt_frontend/src/MainWindow.cpp`. The sidebar footer remains outside the scrolling
summary and is hidden in non-race sessions. Source/diff review only; owner runtime
checks: adjust 0–8 in waiting/ready states, check refreshed plans, and restart the
application to confirm default 1.

### NOTIFY-001 — New Race Leader notification

Electron evidence:

- `electron-frontend/src/renderer/src/app/components/RaceLeaderWatcher.tsx`.
- `electron-frontend/src/renderer/src/app/hooks/useRaceBanners.ts`:
  `handleLeaderChange()`.
- `AppShell.tsx` disables the watcher during playback.

Electron behavior to match:

- During live telemetry, find the active car at position 1 with
  `result_status === 2`.
- The first observed leader establishes state silently.
- A changed car index shows **New Race Leader** with the driver's last name.
- The notification uses the existing notification duration.
- It is disabled during playback.

Qt gap:

- Qt's toast event mapper has no leader-change watcher.

TODO:

- [x] Add the live-only watcher and route changes through the existing Qt toast
      system.
- [x] Reset its state with the session lifecycle.

Implemented in `qt_frontend/src/MainWindow.{h,cpp}` and
`qt_frontend/src/components/ToastEvents.{h,cpp}`. The first qualifying leader is
silent; session start, engine recreation, and playback transitions reset the
watcher. Source/diff review only; owner runtime checks: change the live leader,
confirm surname and configured duration, and verify playback stays silent.

---

## F. Runtime and appearance

### RUNTIME-001 — Reduce Animations

Electron evidence:

- `Settings.tsx`: **Reduce Animations**.
- `useAppConfiguration.ts` persists `reduceAnimations`.
- Reachable consumers include `AppShell.tsx`, `AnalyzeScreen.tsx`,
  `AnalyzeMapComparison.tsx`, `TimingTower.tsx`, and the modal/presence
  helpers.

Electron behavior to match:

- One persisted application setting suppresses decorative transitions and
  motion while leaving data updates and controls functional.

Qt gap:

- Qt has no equivalent preference.

TODO:

- [x] Add the setting.
- [x] Make existing/new Qt decorative animations honor it.
- [x] Do not disable telemetry updates, playback progress, or warning display.

Implemented in `qt_frontend/src/MainWindow.{h,cpp}`,
`components/SettingsDialog.cpp`, `components/TrackMapWidget.{h,cpp}`, and
`components/ToastHost.cpp`. The persisted preference snaps track-map driver and
camera motion and makes toast entrance/exit immediate. It does not change model
updates, warning presentation, playback transport, or telemetry routing.
Source/diff review only; owner runtime checks: toggle during live map movement,
show transient/persistent toasts in both modes, and verify playback and warnings.

### RUNTIME-002 — Focused and unfocused chart FPS

Electron evidence:

- `Settings.tsx`: **FPS in focus** and **FPS out of focus**.
- Choices: Pause, 1, 10, 30, 60, 120, Match display.
- `electron-frontend/src/renderer/src/lib/timechart/frameRate.ts` and chart
  components consume the values.

Qt gap:

- Qt has visibility-based render suspension but no user-selectable focused/
  unfocused chart repaint cap.

TODO:

- [x] Add both settings with the Electron choices.
- [x] Apply them to chart repaint scheduling only.
- [x] Keep Qt's existing hidden/minimized rendering suspension and continuous
      ingest/recording behavior.

Implemented in `qt_frontend/src/PresentationScheduler.h`,
`MainWindow.{h,cpp}`, `TelemetryChart.cpp`, the chart widgets, and
`components/SettingsDialog.{h,cpp}`. Both persisted controls expose Pause, 1,
10, 30, 60, 120, and Match display. Only requests using the Chart scheduling
policy observe the cap; UI coalescing, map animation, ingestion, recording, and
the existing hidden-window gate retain their own behavior. Source/diff review
only; owner runtime checks: exercise every rate in/out of focus and Pause, then
minimize/restore while recording.

### RUNTIME-003 — Lap-comparison delta update frequency

Electron evidence:

- `Settings.tsx`: **Delta Updates** with Realtime, 250 ms, 500 ms, and
  1 second.
- `SessionTimer.tsx` calculates and displays the lap-comparison delta.

Qt gap:

- Qt exposes no user-selectable update cadence for the toolbar's lap-comparison
  delta.

TODO:

- [x] Add the four choices.
- [x] Apply the selected cadence only to the lap-comparison delta.
- [x] Keep the session timer's existing update behavior unchanged.
- [x] Do not change telemetry ingest or recording cadence.

Implemented in `qt_frontend/src/MainWindow.{h,cpp}` and
`components/SettingsDialog.cpp`. Realtime, 250 ms, 500 ms, and 1 second are
persisted and gate only recalculation of the toolbar comparison delta; chart
configuration changes still refresh it immediately. The independent session
timer remains packet-driven and telemetry/recording paths are unchanged.
Source/diff review only; owner runtime checks: compare laps at each cadence in
live and playback modes while confirming the session clock stays responsive.

---

## G. Playback and desktop integration

### PLAY-001 — Replay from end of file

Electron evidence:

- Electron's play action calls `Engine::playerPlay()` through
  `bridgeManager.playerPlay()`.
- `protocol_parser_library/src/Engine.cpp`, `Engine::playerPlay()`, rewinds to
  the recording start when the cursor is at the end before resuming.

Qt gap:

- The engine-backed Qt player already rewinds the engine cursor at EOF, but
  does not reconstruct Qt's installed history and panel state through a seek.

TODO:

- [x] When Play is pressed at EOF, reset Qt playback to the recording start and
      begin playing.
- [x] Ensure UI/model state agrees with the rewound position.
- [x] Use the existing Qt playback seek/reconstruction path; do not change the
      shared Engine for this task.

Implemented in `qt_frontend/src/TnrdPlayer.cpp` and
`qt_frontend/src/PlaybackController.cpp`. EOF Play requests a seek to zero, then
resumes after history and panel installation; pause/close/load/engine replacement
clear pending resume intent. Source/diff review only; owner runtime checks: play
to EOF and replay, seek to EOF and replay, inspect chart history and transport at
the recording start, and close a recording while its replay seek is pending.

### PLAY-002 — Loaded filename and shared dialog directory

Electron evidence:

- `AppHeader.tsx` and `AppHeaderMacOS.tsx` show the loaded
  `filename` in the title bar and replace it with the Open button when no file
  is loaded.
- `electron-frontend/src/main/application.ts` stores
  `dialogs.lastDirectory`.
- The stored directory is shared by recording-folder selection, TNRD open, and
  XLSX export and is updated after a successful selection.

Qt gap:

- Qt's toolbar does not persistently show the loaded recording filename.
- Qt dialogs do not implement Electron's shared last-directory behavior.

TODO:

- [x] Show the current playback filename persistently near the Qt Open action.
- [x] Add one shared last-dialog-directory setting.
- [x] Use it for recording-folder selection, playback Open, and XLSX export.
- [x] Update it only after a successful selection.

Implemented in `qt_frontend/src/AppToolbar.{h,cpp}`,
`qt_frontend/src/MainWindow.{h,cpp}`, and
`qt_frontend/src/components/SettingsDialog.cpp`. The filename stays inline with
middle elision and a full-name tooltip, and clears on close/load failure. Dialogs
share `dialogs/lastDirectory`; canceled dialogs do not update it. As in Electron,
Open remembers a selection before the separate load confirmation, and export
remembers its destination before writing. Source/diff review only; owner runtime
checks: open/close recordings, resize with a long filename, alternate all three
dialogs, cancel selections, and verify the directory survives an app restart.

### DESKTOP-001 — Single instance and operating-system file opening

Electron evidence:

- `electron-frontend/src/main/index.ts` acquires the single-instance lock.
- `electron-frontend/src/main/application.ts`:
  `getFilePathFromArgs()`, `second-instance`, and `open-file`.
- Runtime accepts existing `.tnrd` and `.trnd` paths.
- `electron-frontend/package.json` registers `.tnrd` file associations for
  packaged Windows and macOS builds.

Electron behavior to match:

- Only one application instance remains active.
- Launching a second instance restores/focuses the first.
- A file passed to the second instance is offered to the first through the
  existing open-confirmation flow.
- Startup command-line and macOS open-file paths are handled.

Qt gap:

- Qt has no single-instance coordinator or external file-open routing.
- Its package metadata does not register equivalent `.tnrd` opening.

TODO:

- [x] Add single-instance activation and file-path transfer.
- [x] Handle startup arguments and macOS file-open events where supported.
- [x] Route external files through the same confirmation/load code as toolbar
      Open.
- [x] Add `.tnrd` associations to the Qt Windows and macOS packages that this
      repository ships. Do not claim a Linux association unless one is actually
      added and tested.

### DESKTOP-002 — System tray and Background Mode

Electron evidence:

- `electron-frontend/src/main/application.ts` creates a `Tray` with Show and
  Quit, click-to-show, tooltip, and light/dark icon updates.
- `AppHeader.tsx` and `AppHeaderMacOS.tsx` expose **Background Mode**, which
  hides the window.

**Status: Not needed.** The repository owner excluded this feature from Qt
parity and requested removal of its implementation. Do not implement it as
part of this backlog.

### DESKTOP-003 — Update discovery

Electron evidence:

- `electron-frontend/src/main/updateChecker.ts`.
- `Settings.tsx`: **Check for Updates** toggle.
- `AppShell.tsx`: startup check.
- `UpdateAvailableDialog.tsx`: installed/latest versions, **Skip this
  version**, **Remind me Later**, and **Download**.

Electron behavior to match:

- Update checks are enabled by default and persisted.
- Startup checks are limited to once per 24 hours.
- A skipped version is not shown again.
- The dialog reports installed/latest versions.
- Download opens the GitHub Releases page; it does not auto-install.
- Failed checks log and return without blocking startup.

Qt gap:

- Qt has no update setting, checker, or dialog.

TODO:

- [x] Add the persisted toggle, last-check time, and skipped version.
- [x] Implement the same startup-check cadence and version comparison.
- [x] Add the same three dialog actions.
- [x] Open the release page externally; do not add automatic installation.

Implemented in `qt_frontend/src/UpdateChecker.{h,cpp}`, `MainWindow.h`,
`components/SettingsDialog.cpp`, and `qt_frontend/CMakeLists.txt`. Startup uses
the GitHub latest-release API, records the attempt before the request, applies
SemVer precedence (including prereleases), ignores the selected skipped version,
and fails through logging without blocking launch. The native dialog shows
installed/latest versions with Skip this version, Remind me Later, and Download;
Download opens the Releases page externally. Qt Network was already linked, so
no dependency was added. Source/diff review only; owner runtime checks: exercise
success/failure, the 24-hour guard, skip/remind/download, and the enabled toggle.

### DESKTOP-004 — Launch diagnostics and bridge-failure report

Electron evidence:

- `electron-frontend/src/main/diagnostics.ts` creates a per-launch main log,
  Chromium log, and crash-dump directory before loading the application module.
- It records startup metadata and replaces prior-launch diagnostic files.
- `electron-frontend/src/main/application.ts` installs the recording flush
  handler and, on native bridge startup failure, copies a diagnostic report to
  the clipboard and shows **Telemetry Bridge Failed to Load**.

Electron behavior to match:

- Per-launch diagnostic logging starts early.
- Startup metadata and fatal process errors are captured.
- Recording is flushed through the supported frontend shutdown/fatal path where
  possible.
- A startup engine failure shows a diagnostic message and copies the report to
  the clipboard.
- Electron does not expose a general-purpose normal-state “Save support report”
  feature.

Qt gap:

- Qt does not provide this equivalent early launch log/crash-report flow.

TODO:

- [x] Add early Qt/application diagnostic logging with equivalent version,
      platform, and startup context.
- [x] Capture Qt messages and fatal startup context.
- [x] On engine startup failure, copy the report and show an equivalent modal.
- [x] Do not add unrelated telemetry contents or a new general support-report UI
      under this parity item.

Implemented in `qt_frontend/src/Diagnostics.{h,cpp}`, `main.cpp`,
`MainWindow.cpp`, and `qt_frontend/CMakeLists.txt`. The primary process replaces
the prior `launch-diagnostics/main.log` before graphics/engine startup, records
application/Qt/platform/OS/executable/working-directory/command-line/locale
context, captures Qt messages and `std::terminate`, and registers a best-effort
recording flush for fatal paths. A native engine construction failure logs and
copies the report, then shows **Telemetry Bridge Failed to Load**; normal UDP
bind errors retain their existing dialog. No telemetry data or general support
report UI was added. Source/diff review only; owner runtime checks: inspect log
replacement and metadata, force engine construction failure, verify clipboard
and modal, and confirm orderly/fatal recording recovery.

### DESKTOP-005 — Application fullscreen

Electron evidence:

- `AppHeader.tsx` exposes **Fullscreen**/**Exit Fullscreen**.
- `electron-frontend/src/main/application.ts` handles
  `window-fullscreen` and publishes fullscreen state.
- `FullscreenBanner.tsx` provides the fullscreen hint/banner behavior.

**Status: Not needed.** The repository owner excluded this feature from Qt
parity and requested removal of its implementation. Do not implement it as
part of this backlog.


---

## Suggested implementation order

1. SAFE-001, SAFE-002, NET-001, NET-002.
2. CHART-001 through CHART-006.
3. ANALYZE-001 through ANALYZE-005.
4. LAYOUT-001 through LAYOUT-005 and DENSITY-001.
5. STRATEGY-001 and NOTIFY-001.
6. Runtime, theme, playback, and desktop tasks.

Do not use this ordering—or permission to implement any listed item—as permission
to modify `protocol_parser_library/` or another shared-library contract.

## Verification checklist for every completed item

- [x] The behavior can still be demonstrated in the cited Electron source.
- [x] Qt matches the cited behavior without adding unrequested actions or states.
- [x] Existing Qt functionality listed near the top of this document still works.
- [x] Settings persist only where Electron persists them or where existing Qt
      behavior already requires persistence.
- [x] Live/playback applicability matches Electron.
- [x] Hidden/background behavior does not stop UDP ingest or recording.
- [x] No file under `protocol_parser_library/` was changed. If the repository
      owner separately authorized a specific library change, record that explicit
      permission and keep it outside the assumed scope of this parity task.
- [x] No build, test, linter, type-checker, formatter, benchmark, application, or
      packaging command was run; execution verification is left to the repository
      owner unless they separately and explicitly authorized it.
- [x] No new dependency was added without attribution and license review.
- [x] The task is checked off and annotated with the implementing Qt files.

Checklist assessment is from read-only source and diff review, not execution.
“Still works” here means the pre-existing call sites, data paths, settings, and
visibility gates remain connected in source; runtime verification remains the
repository owner's responsibility under the build/test freeze above.

## Final parity re-audit

After completing this backlog:

- [x] Enumerate every reachable Electron header action, Settings row, layout
      editor, Analysis control, playback action, modal, and lifecycle handler.
- [x] Compare it against current Qt source again.
- [x] Add a new task only when its Electron implementation can be cited.
- [x] Remove tasks whose Electron behavior no longer exists.
- [x] Record deliberate product exceptions separately; do not disguise proposed
      features as parity gaps.

Re-audited 2026-09-15 by source inspection. The cited header, Settings, layout,
Analysis, playback, modal, and lifecycle behavior remains reachable and every
backlog item above now has a Qt implementation annotation. No cited behavior was
removed, so no task was deleted. **Background Mode/System Tray** and
**Application Fullscreen** remain the owner's explicit product exceptions in
DESKTOP-002 and DESKTOP-005; native titlebar/window controls remain native Qt
behavior rather than copied Electron chrome. No additional feature was folded
into this fixed-revision backlog without an independently cited parity task.
