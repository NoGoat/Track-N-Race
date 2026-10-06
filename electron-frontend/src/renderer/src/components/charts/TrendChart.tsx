import { useCallback, useMemo, useState } from 'react'
import TimeChartView, { type AxisLook, type SeriesDef, type YRangeSpec } from './TimeChartView'
import { viewOfRows, sessionTimeAt } from '../../lib/columnStore'
import { LocalChartCoordinatesProvider } from '../../lib/chartCoordinates'
import { TYRE_CHART_Y_AXIS_SIZE } from '../../lib/tyreChartLayout'
import { TYRE_AXIS_LOOK, TYRE_TOOLTIP_STYLE, tyreColorsFor } from '../TyreTrendCharts'
import { themeSeriesColor } from '../../lib/themeColors'
import { niceTicks } from '../../lib/timechart/ticks'

// One tick per lap; the axis plugin thins the labels when they would collide.
function lapTicks(min: number, max: number): number[] {
  const ticks: number[] = []
  for (let lap = Math.ceil(min); lap <= Math.floor(max); lap++) ticks.push(lap)
  return ticks
}
// Round values only: a range end such as max + 0.5 MJ is not worth a label.
const roundYTicks = (min: number, max: number) => niceTicks(min, max, 5)

// Several scales share one plot the way the Overview speed/RPM/ERS chart does:
// every series is drawn normalized to 0-1, and each axis labels the same
// quarter lines in its own units, so all labels sit on the grid.
const AXIS_TICKS = [0, 0.25, 0.5, 0.75, 1]
const axisTicks = () => AXIS_TICKS
// Room for one extra right-hand label column such as "8.0 MJ" or "100%".
const EXTRA_AXIS_WIDTH = 42
const NICE_STEPS = [1, 2, 2.5, 5]

export interface TrendAxis {
  min: number
  max: number
  format: (value: number) => string
  formatTooltip?: (value: number) => string
  /** Label colour; the neutral axis colour when omitted. */
  color?: string
}

/**
 * The narrowest range covering [lo, hi] whose quarter lines are round values.
 * `floor` pins the bottom of the range, such as 0 MJ or 0% wear.
 */
export function quarterAxisRange(lo: number, hi: number, floor?: number): { min: number; max: number } {
  if (!Number.isFinite(lo) || !Number.isFinite(hi)) return { min: floor ?? 0, max: (floor ?? 0) + 1 }
  if (floor !== undefined) lo = floor
  const span = Math.max(hi - lo, 1e-6)
  const magnitude = 10 ** Math.floor(Math.log10(span / 4))
  for (const scale of [1, 10, 100]) for (const nice of NICE_STEPS) {
    const step = nice * magnitude * scale
    const min = floor ?? Math.floor(lo / step) * step
    if (min + 4 * step >= hi) return { min, max: min + 4 * step }
  }
  return { min: lo, max: hi }
}

export interface TrendChartPoint {
  x: number
  values: Array<number | null | undefined>
  label: string
  invalid?: boolean
  fastest?: boolean
  // The lap in progress: its value is still rising.
  provisional?: boolean
  // Session time the lap began, which places it on session-time charts.
  lapStart?: number
  // The lap a sample was taken in, when `x` falls inside it.
  lap?: number
}
interface Props {
  title: string
  points: TrendChartPoint[]
  // Listed top to bottom: the first series is drawn above the rest, and the
  // legend and tooltip follow the same order. `axis` indexes `axes`; it is
  // ignored without them. `perLap` series read `lapPoints`, the rest `points`;
  // each reads value `channel`, by default its position within its own group.
  // In bars, series sharing a `stack` share one bar, stacked bottom up in the
  // order listed.
  series: Array<{ label: string; color: string; visible?: boolean; axis?: number; perLap?: boolean; channel?: number; stack?: string }>
  /**
   * One point per completed lap at x = lap number, the line closing it, drawn
   * above `points` and joined directly. Hovering inside lap N shows lap N's.
   */
  lapPoints?: TrendChartPoint[]
  /** One filled bar per series for each point, side by side around its x. */
  bars?: boolean
  isDark: boolean
  discrete?: boolean
  fixedRange?: [number, number]
  zeroBaselineHeadroom?: number
  formatX: (value: number) => string
  formatY: (value: number) => string
  formatTooltipY?: (value: number) => string
  /** Joins the page's shared tooltip, matched to time charts by lap start. */
  cursorSync?: { id: string; order: number }
  /** Independent value scales: the first on the left, the rest on the right. */
  axes?: TrendAxis[]
}
type StintRow = { session_time: number; [key: string]: number }

