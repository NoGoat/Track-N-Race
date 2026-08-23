import { useEffect, useRef } from 'react'
import { useSize } from '../../hooks/useSize'
import { useChartTooltip, TOOLTIP_STYLE } from '../../hooks/useChartTooltip'
import { useTimeChartScroll } from '../../hooks/useTimeChartScroll'
import { TimeChart, corePlugins, type TChart } from '../../lib/timechart/tc'
import { TimeChartDataBridge } from '../../lib/timechart/dataBridge'
import type { AlignedSeriesData } from '../../lib/timechart/engine/core/alignedData'
import { createAxisPlugin, type AxisConfig } from '../../lib/timechart/axisPlugin'
import { createReferenceLinesPlugin, type RefLine, type RefLinesConfig } from '../../lib/timechart/referenceLines'
import { niceTicks } from '../../lib/timechart/ticks'
import { createAreaFillPlugin } from '../../lib/timechart/areaFill'
import type { CSSProperties } from 'react'
import { useChartCoordinates } from '../../lib/chartCoordinates'
import { formatChartDeltaTooltip } from '../../lib/chartDeltaTooltip'
import { playbackDebug } from '../../lib/playbackDebug'
import { scheduleCooperativeTask } from '../../lib/cooperativeTask'
import { subscribeAllLapsData } from '../../stores/telemetryStore'
import { themeSeriesColor } from '../../lib/themeColors'

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
function overlayBgFor(isDark: boolean) { return isDark ? '#12141f' : '#f1f0ec' }

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
    axis: isDark ? '#7c8098' : '#596168',
    grid: isDark ? 'rgba(255,255,255,0.04)' : 'rgba(0,0,0,0.07)',
    border: isDark ? '#1e2136' : '#afb1ae',
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

interface XIndexedData {
  readonly length: number
  xAt(index: number): number
  lowerBoundX(x: number): number
}

function nearestIndex(source: XIndexedData | null, targetX: number): number {
  if (!source || source.length === 0) return -1
  let index = source.lowerBoundX(targetX)
  if (index === source.length) index--
  else if (index > 0 && Math.abs(source.xAt(index - 1) - targetX) <= Math.abs(source.xAt(index) - targetX)) index--
  return index
}

function lowerBoundSessionTime(rows: readonly { session_time: number }[], value: number): number {
  let lo = 0, hi = rows.length
  while (lo < hi) {
    const mid = (lo + hi) >> 1
    if (rows[mid].session_time < value) lo = mid + 1
    else hi = mid
  }
  return lo
}

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

