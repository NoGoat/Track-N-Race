import { useEffect, useRef } from 'react'
import { useSize } from '../../hooks/useSize'
import { useChartTooltip, TOOLTIP_STYLE } from '../../hooks/useChartTooltip'
import { useTimeChartScroll } from '../../hooks/useTimeChartScroll'
import { TimeChart, corePlugins, type TChart } from '../../lib/timechart/tc'
import { TimeChartDataBridge } from '../../lib/timechart/dataBridge'
import type { AlignedSeriesData } from '../../lib/timechart/engine/core/alignedData'
import { createAxisPlugin, type AxisConfig } from '../../lib/timechart/axisPlugin'
import { createReferenceLinesPlugin, type RefLine, type RefLinesConfig } from '../../lib/timechart/referenceLines'
import { createTimeChartDrawProfilerPlugin } from '../../hooks/useTimeChartDrawProfiler'
import { niceTicks } from '../../lib/timechart/ticks'
import { createAreaFillPlugin } from '../../lib/timechart/areaFill'
import type { CSSProperties } from 'react'

// Reusable WebGL chart. This is the migration target that replaces per-chart
// <UPlotReact> usage: it owns TimeChart creation/disposal, the incremental data
// bridge, the real-time scroll loop, the custom axis / reference-line / tooltip
// plugins, resize and in-place theme updates. The Misc-page leaves (G-Force /
// Ride Height) are thin consumers; the remaining uPlot charts adopt it later.
//
// The chart is created ONCE and updated imperatively (unlike uplot-react, which
// destroys+recreates on option changes) — recreating a WebGL context per theme
// toggle or resize would be wasteful and is the whole reason we moved off uPlot.

function zeroColorFor(isDark: boolean) { return isDark ? 'rgba(255,255,255,0.2)' : 'rgba(0,0,0,0.18)' }
function refColorFor(isDark: boolean) { return isDark ? 'rgba(255,255,255,0.04)' : 'rgba(0,0,0,0.07)' }
function overlayBgFor(isDark: boolean) { return isDark ? '#12141f' : '#ffffff' }

export interface ChartColors {
  axis: string
  grid: string
  border: string
  /** x tick-mark colour; falls back to `axis` when omitted. */
  tickMark?: string
}

// Default theme colours (matches the Misc-page charts).
function defaultColors(isDark: boolean): ChartColors {
  return {
    axis: isDark ? '#7c8098' : '#6b7280',
    grid: isDark ? 'rgba(255,255,255,0.04)' : 'rgba(0,0,0,0.07)',
    border: isDark ? '#1e2136' : '#d0d5e0',
  }
}

// Axis look-and-feel knobs (defaults reproduce the Misc-page charts).
export interface AxisLook {
  font?: string
  /** paddingBottom — space reserved for the x axis (uPlot x `size`). */
  xAxisSize?: number
  paddingRight?: number
  paddingLeftExtra?: number
  xGap?: number
  yGap?: number
  xTickSpacePx?: number
  gridDash?: number[]
  showYGrid?: boolean
  /** x tick-mark length in px; 0/undefined = no tick marks. */
  xTickSize?: number
}

const DEFAULT_FONT = '11px "Cascadia Code", ui-monospace, monospace'

// How often an 'auto' y-range does a full rescan of the visible window (which
// lets the axis shrink again). Between rescans the range only expands via the
// newest sample, so nothing ever clips — only shrinking is delayed, which is
// imperceptible. 200ms keeps the scan cost low at large windows.
const AUTO_RANGE_FULL_MS = 200

export interface SeriesDef<T> {
  label: string
  color: string
  getY: (row: T) => number
  /** Toggle a series in place without rebuilding the WebGL chart. */
  visible?: boolean
  lineWidth?: number
  /** TimeChart line type: 0 = line, 1 = step. */
  lineType?: 0 | 1
  /** Step position within an interval (0 = before, 1 = after). */
  stepLocation?: number
  /** Optional translucent area fill from this baseline to the series. */
  fill?: string
  fillBaseline?: number
}