// Diameter of the dot marking each lap's value on a per-lap line.
const LAP_DOT_SIZE = 8

const BAR_GROUP_WIDTH = 0.8
const BAR_GAP_FRACTION = 0.15
// Corners of one bar share an x; the data bridge needs x to increase.
const BAR_EDGE = 1e-4

/**
 * Bar outlines as line rows: for each point, one bar per slot, side by side
 * across BAR_GROUP_WIDTH of a lap. Each bar is a rectangle traced from the
 * baseline; every channel outside the slot sits on the baseline meanwhile, so
 * each series fills only its own bars. A slot of several channels is a stack:
 * each channel's rectangle reaches the running total up to it, and the series
 * are drawn tallest first, so each shows as its own segment. Values are
 * plotted on their own axes, all from 0.
 */
function barRowsOf(points: readonly TrendChartPoint[], slots: readonly (readonly number[])[]) {
  const rows: StintRow[] = []
  const width = BAR_GROUP_WIDTH / Math.max(slots.length, 1)
  const gap = width * BAR_GAP_FRACTION / 2
  const baseline = (x: number) => {
    const row: StintRow = { session_time: x }
    for (const slot of slots) for (const channel of slot) row[`value_${channel}`] = 0
    return row
  }
  for (const point of points) {
    slots.forEach((slot, k) => {
      const left = point.x - BAR_GROUP_WIDTH / 2 + k * width + gap
      const right = left + width - 2 * gap
      const totals: Record<string, number> = {}
      let total = 0
      for (const channel of slot) {
        const value = point.values[channel]
        total += value != null && Number.isFinite(value) ? value : 0
        totals[`value_${channel}`] = total
      }
      const top = (x: number) => ({ ...baseline(x), ...totals })
      rows.push(baseline(left), top(left + BAR_EDGE), top(right - BAR_EDGE), baseline(right))
    })
  }
  return viewOfRows<StintRow>('stint', rows, false)
}

function rowsOf(points: readonly TrendChartPoint[]) {
  return viewOfRows<StintRow>('stint', points.map(point => {
    const row: StintRow = { session_time: point.x }
    point.values.forEach((value, channel) => { if (value != null) row[`value_${channel}`] = value })
    if (point.lapStart !== undefined) row.lap_start = point.lapStart
    return row
  }), false)
}