export interface TimeChartViewProps<T extends { session_time: number }> {
  isDark: boolean
  rows: readonly T[]
  /** Optional completed-lap trace drawn underneath the live rows. */
  comparisonRows?: readonly T[]
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
  /** Builds tooltip HTML from the cursor x, current values, and optional comparison-lap values. */
  tooltipFormat: (x: number, values: number[], comparisonValues?: number[]) => string
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

export default function TimeChartView<T extends { session_time: number }>(props: TimeChartViewProps<T>) {
  const {
    isDark, rows, comparisonRows, getX, series, windowSeconds, yRange, yAxisSize,
    yTickValues, yTickFormat, xTickFormat, refLines, tooltipFormat,
    colorsFor = defaultColors, axisLook, tooltipStyle = TOOLTIP_STYLE, fastScroll,
    followSessionClock, minScrollStallS,
  } = props

  const look = axisLook ?? {}
  const coordinates = useChartCoordinates()
  const effectiveGetX = coordinates.distanceMode ? coordinates.getX : getX
  const effectiveWindow = coordinates.distanceMode ? Math.max(coordinates.trackLengthM, 1) : windowSeconds
  const effectiveXTickFormat = coordinates.distanceMode || coordinates.allLapsMode ? coordinates.formatX : xTickFormat
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
  const comparisonBridgeRef = useRef<TimeChartDataBridge<T> | null>(null)
  const comparisonLapRef = useRef<number | null>(null)
  const lapRevisionRef = useRef(coordinates.lapRevision)
  const historyRevisionRef = useRef(coordinates.historyRevision)
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
  const debugChartName = series.map(s => s.label).join('+')

  const initColors = colorsFor(isDark)
  // Plugin configs live in mutable refs so theme changes update colors in place
  // (the plugins re-read cfg.current on every draw) without recreating the chart.
  const axisCfgRef = useRef<AxisConfig>({
    axisColor: initColors.axis,
    gridColor: initColors.grid,
    borderColor: initColors.border,
    font,
    xTickSpacePx: look.xTickSpacePx ?? 80,
    xTickFormat: effectiveXTickFormat,
    xTickValues: coordinates.xTickValues,
    xTickAnchor: coordinates.allLapsMode ? 'start' : 'middle',
    xLabelOffset: coordinates.allLapsMode ? 4 : 0,
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
  const deltaRevisionRef = useRef('')
  deltaRevisionRef.current = coordinates.comparisonMode
    ? `${coordinates.progressRevision}:${coordinates.lapData?.lapNum ?? ''}`
    : ''

  const historyStartIndex = coordinates.stintLapsMode
    ? lowerBoundSessionTime(rows, coordinates.historyStartTime)
    : 0
  const latestT = rows.length > historyStartIndex ? effectiveGetX(rows[rows.length - 1]) : null
  const firstT = rows.length > historyStartIndex ? effectiveGetX(rows[historyStartIndex]) : null
  const { attach, detach, wake, acceptDataRange } = useTimeChartScroll(
    !coordinates.distanceMode, latestT, firstT, effectiveWindow, dataDirtyRef,
    { fastFrames: fastScroll || coordinates.allLapsMode, fullFps: coordinates.allLapsMode ? 12 : 60, followSessionClock, minStallS: minScrollStallS, accumulateFromStart: coordinates.allLapsMode },
  )

  // Static per-chart bits captured at mount (labels/colors/getY don't change).
  const seriesDefs = useRef(series)
  const getXRef = useRef(effectiveGetX)
  getXRef.current = effectiveGetX

  const blendColor = (hex: string): string => {
    const match = /^#([0-9a-f]{6})$/i.exec(hex)
    if (!match) return hex
    const value = Number.parseInt(match[1], 16)
    const bg = isDark ? [0x12, 0x14, 0x1f] : [0xeb, 0xea, 0xe6]
    const rgb = [(value >> 16) & 255, (value >> 8) & 255, value & 255]
    return `#${rgb.map((channel, i) => Math.round(channel * 0.35 + bg[i] * 0.65).toString(16).padStart(2, '0')).join('')}`
  }

  // --- create the chart once ---
  useEffect(() => {
    const el = containerRef.current
    if (!el) return

    const defs = seriesDefs.current
    const bridge = new TimeChartDataBridge<T>(row => getXRef.current(row), defs.map((s) => s.getY))
    const comparisonBridge = new TimeChartDataBridge<T>(row => coordinates.getComparisonX(row), defs.map((s) => s.getY))
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
      seriesIndex: i + defs.length,
      color: s.fill,
      baseline: s.fillBaseline ?? 0,
      stepped: s.lineType === 1,
      stepLocation: s.stepLocation,
    }] : [])
    if (fillDefs.length > 0) plugins.areaFill = createAreaFillPlugin(fillDefs)
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
      series: [
        ...defs.map((s, index) => ({
          name: `comparison:${s.label}`,
          color: blendColor(themeSeriesColor(s.color, isDark)),
          lineWidth: Math.max(1, (s.lineWidth ?? 1.5) - 0.5),
          lineType: s.lineType ?? TimeChart.LineType.Line,
          stepLocation: s.stepLocation ?? 1,
          visible: comparisonRows != null && s.visible !== false,
          data: comparisonBridge.series[index],
        })),
        ...defs.map((s, index) => ({
        name: s.label,
        color: themeSeriesColor(s.color, isDark),
        lineWidth: s.lineWidth ?? 1.5,
        lineType: s.lineType ?? TimeChart.LineType.Line,
        stepLocation: s.stepLocation ?? 1,
        visible: s.visible !== false,
        data: bridge.series[index],
        })),
      ],
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      plugins: plugins as any,
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
    } as any)
    chartRef.current = chart

    seriesBuffersRef.current = bridge.series
    bridgeRef.current = bridge
    comparisonBridgeRef.current = comparisonBridge

    // Crosshair uses currentColor; the nearest-point dot centre uses
    // --background-overlay. Keep both subtle and theme-aware.
    el.style.color = axisCfgRef.current.axisColor
    el.style.setProperty('--background-overlay', overlayBgFor(isDark))

