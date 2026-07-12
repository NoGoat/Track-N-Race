import { useEffect, useRef } from 'react'
import { useSize } from '../../hooks/useSize'
import { useChartTooltip, TOOLTIP_STYLE } from '../../hooks/useChartTooltip'
import { useTimeChartScroll } from '../../hooks/useTimeChartScroll'
import { TimeChart, corePlugins, type TChart } from '../../lib/timechart/tc'
import { TimeChartDataBridge, type DataPoint } from '../../lib/timechart/dataBridge'
import { createAxisPlugin, type AxisConfig } from '../../lib/timechart/axisPlugin'
import { createReferenceLinesPlugin, type RefLine, type RefLinesConfig } from '../../lib/timechart/referenceLines'
import { createTimeChartDrawProfilerPlugin } from '../../hooks/useTimeChartDrawProfiler'

// Reusable WebGL chart. This is the migration target that replaces per-chart
// <UPlotReact> usage: it owns TimeChart creation/disposal, the incremental data
// bridge, the real-time scroll loop, the custom axis / reference-line / tooltip
// plugins, resize and in-place theme updates. The Misc-page leaves (G-Force /
// Ride Height) are thin consumers; the remaining uPlot charts adopt it later.
//
// The chart is created ONCE and updated imperatively (unlike uplot-react, which
// destroys+recreates on option changes) — recreating a WebGL context per theme
// toggle or resize would be wasteful and is the whole reason we moved off uPlot.

const FONT = '11px "Cascadia Code", ui-monospace, monospace'

function axisColorFor(isDark: boolean) { return isDark ? '#7c8098' : '#6b7280' }
function gridColorFor(isDark: boolean) { return isDark ? 'rgba(255,255,255,0.04)' : 'rgba(0,0,0,0.07)' }
function borderColorFor(isDark: boolean) { return isDark ? '#1e2136' : '#d0d5e0' }
function zeroColorFor(isDark: boolean) { return isDark ? 'rgba(255,255,255,0.2)' : 'rgba(0,0,0,0.18)' }
function refColorFor(isDark: boolean) { return isDark ? 'rgba(255,255,255,0.04)' : 'rgba(0,0,0,0.07)' }
function overlayBgFor(isDark: boolean) { return isDark ? '#12141f' : '#ffffff' }

export interface SeriesDef<T> {
  label: string
  color: string
  getY: (row: T) => number
  lineWidth?: number
}

export type YRangeSpec =
  | { kind: 'fixed'; min: number; max: number }
  | { kind: 'expand'; initialLower: number; initialUpper: number; lowerPad: number; upperPad: number }

export interface TimeChartViewProps<T> {
  isDark: boolean
  rows: readonly T[]
  getX: (row: T) => number
  series: SeriesDef<T>[]
  windowSeconds: number
  yRange: YRangeSpec
  /** width reserved for the y-axis labels (uPlot axis `size`). */
  yAxisSize: number
  yTickValues: (min: number, max: number) => number[]
  yTickFormat: (v: number) => string
  xTickFormat: (seconds: number) => string
  refLines?: RefLine[]
  /** builds the tooltip HTML from the cursor's x and the per-series y values. */
  tooltipFormat: (x: number, values: number[]) => string
  profilerLabel?: string
}