export type YRangeSpec =
  | { kind: 'fixed'; min: number; max: number }
  | { kind: 'expand'; initialLower: number; initialUpper: number; lowerPad: number; upperPad: number; expandLower?: boolean }
  // Fit to the visible window each update (matches uPlot's auto-range), with
  // padding and nice round ticks.
  | { kind: 'auto'; padFraction?: number; tickCount?: number; fixedMin?: number }

export interface TimeChartViewProps<T> {
  isDark: boolean
  rows: readonly T[]
  getX: (row: T) => number
  series: SeriesDef<T>[]
  windowSeconds: number
  yRange: YRangeSpec
  /** width reserved for the y-axis labels (uPlot axis `size`). */
  yAxisSize: number
  /** required for fixed/expand ranges; ignored for `auto` (nice ticks derived). */
  yTickValues?: (min: number, max: number) => number[]
  yTickFormat: (v: number) => string
  xTickFormat: (seconds: number) => string
  refLines?: RefLine[]
  /** builds the tooltip HTML from the cursor's x and the per-series y values. */
  tooltipFormat: (x: number, values: number[]) => string
  profilerLabel?: string
  /** theme colour resolver; defaults to the Misc-page palette. */
  colorsFor?: (isDark: boolean) => ChartColors
  axisLook?: AxisLook
  tooltipStyle?: CSSProperties
  /** Keep WebGL scrolling at display rate while throttling full SVG/plugin work. */
  fastScroll?: boolean
  /** Use the dense telemetry session clock for a sparse status/damage series. */
  followSessionClock?: boolean
  /** Sparse-stream coast window before the scheduler parks this chart. */
  minScrollStallS?: number
}

