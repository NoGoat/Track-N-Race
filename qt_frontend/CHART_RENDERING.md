# Chart rendering contract

`ChartView` follows the vendored Timecharts fork in
`electron-frontend/src/renderer/src/lib/timechart/engine/`, particularly
`plugins/lineChart.ts`, `core/nearestPoint.ts`, and `chartZoom/wheel.ts`.
Application configuration comes from `TimeChartView.tsx`,
`SpeedRpmTimeChart.tsx`, `InputsChart.tsx`, and `GearChart.tsx`.

## Series geometry

- `LineType::Line` connects samples linearly. Width is in logical pixels,
  including fractional widths, and is expanded by the GPU independently of
  hardware wide-line support. Resizing and changing ranges update uniforms;
  they do not regenerate the retained geometry.
- `LineType::Step` (or the existing `step` flag) transitions at
  `x0 + stepLocation * (x1 - x0)`. Locations 0 and 1 use two segments;
  intermediate locations use three. For samples `(0, 2), (10, 8)`, a location
  of 0.5 holds 2 until x=5, changes vertically to 8, then holds 8 until x=10.
- Fills follow exactly the same path, including every step corner, and extend
  to `fillBaseline`. All fills render before all strokes. Explicit fill alpha
  is preserved; an unspecified fill uses 20% of the series color's alpha.
- `opacity` multiplies both stroke and fill alpha. Width/opacity changes only
  update uniforms; line-type, step-location and baseline changes invalidate
  the affected geometry.
- `NativeLine` retains a compact hardware line strip. Ordinary All Laps and
  Stint Laps views select it for continuous series, as Electron does. Native
  line thickness remains backend-dependent; use `Line` for portable widths.
- `NativePoint` draws independent square markers with the requested logical
  pixel size. It uses instanced quads so point size also works on APIs without
  configurable native point size. A single valid sample can produce a marker.
- Non-finite Y values break strokes and fills. Non-finite X values are ignored.
  Duplicate X values preserve source order. Bulk input is stably sorted if
  needed to preserve the binary-search contract.
- Trimming retains one sample before the cutoff, so a segment crossing the
  left plot edge can be clipped instead of disappearing prematurely.

Pedals use start-of-interval steps; gear uses end-of-interval steps, a 0.5
baseline, and a 0.5–8.5 range with integer ticks. Overview Speed/RPM/ERS use
continuous lines. Analysis step metrics retain end-of-interval steps.

## Retention and interaction

CPU X keys remain doubles. GPU X coordinates are relative to a retained series
origin. Changed suffixes repair the preceding segment and fill corners. Buffer
creation, growth, compaction and graphics-device recreation upload the complete
required geometry. Unchanged data, including paired NaNs, does not upload again.
Bulk replacement checks the entire shared prefix, not just its endpoints.

Finite runs are cached and repaired with changed data. A scrolling frame uses
binary search to find visible runs, instead of scanning each visible sample for
NaNs. There is no per-pixel decimation. Qt retains its per-series buffers and
single render target for multiple panels; it does not share the fork's WebGL
texture arrays or aligned circular storage across channels.

Nearest-sample hover chooses the earlier sample on an exact midpoint tie;
it does not interpolate tooltip values. Pointer work is coalesced on a 16 ms
single-shot timer and also refreshed when data changes under a stationary
pointer. Synchronized cursor changes only repaint the overlay. The source panel
is included once in the tooltip, and leaving the plot clears linked cursors.

Navigation applies within a navigable plot. Ctrl/Meta-wheel zooms about the
pointer, Shift accelerates the operation, and Alt selects horizontal wheel
motion. Pixel deltas support trackpads. Dragging uses plot width and updates
incrementally at bounds. Left double-click resets; right double-click retains
the existing inspection action. Toolbar zoom remains centered.

New Y extrema expand immediately, including peaks inside an appended batch;
full-window rescans and shrinking retain the 200 ms throttle.

## Fractional scaling and labels

The application explicitly uses Qt's `PassThrough` DPI rounding policy. Chart
viewports preserve fractional physical coordinates at scales such as 125%, 150%
and 175%; only the integer scissor rectangle rounds outward. The painter overlay
uses the same continuous plot edges as the GPU, avoiding the one-logical-pixel
offset introduced by `QRect::right()` and `bottom()`.