export default function TimeChartView<T>(props: TimeChartViewProps<T>) {
  const {
    isDark, rows, getX, series, windowSeconds, yRange, yAxisSize,
    yTickValues, yTickFormat, xTickFormat, refLines, tooltipFormat, profilerLabel,
  } = props

  const { ref: sizeRef, width, height } = useSize()
  const { tooltipRef, show, hide } = useChartTooltip()

  const containerRef = useRef<HTMLDivElement>(null)
  const chartRef = useRef<TChart | null>(null)
  const bridgeRef = useRef<TimeChartDataBridge<T> | null>(null)
  const seriesBuffersRef = useRef<DataPoint[][]>([])
  const dataDirtyRef = useRef(false)
  const boundsRef = useRef(
    yRange.kind === 'expand'
      ? { lower: yRange.initialLower, upper: yRange.initialUpper }
      : { lower: yRange.min, upper: yRange.max },
  )

  // Plugin configs live in mutable refs so theme changes update colors in place
  // (the plugins re-read cfg.current on every draw) without recreating the chart.
  const axisCfgRef = useRef<AxisConfig>({
    axisColor: axisColorFor(isDark),
    gridColor: gridColorFor(isDark),
    borderColor: borderColorFor(isDark),
    font: FONT,
    xTickSpacePx: 80,
    xTickFormat,
    yTickValues,
    yTickFormat,
    xGap: 4,
    yGap: 6,
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
  const { attach, detach } = useTimeChartScroll(
    true, latestT, firstT, windowSeconds, dataDirtyRef, undefined, profilerLabel,
  )

  // Static per-chart bits captured at mount (labels/colors/getY don't change).
  const seriesDefs = useRef(series)
  const getXRef = useRef(getX)

  // --- create the chart once ---
  useEffect(() => {
    const el = containerRef.current
    if (!el) return

    const defs = seriesDefs.current
    const plugins: Record<string, unknown> = {
      lineChart: corePlugins.lineChart,
      crosshair: corePlugins.crosshair,
      nearestPoint: corePlugins.nearestPoint,
      axis: createAxisPlugin(axisCfgRef),
    }
    if (refLines && refLines.length > 0) {
      plugins.refLines = createReferenceLinesPlugin(refCfgRef)
    }
    if (profilerLabel) {
      plugins.profiler = createTimeChartDrawProfilerPlugin(profilerLabel)
    }

    const b = boundsRef.current
    const chart = new TimeChart.core(el, {
      // Padding reserves space for our axes (mirrors uPlot axis `size` + padding).
      paddingTop: 4,
      paddingRight: 16,
      paddingBottom: 22,
      paddingLeft: yAxisSize + 4,
      lineWidth: 1.5,
      yRange: { min: b.lower, max: b.upper },
      series: defs.map((s) => ({
        name: s.label,
        color: s.color,
        lineWidth: s.lineWidth ?? 1.5,
        data: [] as DataPoint[],
      })),
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      plugins: plugins as any,
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
    } as any)
    chartRef.current = chart

    // Grab the (now DataPointsBuffer-backed) series data arrays and wire the bridge.
    const buffers = chart.options.series.map((s) => s.data as unknown as DataPoint[])
    seriesBuffersRef.current = buffers
    const bridge = new TimeChartDataBridge<T>(buffers, getXRef.current, defs.map((s) => s.getY))
    bridgeRef.current = bridge

    // Crosshair uses currentColor; the nearest-point dot centre uses
    // --background-overlay. Keep both subtle and theme-aware.
    el.style.color = axisCfgRef.current.axisColor
    el.style.setProperty('--background-overlay', overlayBgFor(isDark))

    // Custom HTML tooltip: snap to the nearest sample (single index across all
    // series, matching uPlot's cursor.idx), reusing the shared tooltip element.
    const onMove = (ev: MouseEvent) => {
      const bridge = bridgeRef.current
      const chart = chartRef.current
      if (!bridge || !chart) return
      const xs = bridge.xBuffer
      if (xs.length === 0) { hide(); return }
      const rect = el.getBoundingClientRect()
      const px = ev.clientX - rect.left
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      const dataX = (chart.model.xScale as any).invert(px) as number
      // nearest index by binary search on the shared x buffer
      let lo = 0, hi = xs.length - 1
      while (lo < hi) {
        const mid = (lo + hi) >> 1
        if (xs[mid].x < dataX) lo = mid + 1
        else hi = mid
      }
      if (lo > 0 && Math.abs(xs[lo - 1].x - dataX) <= Math.abs(xs[lo].x - dataX)) lo -= 1
      const idx = lo
      const bufs = seriesBuffersRef.current
      const values = bufs.map((buf) => (buf[idx] ? buf[idx].y : NaN))
      const xVal = xs[idx].x
      show(tooltipFormatRef.current(xVal, values), px, ev.clientY - rect.top, el.clientWidth, el.clientHeight)
    }
    const detector = chart.contentBoxDetector.node
    detector.addEventListener('mousemove', onMove)
    detector.addEventListener('mouseleave', hide)

    attach(chart)

    return () => {
      detector.removeEventListener('mousemove', onMove)
      detector.removeEventListener('mouseleave', hide)
      detach()
      chart.dispose()
      chartRef.current = null
      bridgeRef.current = null
    }
    // Created once; all live updates happen through refs/other effects.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

  // --- feed new data ---
  useEffect(() => {
    const bridge = bridgeRef.current
    if (!bridge) return
    if (bridge.sync(rows)) dataDirtyRef.current = true
  }, [rows])

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
    if (minVal < b.lower + yRange.lowerPad) { b.lower = Math.floor(minVal - yRange.lowerPad); changed = true }
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

  // --- theme: update plugin colors + cursor colors in place, then redraw ---
  useEffect(() => {
    axisCfgRef.current = {
      ...axisCfgRef.current,
      axisColor: axisColorFor(isDark),
      gridColor: gridColorFor(isDark),
      borderColor: borderColorFor(isDark),
    }
    refCfgRef.current = {
      ...refCfgRef.current,
      solidColor: zeroColorFor(isDark),
      dashedColor: refColorFor(isDark),
    }
    const el = containerRef.current
    if (el) {
      el.style.color = axisCfgRef.current.axisColor
      el.style.setProperty('--background-overlay', overlayBgFor(isDark))
    }
    chartRef.current?.model.update()
  }, [isDark])

  return (
    <div className="absolute inset-0" ref={sizeRef}>
      <div ref={containerRef} style={{ position: 'absolute', inset: 0 }} />
      <div ref={tooltipRef} style={TOOLTIP_STYLE} />
    </div>
  )
}