export default function TimeChartView<T>(props: TimeChartViewProps<T>) {
  const {
    isDark, rows, getX, series, windowSeconds, yRange, yAxisSize,
    yTickValues, yTickFormat, xTickFormat, refLines, tooltipFormat, profilerLabel,
    colorsFor = defaultColors, axisLook, tooltipStyle = TOOLTIP_STYLE, fastScroll,
    followSessionClock, minScrollStallS,
  } = props

  const look = axisLook ?? {}
  const font = look.font ?? DEFAULT_FONT
  const padFraction = yRange.kind === 'auto' ? (yRange.padFraction ?? 0.1) : 0.1
  const tickCount = yRange.kind === 'auto' ? (yRange.tickCount ?? 5) : 5

  // For auto ranges the tick values are nice round numbers derived from the
  // (padded) domain; for fixed/expand the caller supplies them.
  const effYTickValues = yRange.kind === 'auto'
    ? (min: number, max: number) => niceTicks(min, max, tickCount)
    : (yTickValues ?? ((min: number, max: number) => [min, max]))

  const { ref: sizeRef, width, height } = useSize()
  const { tooltipRef, show, hide } = useChartTooltip()

  const containerRef = useRef<HTMLDivElement>(null)
  const chartRef = useRef<TChart | null>(null)
  const bridgeRef = useRef<TimeChartDataBridge<T> | null>(null)
  const seriesBuffersRef = useRef<readonly AlignedSeriesData[]>([])
  const dataDirtyRef = useRef(false)
  // Auto y-range state: running min/max plus the last full-rescan timestamp.
  const autoRef = useRef({ min: Infinity, max: -Infinity, lastFull: 0 })
  const boundsRef = useRef(
    yRange.kind === 'expand' ? { lower: yRange.initialLower, upper: yRange.initialUpper }
      : yRange.kind === 'fixed' ? { lower: yRange.min, upper: yRange.max }
        : { lower: 0, upper: 1 },
  )
  const yRangeKey = yRange.kind === 'fixed'
    ? `fixed:${yRange.min}:${yRange.max}`
    : yRange.kind === 'expand'
      ? `expand:${yRange.initialLower}:${yRange.initialUpper}:${yRange.lowerPad}:${yRange.upperPad}:${yRange.expandLower !== false}`
      : `auto:${yRange.padFraction ?? 0.1}:${yRange.tickCount ?? 5}:${yRange.fixedMin ?? ''}`
  const visibilityKey = series.map(s => s.visible !== false ? '1' : '0').join('')
  const visibilityRef = useRef(series.map(s => s.visible !== false))

  const initColors = colorsFor(isDark)
  // Plugin configs live in mutable refs so theme changes update colors in place
  // (the plugins re-read cfg.current on every draw) without recreating the chart.
  const axisCfgRef = useRef<AxisConfig>({
    axisColor: initColors.axis,
    gridColor: initColors.grid,
    borderColor: initColors.border,
    font,
    xTickSpacePx: look.xTickSpacePx ?? 80,
    xTickFormat,
    yTickValues: effYTickValues,
    yTickFormat,
    xGap: look.xGap ?? 4,
    yGap: look.yGap ?? 6,
    gridDash: look.gridDash,
    showYGrid: look.showYGrid,
    xTickMark: look.xTickSize ? { color: initColors.tickMark ?? initColors.axis, size: look.xTickSize } : null,
  })
  const refCfgRef = useRef<RefLinesConfig>({
    lines: refLines ?? [],
    solidColor: zeroColorFor(isDark),
    dashedColor: refColorFor(isDark),
  })

  // Latest tooltip formatter for the imperative mousemove handler.
  const tooltipFormatRef = useRef(tooltipFormat)
  tooltipFormatRef.current = tooltipFormat

  const latestT = rows.length > 0 ? getX(rows[rows.length - 1]) : null
  const firstT = rows.length > 0 ? getX(rows[0]) : null
  const { attach, detach, wake } = useTimeChartScroll(
    true, latestT, firstT, windowSeconds, dataDirtyRef,
    { fastFrames: fastScroll, fullFps: 60, followSessionClock, minStallS: minScrollStallS }, profilerLabel,
  )

  // Static per-chart bits captured at mount (labels/colors/getY don't change).
  const seriesDefs = useRef(series)
  const getXRef = useRef(getX)

  // --- create the chart once ---
  useEffect(() => {
    const el = containerRef.current
    if (!el) return

    const defs = seriesDefs.current
    const bridge = new TimeChartDataBridge<T>(getXRef.current, defs.map((s) => s.getY))
    const plugins: Record<string, unknown> = {
      lineChart: corePlugins.lineChart,
      crosshair: corePlugins.crosshair,
      nearestPoint: corePlugins.nearestPoint,
      axis: createAxisPlugin(axisCfgRef),
    }
    if (refLines && refLines.length > 0) {
      plugins.refLines = createReferenceLinesPlugin(refCfgRef)
    }
    const fillDefs = defs.flatMap((s, i) => s.fill ? [{
      seriesIndex: i,
      color: s.fill,
      baseline: s.fillBaseline ?? 0,
      stepped: s.lineType === 1,
      stepLocation: s.stepLocation,
    }] : [])
    if (fillDefs.length > 0) plugins.areaFill = createAreaFillPlugin(fillDefs)
    if (profilerLabel) {
      plugins.profiler = createTimeChartDrawProfilerPlugin(profilerLabel)
    }

    const b = boundsRef.current
    const paddingTop = 4
    const paddingRight = look.paddingRight ?? 16
    const paddingBottom = look.xAxisSize ?? 22
    const paddingLeft = yAxisSize + (look.paddingLeftExtra ?? 4)
    const chart = new TimeChart.core(el, {
      // Padding reserves space for our axes (mirrors uPlot axis `size` + padding).
      paddingTop,
      paddingRight,
      paddingBottom,
      paddingLeft,
      // TimeChart separates its scale padding from its WebGL viewport padding.
      // Without matching render padding, lines are transformed against the
      // inset scales but can still paint across the axis/label margins.
      renderPaddingTop: paddingTop,
      renderPaddingRight: paddingRight,
      renderPaddingBottom: paddingBottom,
      renderPaddingLeft: paddingLeft,
      lineWidth: 1.5,
      yRange: { min: b.lower, max: b.upper },
      series: defs.map((s, index) => ({
        name: s.label,
        color: s.color,
        lineWidth: s.lineWidth ?? 1.5,
        lineType: s.lineType ?? TimeChart.LineType.Line,
        stepLocation: s.stepLocation ?? 1,
        visible: s.visible !== false,
        data: bridge.series[index],
      })),
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      plugins: plugins as any,
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
    } as any)
    chartRef.current = chart

    seriesBuffersRef.current = bridge.series
    bridgeRef.current = bridge

    // Crosshair uses currentColor; the nearest-point dot centre uses
    // --background-overlay. Keep both subtle and theme-aware.
    el.style.color = axisCfgRef.current.axisColor
    el.style.setProperty('--background-overlay', overlayBgFor(isDark))

    // Custom HTML tooltip: snap to the nearest sample (single index across all
    // series, matching uPlot's cursor.idx), reusing the shared tooltip element.
    let lastX = NaN
    let lastFormatter: typeof tooltipFormatRef.current | undefined
    let lastHtml = ''
    const onMove = (contentX: number, contentY: number) => {
      const bridge = bridgeRef.current
      const chart = chartRef.current
      if (!bridge || !chart) return
      if (bridge.length === 0) { hide(); return }
      const px = contentX + paddingLeft
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      const dataX = (chart.model.xScale as any).invert(px) as number
      // nearest index by binary search on the shared x buffer
      let idx = bridge.lowerBoundX(dataX)
      if (idx === bridge.length) idx--
      else if (idx > 0 && Math.abs(bridge.xAt(idx - 1) - dataX) <= Math.abs(bridge.xAt(idx) - dataX)) idx--
      const x = bridge.xAt(idx)
      const formatter = tooltipFormatRef.current
      // Pointer events frequently remain on the same telemetry sample. Only
      // rebuild tooltip arrays/HTML when that snapped sample (or formatter)
      // actually changes; positioning remains smooth every frame.
      if (x !== lastX || formatter !== lastFormatter) {
        const bufs = seriesBuffersRef.current
        const values = bufs.map((buf) => idx < buf.length ? buf.yAt(idx) : NaN)
        lastHtml = formatter(x, values)
        lastX = x
        lastFormatter = formatter
      }
      show(lastHtml, px, contentY + paddingTop, chart.clientWidth, chart.clientHeight)
    }
    const stopMove = chart.contentBoxDetector.moved.on(onMove)
    const stopLeave = chart.contentBoxDetector.left.on(hide)

    attach(chart)

    return () => {
      stopMove()
      stopLeave()
      detach()
      chart.dispose()
      chartRef.current = null
      bridgeRef.current = null
    }
    // Created once; all live updates happen through refs/other effects.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

  // Series topology stays fixed; capability changes only toggle the existing
  // GPU series in place (for example MGU-H when switching protocol format).
  useEffect(() => {
    const chart = chartRef.current
    const visibility = series.map(s => s.visible !== false)
    visibilityRef.current = visibility
    if (!chart) return
    let changed = false
    chart.options.series.forEach((s, i) => {
      if (s.visible !== visibility[i]) { s.visible = visibility[i]; changed = true }
    })
    if (changed) {
      autoRef.current = { min: Infinity, max: -Infinity, lastFull: 0 }
      dataDirtyRef.current = true
      wake()
    }
    // `visibilityKey` is the stable primitive dependency; series arrays are
    // commonly rebuilt by thin chart consumers on ordinary telemetry renders.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [visibilityKey, wake])

  // --- feed new data ---
  useEffect(() => {
    const bridge = bridgeRef.current
    const chart = chartRef.current
    if (!bridge) return
    const changed = bridge.sync(rows)
    if (changed) {
      dataDirtyRef.current = true
      wake()
    }

    // Auto y-range: fit to the visible window like uPlot's auto-range. A full
    // scan of the buffers is O(window), which at 5-/10-min windows is ~tens of
    // thousands of points *per publication* and is what made the tyre charts lag
    // (the Misc charts use fixed/expand ranges and never scan). So:
    //   - every publication, cheaply expand the range to include the newest
    //     sample — O(series) — so a rising/spiking trace never clips the frame;
    //   - only rescan the whole window on a throttle, which is what lets the
    //     axis *shrink* again once old extremes scroll off.
    if (changed && chart && yRange.kind === 'auto') {
      const a = autoRef.current
      if (rows.length > 0) {
        const last = rows[rows.length - 1]
        for (let i = 0; i < seriesDefs.current.length; i++) {
          if (!visibilityRef.current[i]) continue
          const s = seriesDefs.current[i]
          const v = s.getY(last)
          if (v < a.min) a.min = v
          if (v > a.max) a.max = v
        }
      }
      const now = performance.now()
      if (now - a.lastFull >= AUTO_RANGE_FULL_MS) {
        a.lastFull = now
        const bufs = seriesBuffersRef.current
        const b0 = bufs.find((_, i) => visibilityRef.current[i])
        if (b0 && b0.length > 0) {
          // Scan only the visible window. Binary-searching the aligned X ring
          // also keeps this correct when a caller retains a wider source span.
          const startX = b0.xAt(b0.length - 1) - windowSeconds
          const lb = b0.lowerBoundX(startX)
          let lo = Infinity, hi = -Infinity
          for (let k = 0; k < bufs.length; k++) {
            if (!visibilityRef.current[k]) continue
            const buf = bufs[k]
            for (let i = lb; i < buf.length; i++) {
              const v = buf.yAt(i)
              if (v < lo) lo = v
              if (v > hi) hi = v
            }
          }
          if (Number.isFinite(lo) && Number.isFinite(hi)) { a.min = lo; a.max = hi }
        }
      }
      if (Number.isFinite(a.min) && Number.isFinite(a.max)) {
        const pad = a.max === a.min ? Math.abs(a.max) * 0.05 + 1 : (a.max - a.min) * padFraction
        chart.options.yRange = {
          min: yRange.fixedMin ?? a.min - pad,
          max: a.max + pad,
        }
        dataDirtyRef.current = true
      }
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [rows, wake])

  // Apply Y-axis policy changes in place so a Settings toggle never recreates
  // the WebGL chart. Fixed bounds may also be data-derived (for example Fuel).
  // Expand and auto policies immediately account for the currently visible
  // buffers, so changing the policy works even while playback is paused.
  useEffect(() => {
    const chart = chartRef.current
    if (!chart) return
    // The tick generator is part of the Y-axis policy too. In particular,
    // fixed tyre ranges include their exact endpoints, while auto ranges must
    // show only rounded nice ticks; retaining the fixed generator would print
    // long, noisy dynamic min/max labels at the top and bottom of the axis.
    axisCfgRef.current = { ...axisCfgRef.current, yTickValues: effYTickValues }
    const b = boundsRef.current
    if (yRange.kind === 'fixed') {
      b.lower = yRange.min
      b.upper = yRange.max
      chart.options.yRange = { min: yRange.min, max: yRange.max }
    } else {
      const bufs = seriesBuffersRef.current
      let lo = Infinity, hi = -Infinity
      for (let k = 0; k < bufs.length; k++) {
        if (!visibilityRef.current[k]) continue
        const buf = bufs[k]
        const start = buf.length > 0 ? buf.lowerBoundX(buf.xAt(buf.length - 1) - windowSeconds) : 0
        for (let i = start; i < buf.length; i++) {
          const v = buf.yAt(i)
          if (v < lo) lo = v
          if (v > hi) hi = v
        }
      }

      if (yRange.kind === 'expand') {
        b.lower = yRange.initialLower
        b.upper = yRange.initialUpper
        if (Number.isFinite(hi) && hi > b.upper - yRange.upperPad) b.upper = Math.ceil(hi + yRange.upperPad)
        if (yRange.expandLower !== false && Number.isFinite(lo) && lo < b.lower + yRange.lowerPad) b.lower = Math.floor(lo - yRange.lowerPad)
        chart.options.yRange = { min: b.lower, max: b.upper }
      } else {
        autoRef.current = { min: lo, max: hi, lastFull: performance.now() }
        if (Number.isFinite(lo) && Number.isFinite(hi)) {
          const pad = hi === lo ? Math.abs(hi) * 0.05 + 1 : (hi - lo) * padFraction
          chart.options.yRange = { min: yRange.fixedMin ?? lo - pad, max: hi + pad }
        }
      }
    }
    dataDirtyRef.current = true
    wake()
    // `yRangeKey` contains every policy primitive; objects are deliberately not
    // dependencies because thin consumers often construct them during render.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [yRangeKey, wake])

  // --- expand-only y-range (Ride Height): only look at the newest sample and
  // push a bound outward if exceeded; never rescans, never shrinks. ---
  useEffect(() => {
    if (yRange.kind !== 'expand') return
    const chart = chartRef.current
    if (!chart || rows.length === 0) return
    const last = rows[rows.length - 1]
    const ys = seriesDefs.current.map((s) => s.getY(last))
    const maxVal = Math.max(...ys)
    const minVal = Math.min(...ys)
    const b = boundsRef.current
    let changed = false
    if (maxVal > b.upper - yRange.upperPad) { b.upper = Math.ceil(maxVal + yRange.upperPad); changed = true }
    if (yRange.expandLower !== false && minVal < b.lower + yRange.lowerPad) { b.lower = Math.floor(minVal - yRange.lowerPad); changed = true }
    if (changed) {
      chart.options.yRange = { min: b.lower, max: b.upper }
      dataDirtyRef.current = true
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [rows])

  // --- resize ---
  useEffect(() => {
    const chart = chartRef.current
    if (chart && width > 0 && height > 0) chart.onResize()
  }, [width, height])

  // --- theme: update plugin colours + cursor colours in place, then redraw ---
  useEffect(() => {
    const colors = colorsFor(isDark)
    axisCfgRef.current = {
      ...axisCfgRef.current,
      axisColor: colors.axis,
      gridColor: colors.grid,
      borderColor: colors.border,
      xTickMark: axisCfgRef.current.xTickMark
        ? { ...axisCfgRef.current.xTickMark, color: colors.tickMark ?? colors.axis }
        : axisCfgRef.current.xTickMark,
    }
    refCfgRef.current = {
      ...refCfgRef.current,
      solidColor: zeroColorFor(isDark),
      dashedColor: refColorFor(isDark),
    }
    const el = containerRef.current
    if (el) {
      el.style.color = colors.axis
      el.style.setProperty('--background-overlay', overlayBgFor(isDark))
    }
    chartRef.current?.model.requestRedraw()
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [isDark])

  // --- series colours: some charts recolour series with the theme (tyre
  // FR/RL/RR differ in light vs dark). Push new colours to the live series and
  // redraw; the WebGL renderer re-reads series.color each frame. ---
  const seriesColorKey = series.map((s) => s.color).join('|')
  useEffect(() => {
    const chart = chartRef.current
    if (!chart) return
    series.forEach((s, i) => {
      const so = chart.options.series[i]
      if (so) so.color = s.color
    })
    chart.model.requestRedraw()
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [seriesColorKey])

  return (
    <div className="absolute inset-0" ref={sizeRef}>
      <div ref={containerRef} style={{ position: 'absolute', inset: 0 }} />
      <div ref={tooltipRef} style={tooltipStyle} />
    </div>
  )
}