Axes, grid lines and crosshairs use cosmetic one-device-pixel strokes aligned
in the painter's device coordinates. Text stays at fractional logical positions,
with device-aware floating font measurements. Layout is refreshed on screen,
DPR, font and locale changes. Label boxes use the actual font height, and crowded
axes omit overlapping labels while retaining tick marks and grid lines.

Chart labels and tooltips request tabular digits from the active font. Axis
numbers use the locale's decimal separator, omit grouping and unnecessary
trailing zeros, and increase precision for fractional tick spacing. For example,
0.25, 0.5 and 0.75 remain distinct even if an axis started with precision zero.
Near-zero negative values render without a minus sign when they round to zero.
Explicit tooltip precision and optional thousands grouping are retained.

Duration axes honor `%h`, `%m`, `%s` and `%z` (milliseconds). Ordinary ticks use
`m:ss`; subsecond zoom adds enough decimal places to distinguish adjacent ticks.
Indexed tick generation avoids accumulating floating-point addition error.

### Text blending above the GPU plot

The overlay's raster surface starts transparent; the GPU's background is added
later by the widget compositor. Painting text directly onto that surface triggers
Qt's fallback glyph blending for non-opaque destination pixels, making chart
text appear thin on a dark background. This differs from ordinary widgets whose
background has already been painted. See Qt's
[`alphamapblend_argb32` and `alphargbblend_argb32`](https://codebrowser.dev/qt6/qtbase/src/gui/painting/qdrawhelper.cpp.html#5411).

The overlay now paints opaque, palette-matched chart margins and headers before
text and controls. Text boxes inside the plot also receive an opaque background,
rounded outward to physical pixels. The plot itself stays transparent so GPU
traces remain visible. Font size and weight are unchanged. This addresses the
source-level blending difference; visual confirmation remains an owner check.

## Verification status and owner checks

This change received source/diff review only. No chart-specific automated tests
exist in this repository. No build, shader compilation, application launch or
runtime validation was performed, in accordance with the project guardrail.
Exact visual equivalence across graphics backends is not yet verified.

After building with the existing Qt build workflow, check:

1. Render `(0, 2), (10, 8), (20, 4)` with step locations 0, 0.5 and 1, both
   with and without fills. Compare with the fork; at 0.5 the transitions must
   be at x=5 and x=15. Verify baselines 0 and 0.5 and an opaque explicit fill.
2. Hover exactly halfway between samples and then just either side. The tie
   must select the earlier sample. Leave the pointer still during playback;
   values should track the data without duplicated synchronized rows.
3. Append `(30, NaN), (40, 6), (50, 7)` and confirm both the stroke and fill
   stay broken at the gap. Repeat with bulk replacement and trimming.
4. Replace the middle Y of an otherwise identical three-point series. Then
   shorten, clear, refill, insert an older point, and append past buffer growth
   and compaction boundaries. Check for stale vertices or spurious connections.
5. Pan the left edge into the middle of a segment. Resize stacked panels and
   switch between time, distance, Stint Laps and All Laps views. Check that
   fills, steps and reference lines stay within their own plot.
6. Compare 1, 1.5 and 2.25 pixel strokes at 100% and 200% display scaling on
   available graphics backends, including MSAA off/on. Exercise graphics/style
   reinitialization and check shader/resource creation diagnostics.
7. Check pedal transitions, gear ticks/fill/reference lines, overview ERS,
   analysis gaps, cursor-centered wheel zoom, trackpad scrolling, and dragging
   against both navigation bounds.
8. Append a batch with a large peak in its middle while automatic Y fitting
   is throttled. The peak should be visible immediately; shrinking should
   wait until the next full scan.

9. Repeat resizing at 100%, 125%, 150%, 175% and 200%, and move the window
   between monitors with different scaling. Check grid/trace alignment, the
   upper Y label, label clipping, and cursor positioning in every panel.
10. Check quarter-unit numeric ticks, small negative values that round to zero,
    RPM `k` suffixes, explicit gear ticks, subsecond time zoom, and locales with
    comma decimal separators. Adjacent labels must remain distinct and must not
    overlap in narrow or short panels.
11. Compare chart labels, legends and window selectors against ordinary labels
    in dark and light themes at 100%, 125% and 150%. Check both header legends
    and legends inside plots, confirming normal text weight and visible traces.