    // Custom HTML tooltip: sample each lap independently at the cursor. The
    // current lap retains its endpoint fallback while the completed comparison
    // lap follows the hovered distance.
    let lastCurrentX = NaN
    let lastComparisonX = NaN
    let lastFormatter: typeof tooltipFormatRef.current | undefined
    let lastDeltaRevision = ''
    let lastHtml = ''
    const onMove = (contentX: number, contentY: number) => {
      const bridge = bridgeRef.current
      const chart = chartRef.current
      if (!bridge || !chart) return
      if (bridge.length === 0) { hide(); return }
      const comparisonBridge = comparisonBridgeRef.current
      const px = contentX + paddingLeft
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      const dataX = (chart.model.xScale as any).invert(px) as number
      const idx = nearestIndex(bridge, dataX)
      const comparisonIdx = nearestIndex(comparisonBridge, dataX)
      const currentX = bridge.xAt(idx)
      const comparisonX = comparisonIdx >= 0 ? comparisonBridge!.xAt(comparisonIdx) : NaN
      const x = comparisonIdx >= 0 ? comparisonX : currentX
      const formatter = tooltipFormatRef.current
      const deltaRevision = deltaRevisionRef.current
      // Pointer events frequently remain on the same telemetry sample. Only
      // rebuild tooltip arrays/HTML when that snapped sample (or formatter)
      // actually changes; positioning remains smooth every frame.
      if (currentX !== lastCurrentX || comparisonX !== lastComparisonX || formatter !== lastFormatter || deltaRevision !== lastDeltaRevision) {
        const bufs = seriesBuffersRef.current
        const values = bufs.map((buf) => idx < buf.length ? buf.yAt(idx) : NaN)
        let comparisonValues: number[] | undefined
        if (comparisonBridge && comparisonIdx >= 0) {
          comparisonValues = comparisonBridge.series.map((buf) => comparisonIdx < buf.length ? buf.yAt(comparisonIdx) : NaN)
        }
        lastHtml = formatter(x, values, comparisonValues) + formatChartDeltaTooltip(coordinates.getDeltaAtDistance(x))
        lastCurrentX = currentX
        lastComparisonX = comparisonX
        lastFormatter = formatter
        lastDeltaRevision = deltaRevision
      }
      show(lastHtml, px, contentY + paddingTop, chart.clientWidth, chart.clientHeight)
    }
    const stopTooltipSync = chart.nearestPoint.updated.on(() => {
      const pointer = chart.nearestPoint.lastPointerPos
      if (!pointer) { hide(); return }
      onMove(pointer.x - paddingLeft, pointer.y - paddingTop)
    })

    attach(chart)

