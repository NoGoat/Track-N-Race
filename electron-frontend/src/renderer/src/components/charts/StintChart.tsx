import { useCallback, useMemo, useRef } from 'react'
import TimeChartView, { type SeriesDef, type YRangeSpec } from './TimeChartView'
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

export interface StintChartPoint {
  x: number
  values: Array<number | null | undefined>
  label: string
  invalid?: boolean
  fastest?: boolean
  // The lap in progress: its value is still rising.
  provisional?: boolean
}
interface Props {
  title: string
  points: StintChartPoint[]
  series: Array<{ label: string; color: string; visible?: boolean }>
  isDark: boolean
  discrete?: boolean
  fixedRange?: [number, number]
  zeroBaselineHeadroom?: number
  formatX: (value: number) => string
  formatY: (value: number) => string
  formatTooltipY?: (value: number) => string
}
type StintRow = { session_time: number; [key: string]: number }

// Adapt the selected-driver snapshot to the same WebGL renderer used by Tyres.
// Rendering, cursor points, tooltip placement and interaction stay shared.
export default function StintChart({ title, points, series, isDark, discrete, fixedRange, zeroBaselineHeadroom, formatX, formatY, formatTooltipY = formatY }: Props) {
  const previous = useRef<StintChartPoint[]>([])
  const revision = useRef(0)
  // Rebuild only if history before the newest point actually changed.
  // Appends and newest-point patches use the shared incremental GPU bridge.
  if (points !== previous.current) {
    const old = previous.current
    if (points.length < old.length || old.slice(0, -1).some((point, i) =>
      point.x !== points[i]?.x || point.values.some((value, channel) => !Object.is(value, points[i]?.values[channel])))) revision.current++
    previous.current = points
  }
  const rows = useMemo(() => {
    const samples: StintRow[] = []
    points.forEach(point => {
      const row: StintRow = { session_time: point.x }
      point.values.forEach((value, channel) => { if (value != null) row[`value_${channel}`] = value })
      samples.push(row)
    })
    return viewOfRows<StintRow>('stint', samples, false)
  }, [points])
  const chartSeries = useMemo<SeriesDef<StintRow>[]>(() => series.map((item, index) => ({
    ...item, getY: (source, i) => source.num(`value_${index}`, i),
  })), [series])
  const yRange = useMemo<YRangeSpec>(() => {
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
  }, [fixedRange, points, zeroBaselineHeadroom])
  const first = points[0]?.x ?? 0, last = points.at(-1)?.x ?? first + 1
  const xRange = useMemo(() => ({ min: discrete ? first - 0.5 : first, max: discrete ? last + 0.5 : Math.max(last, first + 1) }), [discrete, first, last])
  const tooltipFormat = useCallback((x: number, values: number[]) => {
    let lo = 0, hi = points.length
    while (lo < hi) { const mid = (lo + hi) >> 1; if (points[mid].x < x) lo = mid + 1; else hi = mid }
    let index = Math.min(lo, points.length - 1)
    if (index > 0 && Math.abs(points[index - 1].x - x) < Math.abs(points[index].x - x)) index--
    const point = points[index]
    const axisColor = tyreColorsFor(isDark).axis
    let html = `<div style="color:${axisColor};margin-bottom:3px">${point?.label ?? formatX(x)}${point?.invalid ? ' · Invalid' : ''}${point?.fastest ? ' · Fastest in stint' : ''}${point?.provisional ? ' · In progress' : ''}</div>`
    series.forEach((item, i) => {
      if (item.visible === false) return
      const value = values[i]
      html += `<div><span style="color:${themeSeriesColor(item.color, isDark)}">${item.label}</span>: ${Number.isFinite(value) ? formatTooltipY(value) : '—'}</div>`
    })
    return html
  }, [points, series, isDark, formatX, formatTooltipY])
  return <LocalChartCoordinatesProvider>
    <div className="relative h-full min-h-0 w-full" aria-label={title}>
      <TimeChartView<StintRow> key={revision.current} isDark={isDark} rows={rows} getX={sessionTimeAt} series={chartSeries}
        fixedXRange={xRange} windowSeconds={xRange.max - xRange.min} yRange={yRange}
        yAxisSize={TYRE_CHART_Y_AXIS_SIZE} yTickValues={roundYTicks} yTickFormat={formatY} xTickFormat={formatX} xTickValues={lapTicks}
        tooltipFormat={tooltipFormat} colorsFor={tyreColorsFor} axisLook={TYRE_AXIS_LOOK} tooltipStyle={TYRE_TOOLTIP_STYLE}
        />
    </div>
  </LocalChartCoordinatesProvider>
}