// Adapt the selected-driver snapshot to the same WebGL renderer used by Tyres.
// Rendering, cursor points, tooltip placement and interaction stay shared.
export default function TrendChart({ title, points, series, isDark, discrete, fixedRange, zeroBaselineHeadroom, formatX, formatY, formatTooltipY = formatY, cursorSync, axes, lapPoints, bars }: Props) {
  // Rebuild only if history before the newest point actually changed.
  // Appends and newest-point patches use the shared incremental GPU bridge.
  // The previous points are state updated during render, as React documents
  // for information from previous renders; React re-runs this before children.
  const [previous, setPrevious] = useState({ points, revision: 0 })
  let revision = previous.revision
  if (points !== previous.points) {
    const old = previous.points
    if (points.length < old.length || old.slice(0, -1).some((point, i) =>
      point.x !== points[i]?.x || point.values.some((value, channel) => !Object.is(value, points[i]?.values[channel])))) revision++
    setPrevious({ points, revision })
  }
  const lapRows = useMemo(() => lapPoints && rowsOf(lapPoints), [lapPoints])
  // Each series' value channel: its position within its own group.
  const channels = useMemo(() => {
    let main = 0, perLap = 0
    return series.map(item => { const position = item.perLap ? perLap++ : main++; return item.channel ?? position })
  }, [series])
  // Bars draw their outlines, so their rows are the bar corners, not the points.
  // Bar slots in listed order; a series joins the slot of its stack.
  const barSlots = useMemo(() => {
    const slots: number[][] = []
    const stacks = new Map<string, number[]>()
    series.forEach((item, index) => {
      if (item.perLap) return
      const existing = item.stack !== undefined ? stacks.get(item.stack) : undefined
      if (existing) { existing.push(channels[index]); return }
      const slot = [channels[index]]
      slots.push(slot)
      if (item.stack !== undefined) stacks.set(item.stack, slot)
    })
    return slots
  }, [channels, series])
  const rows = useMemo(() => bars ? barRowsOf(points, barSlots) : rowsOf(points), [bars, points, barSlots])
  // Reversed so the first listed is drawn last, on top; a stack's bottom
  // segment, listed first, is its shortest rectangle and so stays visible.
  const seriesDefs = useCallback((perLap: boolean) => series.flatMap((item, index): SeriesDef<StintRow>[] => {
    if (!!item.perLap !== perLap) return []
    const axis = axes?.[item.axis ?? 0]
    const key = `value_${channels[index]}`
    const getY: SeriesDef<StintRow>['getY'] = axis
      ? (source, i) => (source.num(key, i) - axis.min) / (axis.max - axis.min)
      : (source, i) => source.num(key, i)
    // Per-lap lines carry a dot at each lap's value, drawn above the line.
    const dots: SeriesDef<StintRow>[] = perLap
      ? [{ label: `${item.label} dots`, color: item.color, visible: item.visible, lineType: 3, lineWidth: LAP_DOT_SIZE, nearestSnap: 'none', getY }]
      : []
    return [...dots, {
      ...item,
      // A bar is read from its tooltip; a marker on a corner would mislead.
      nearestSnap: bars ? 'none' : perLap ? 'next' : undefined,
      fill: bars ? item.color : undefined,
      fillBaseline: 0,
      getY,
    }]
  }).reverse(), [series, axes, channels, bars])
  const chartSeries = useMemo(() => seriesDefs(false), [seriesDefs])
  const overlay = useMemo(() => lapRows && { rows: lapRows, series: seriesDefs(true) }, [lapRows, seriesDefs])
  const yRange = useMemo<YRangeSpec>(() => {
    if (axes) return { kind: 'fixed', min: 0, max: 1 }
    if (fixedRange) return { kind: 'fixed', min: fixedRange[0], max: fixedRange[1] }
    let min = Infinity, max = -Infinity
    for (const point of points) for (const value of point.values)
      if (value != null && Number.isFinite(value)) { min = Math.min(min, value); max = Math.max(max, value) }
    if (zeroBaselineHeadroom != null) return { kind: 'fixed', min: 0, max: Math.max(max, 0) + zeroBaselineHeadroom }
    // Always derived from the points: auto ranging a single lap has no height
    // and flips between a degenerate and a padded axis.
    if (!Number.isFinite(min)) return { kind: 'fixed', min: 0, max: 1 }
    const pad = max > min ? (max - min) * 0.1 : 1
    return { kind: 'fixed', min: min - pad, max: max + pad }
  }, [axes, fixedRange, points, zeroBaselineHeadroom])
  const axisLabel = useCallback((axis: TrendAxis) => (value: number) =>
    axis.format(axis.min + value * (axis.max - axis.min)), [])
  const yTickFormat = useMemo(() => axes ? axisLabel(axes[0]) : formatY, [axes, axisLabel, formatY])
  const extraYAxes = useMemo(() => axes?.slice(1).map((axis, i) => ({
    side: 'right' as const, offset: 4 + i * EXTRA_AXIS_WIDTH, color: axis.color ?? tyreColorsFor(isDark).axis,
    values: AXIS_TICKS, format: axisLabel(axis),
  })), [axes, axisLabel, isDark])
  const axisLook = useMemo<AxisLook>(() => extraYAxes?.length
    ? { ...TYRE_AXIS_LOOK, paddingRight: 4 + extraYAxes.length * EXTRA_AXIS_WIDTH }
    : TYRE_AXIS_LOOK, [extraYAxes])
  // The tick format and the series are read when the chart is created, so a
  // new scale, or a series added, removed or reordered, remounts it.
  const axesKey = axes?.map(axis => `${axis.min}:${axis.max}`).join('|') ?? ''
  const seriesKey = `${bars ? 'bars' : 'lines'}|` + series.map((item, index) => `${item.label}:${item.perLap ? 1 : 0}:${channels[index]}:${item.axis ?? 0}:${item.stack ?? ''}`).join('|')
  const first = Math.min(points[0]?.x ?? Infinity, lapPoints?.[0]?.x ?? Infinity)
  const last = Math.max(points.at(-1)?.x ?? -Infinity, lapPoints?.at(-1)?.x ?? -Infinity)
  const xRange = useMemo(() => {
    if (!Number.isFinite(first)) return { min: 0, max: 1 }
    return { min: discrete ? first - 0.5 : first, max: discrete ? last + 0.5 : Math.max(last, first + 1) }
  }, [discrete, first, last])
  const pointLabel = useCallback((point: TrendChartPoint) =>
    `${point.label}${point.invalid ? ' · Invalid' : ''}${point.fastest ? ' · Fastest' : ''}${point.provisional ? ' · In progress' : ''}`, [])
  const formatValues = useCallback((values: ArrayLike<number | null | undefined>) => {
    let html = ''
    series.forEach((item, i) => {
      if (item.visible === false) return
      const value = values[channels[i]]
      const axis = axes?.[item.axis ?? 0]
      const format = axis ? axis.formatTooltip ?? axis.format : formatTooltipY
      html += `<div><span style="color:${themeSeriesColor(item.color, isDark)}">${item.label}</span>: ${value != null && Number.isFinite(value) ? format(value) : '—'}</div>`
    })
    return html
  }, [series, channels, axes, isDark, formatTooltipY])
  const tooltipFormat = useCallback((x: number) => {
    let lo = 0, hi = points.length
    while (lo < hi) { const mid = (lo + hi) >> 1; if (points[mid].x < x) lo = mid + 1; else hi = mid }
    let index = Math.min(lo, points.length - 1)
    if (index > 0 && Math.abs(points[index - 1].x - x) < Math.abs(points[index].x - x)) index--
    const point = points[index]
    const axisColor = tyreColorsFor(isDark).axis
    const header = (text: string) => `<div style="color:${axisColor};margin-bottom:3px">${text}</div>`
    if (lapPoints) {
      // The hovered lap's values are those at its closing line.
      const lap = point?.lap ?? Math.floor(x) + 1
      const closing = lapPoints.find(candidate => candidate.x === lap)
      let html = header(closing ? pointLabel(closing) : `Lap ${lap} · In progress`)
      series.forEach((item, i) => {
        if (item.visible === false) return
        const value = (item.perLap ? closing : point)?.values[channels[i]]
        const axis = axes?.[item.axis ?? 0]
        const format = axis ? axis.formatTooltip ?? axis.format : formatTooltipY
        html += `<div><span style="color:${themeSeriesColor(item.color, isDark)}">${item.label}</span>: ${value != null && Number.isFinite(value) ? format(value) : '—'}</div>`
      })
      return html
    }
    // The point holds the raw values; the chart's own may be normalized.
    return header(point ? pointLabel(point) : formatX(x)) + (point ? formatValues(point.values) : '')
  }, [points, lapPoints, series, channels, axes, isDark, formatX, formatTooltipY, pointLabel, formatValues])
  // Rows are built one per point, so a row index is a point index.
  const syncConfig = useMemo(() => cursorSync && {
    ...cursorSync,
    formatRow: (_rows: unknown, i: number) => {
      const point = points[i]
      if (!point) return ''
      return `<div style="color:${tyreColorsFor(isDark).axis};margin-top:3px">${title} · ${pointLabel(point)}</div>${formatValues(point.values)}`
    },
    lapAxis: {
      lapStartAt: (_rows: unknown, i: number) => points[i]?.lapStart ?? NaN,
      // The lap whose span holds the session time: the last one started by then.
      rowAtSessionTime: (_rows: unknown, sessionTime: number) => {
        if (!Number.isFinite(sessionTime)) return -1
        for (let i = points.length - 1; i >= 0; i--) {
          const start = points[i].lapStart
          if (start !== undefined && start <= sessionTime) return i
        }
        return -1
      },
    },
  }, [cursorSync, points, isDark, title, pointLabel, formatValues])
  return <LocalChartCoordinatesProvider>
    <div className="relative h-full min-h-0 w-full" aria-label={title}>
      <TimeChartView<StintRow> key={`${revision}:${axesKey}:${seriesKey}`} isDark={isDark} rows={rows} getX={sessionTimeAt} series={chartSeries}
        fixedXRange={xRange} windowSeconds={xRange.max - xRange.min} yRange={yRange}
        yAxisSize={TYRE_CHART_Y_AXIS_SIZE} yTickValues={axes ? axisTicks : roundYTicks} yTickFormat={yTickFormat} yAxisColor={axes?.[0].color} extraYAxes={extraYAxes}
        xTickFormat={formatX} xTickValues={lapTicks}
        overlay={overlay} tooltipFormat={tooltipFormat} cursorSync={syncConfig} colorsFor={tyreColorsFor} axisLook={axisLook} tooltipStyle={TYRE_TOOLTIP_STYLE}
        />
    </div>
  </LocalChartCoordinatesProvider>
}