    return () => {
      stopTooltipSync()
      detach()
      chart.dispose()
      chartRef.current = null
      bridgeRef.current = null
      comparisonBridgeRef.current = null
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
    const count = series.length
    chart.options.series.forEach((s, i) => {
      const visible = visibility[i % count] && (i >= count || comparisonRows != null)
      if (s.visible !== visible) { s.visible = visible; changed = true }
    })
    if (changed) {
      autoRef.current = { min: Infinity, max: -Infinity, lastFull: 0 }
      dataDirtyRef.current = true
      wake()
    }
    // `visibilityKey` is the stable primitive dependency; series arrays are
    // commonly rebuilt by thin chart consumers on ordinary telemetry renders.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [comparisonRows, visibilityKey, wake])

  useEffect(() => {
    const bridge = comparisonBridgeRef.current
    const chart = chartRef.current
    if (!bridge) return
    if (!comparisonRows || !coordinates.comparisonMode) {
      if (bridge.clear()) chart?.model.requestRedraw()
      comparisonLapRef.current = null
      return
    }
    const comparisonLap = coordinates.lapData?.lapNum ?? null
    if (comparisonLapRef.current !== comparisonLap) {
      playbackDebug('chart-comparison-lap-change', {
        chart: debugChartName,
        previousLap: comparisonLapRef.current,
        comparisonLap,
        comparisonRows: comparisonRows.length,
        bufferRowsBeforeClear: bridge.length,
      })
      bridge.clear()
      comparisonLapRef.current = comparisonLap
    }
    let syncEnd = comparisonRows.length
    let lo = 0, hi = comparisonRows.length
    while (lo < hi) {
      const mid = (lo + hi) >> 1
      if (Number.isFinite(coordinates.getComparisonX(comparisonRows[mid]))) lo = mid + 1
      else hi = mid
    }
    syncEnd = lo
    const { changed } = bridge.sync(comparisonRows, syncEnd)
    if (changed) chart?.model.requestRedraw()
  }, [comparisonRows, coordinates])

  // --- feed new data ---
  const syncRowsRef = useRef<() => void>(() => {})
  const syncTaskPendingRef = useRef(false)
  const scheduleRowsSync = (): void => {
    if (syncTaskPendingRef.current) return
    syncTaskPendingRef.current = true
    scheduleCooperativeTask(() => {
      syncTaskPendingRef.current = false
      syncRowsRef.current()
    })
  }
  syncRowsRef.current = () => {
    const bridge = bridgeRef.current
    const chart = chartRef.current
    if (!bridge) return
    const lapRevisionChanged = coordinates.distanceMode && lapRevisionRef.current !== coordinates.lapRevision
    const historyRevisionChanged = historyRevisionRef.current !== coordinates.historyRevision
    if (lapRevisionChanged || historyRevisionChanged) {
      playbackDebug('chart-lap-revision', {
        chart: debugChartName,
        previousRevision: lapRevisionRef.current,
        revision: coordinates.lapRevision,
        mode: coordinates.mode,
        rows: rows.length,
        firstSessionTime: rows[0]?.session_time ?? null,
        lastSessionTime: rows[rows.length - 1]?.session_time ?? null,
        firstX: rows.length ? effectiveGetX(rows[0]) : null,
        lastX: rows.length ? effectiveGetX(rows[rows.length - 1]) : null,
        bufferRowsBeforeClear: bridge.length,
      })
      bridge.clear()
      lapRevisionRef.current = coordinates.lapRevision
      historyRevisionRef.current = coordinates.historyRevision
      autoRef.current = { min: Infinity, max: -Infinity, lastFull: 0 }
    }
    let syncEnd = rows.length
    if (coordinates.distanceMode) {
      let lo = 0, hi = rows.length
      while (lo < hi) {
        const mid = (lo + hi) >> 1
        if (Number.isFinite(effectiveGetX(rows[mid]))) lo = mid + 1
        else hi = mid
      }
      syncEnd = lo
    }
    const syncStart = coordinates.stintLapsMode
      ? lowerBoundSessionTime(rows, coordinates.historyStartTime)
      : 0
    const { changed, syncedFrom } = bridge.sync(rows, syncEnd, syncStart)
    if (lapRevisionChanged) {
      playbackDebug('chart-lap-revision-synced', {
        chart: debugChartName,
        revision: coordinates.lapRevision,
        inputRows: rows.length,
        syncEnd,
        syncedFrom,
        changed,
        bufferRows: bridge.length,
        bufferFirstX: bridge.length ? bridge.xAt(0) : null,
        bufferLastX: bridge.length ? bridge.xAt(bridge.length - 1) : null,
      })
    }
    if (changed) {
      if (syncEnd > syncStart) acceptDataRange(effectiveGetX(rows[syncEnd - 1]), effectiveGetX(rows[syncStart]))
      dataDirtyRef.current = true
      wake()
    }
    if (changed && chart && coordinates.distanceMode) {
      const max = coordinates.trackLengthM > 0 ? coordinates.trackLengthM : (bridge.length ? bridge.xAt(bridge.length - 1) : 1)
      chart.options.xRange = { min: 0, max: Math.max(max, 1) }
      chart.model.requestRedraw()
      dataDirtyRef.current = false
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
      if (coordinates.allLapsMode && syncedFrom !== null) {
        // Full-lap modes never evict a visible prefix within their active
        // range, so the range only needs to expand between stint resets.
        // Scan exactly the newly synchronized chunk (or the complete buffer
        // after a backfill rebuild) instead of rescanning the whole race.
        for (let rowIndex = syncedFrom; rowIndex < syncEnd; rowIndex++) {
          const row = rows[rowIndex]
          for (let i = 0; i < seriesDefs.current.length; i++) {
            if (!visibilityRef.current[i]) continue
            const v = seriesDefs.current[i].getY(row)
            if (v < a.min) a.min = v
            if (v > a.max) a.max = v
          }
        }
      } else if (syncEnd > 0) {
        const last = rows[syncEnd - 1]
        for (let i = 0; i < seriesDefs.current.length; i++) {
          if (!visibilityRef.current[i]) continue
          const s = seriesDefs.current[i]
          const v = s.getY(last)
          if (v < a.min) a.min = v
          if (v > a.max) a.max = v
        }
      }
      const now = performance.now()
      if (!coordinates.allLapsMode && now - a.lastFull >= AUTO_RANGE_FULL_MS) {
        a.lastFull = now
        const currentBuffers = seriesBuffersRef.current
        const comparisonBuffers = coordinates.comparisonMode ? comparisonBridgeRef.current?.series ?? [] : []
        const visibleBuffers = [...currentBuffers, ...comparisonBuffers]
        const b0 = visibleBuffers.find((_, i) => visibilityRef.current[i % currentBuffers.length])
        if (b0 && b0.length > 0) {
          let lo = Infinity, hi = -Infinity
          for (let k = 0; k < visibleBuffers.length; k++) {
            if (!visibilityRef.current[k % currentBuffers.length]) continue
            const buf = visibleBuffers[k]
            const startX = coordinates.distanceMode || coordinates.allLapsMode ? buf.xAt(0) : buf.xAt(buf.length - 1) - windowSeconds
            const lb = buf.lowerBoundX(startX)
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

    // Expanding ranges normally inspect only newly synchronized source rows.
    // This remains O(new data) during live use, while also covering playback
    // seeks that append a whole batch whose peak may be in the middle rather
    // than at the newest sample.
    if (changed && chart && yRange.kind === 'expand' && syncedFrom !== null) {
      let minVal = Infinity
      let maxVal = -Infinity
      for (let rowIndex = syncedFrom; rowIndex < syncEnd; rowIndex++) {
        const row = rows[rowIndex]
        for (let seriesIndex = 0; seriesIndex < seriesDefs.current.length; seriesIndex++) {
          if (!visibilityRef.current[seriesIndex]) continue
          const value = seriesDefs.current[seriesIndex].getY(row)
          if (value < minVal) minVal = value
          if (value > maxVal) maxVal = value
        }
      }
      const b = boundsRef.current
      let rangeChanged = false
      if (maxVal > b.upper - yRange.upperPad) { b.upper = Math.ceil(maxVal + yRange.upperPad); rangeChanged = true }
      if (yRange.expandLower !== false && minVal < b.lower + yRange.lowerPad) { b.lower = Math.floor(minVal - yRange.lowerPad); rangeChanged = true }
      if (rangeChanged) {
        chart.options.yRange = { min: b.lower, max: b.upper }
        dataDirtyRef.current = true
      }
    }
  }

  useEffect(() => {
    scheduleRowsSync()
  }, [rows, wake, coordinates.allLapsMode, coordinates.distanceMode, coordinates.historyRevision, coordinates.historyStartTime, coordinates.lapRevision, coordinates.progressRevision, coordinates.stintLapsMode, coordinates.trackLengthM])

  useEffect(() => {
    if (!coordinates.allLapsMode) return
    return subscribeAllLapsData(scheduleRowsSync)
  }, [coordinates.allLapsMode])

  useEffect(() => {
    axisCfgRef.current = {
      ...axisCfgRef.current,
      xTickFormat: effectiveXTickFormat,
      xTickValues: coordinates.xTickValues,
      xTickAnchor: coordinates.allLapsMode ? 'start' : 'middle',
      xLabelOffset: coordinates.allLapsMode ? 4 : 0,
    }
    chartRef.current?.model.requestRedraw()
  }, [coordinates.allLapsMode, coordinates.axisRevision, coordinates.xTickValues, effectiveXTickFormat])

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
        const start = buf.length > 0
          ? coordinates.allLapsMode ? 0 : buf.lowerBoundX(buf.xAt(buf.length - 1) - windowSeconds)
          : 0
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
    const count = series.length
    series.forEach((s, i) => {
      const comparison = chart.options.series[i]
      const current = chart.options.series[i + count]
      const color = themeSeriesColor(s.color, isDark)
      if (comparison) comparison.color = blendColor(color)
      if (current) current.color = color
    })
    chart.model.requestRedraw()
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [isDark, seriesColorKey])

  return (
    <div className="absolute inset-0" ref={sizeRef}>
      <div ref={containerRef} style={{ position: 'absolute', inset: 0 }} />
      <div ref={tooltipRef} style={tooltipStyle} />
    </div>
  )
}
