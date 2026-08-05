import { useEffect, useRef, type MutableRefObject } from 'react'
import { useChartTooltip, TOOLTIP_STYLE } from '../../hooks/useChartTooltip'
import { ANALYZE_METRICS, ANALYZE_METRIC_BY_ID, type AnalyzeSeriesConfig, type AnalyzeSource } from '../../lib/analyzeMetrics'
import { createAxisPlugin, type AxisConfig } from '../../lib/timechart/axisPlugin'
import { TimeChart, corePlugins, type TChart } from '../../lib/timechart/tc'
import { AlignedDataBuffer, type SeriesData } from '../../lib/timechart/engine/core/alignedData'
import type { AnalyzeLapData, LapProgressPoint } from '../../types'

interface Props {
  isDark: boolean
  current: AnalyzeLapData
  currentRevision: string | number
  comparison: AnalyzeLapData | null
  selected: AnalyzeSeriesConfig[]
  primaryLabel?: string
  comparisonLabel?: string
  distanceMode: boolean
  trackLengthM: number
  deltaPositiveColor: string
  deltaNegativeColor: string
  zoomEnabled: boolean
  controlsRef: MutableRefObject<AnalyzeChartControls | null>
}

export interface AnalyzeChartControls {
  zoomIn: () => void
  zoomOut: () => void
  panLeft: () => void
  panRight: () => void
  reset: () => void
}

type Role = 'comparison' | 'current'
type Buffers = Record<Role, Record<AnalyzeSource, AlignedDataBuffer>> & {
  deltaPositive: AlignedDataBuffer
  deltaNegative: AlignedDataBuffer
}
type SeriesRecord = Record<Role, Map<string, any>>

const SOURCES: AnalyzeSource[] = ['telemetry', 'motion', 'motionEx', 'status', 'damage']
const Y_TICKS = [0, 0.25, 0.5, 0.75, 1]
const TOP_PADDING = 16
const MIN_ZOOM_SECONDS = 0.5
const MIN_ZOOM_METRES = 25
const DELTA_GRID_METRES = 5
const METRICS_BY_SOURCE = Object.fromEntries(SOURCES.map(source => [source, ANALYZE_METRICS.filter(metric => metric.source === source)])) as Record<AnalyzeSource, typeof ANALYZE_METRICS>

function rowsFor(lap: AnalyzeLapData, source: AnalyzeSource): any[] {
  if (source === 'status') return lap.statusHistory
  if (source === 'damage') return lap.damageHistory
  return lap[source]
}

function makeBuffers(): Buffers {
  const makeRole = () => Object.fromEntries(SOURCES.map(source => [source, new AlignedDataBuffer(METRICS_BY_SOURCE[source].length)])) as Record<AnalyzeSource, AlignedDataBuffer>
  return {
    comparison: makeRole(), current: makeRole(),
    deltaPositive: new AlignedDataBuffer(1),
    deltaNegative: new AlignedDataBuffer(1),
  }
}

interface ProgressMap {
  points: LapProgressPoint[]
  maxSessionTime: number
  maxDistance: number
}

function buildProgressMap(lap: AnalyzeLapData | null): ProgressMap | null {
  if (!lap || lap.lapProgress.length === 0) return null
  const points: LapProgressPoint[] = [{
    session_time: lap.startSessionTime,
    current_lap_ms: 0,
    lap_distance_m: 0,
  }]
  let lastTime = lap.startSessionTime
  let lastDistance = 0
  for (const point of lap.lapProgress) {
    if (point.session_time > lap.endSessionTime) break
    if (!Number.isFinite(point.session_time) || !Number.isFinite(point.lap_distance_m) ||
        !Number.isFinite(point.current_lap_ms) || point.session_time < lastTime ||
        point.lap_distance_m < lastDistance || point.current_lap_ms < 0) continue
    // The zero-distance/zero-time point is the mathematical lap origin. Keep it
    // even when the first recorded packet has the same timestamp: playback's
    // native seek boundary and the indexed JSON row can differ by sub-microsecond
    // float representation, but that must not change the resulting progress map.
    if (points.length === 1) {
      if (point.lap_distance_m === 0) continue
      points.push(point)
    } else if (point.lap_distance_m === lastDistance) {
      // Distance -> elapsed time is defined by the first arrival at a distance.
      // Recorded rows can repeat the exact float distance while time advances.
      // Replacing that point with the later row makes a completed map disagree
      // with its own live prefix and leaves false delta dips behind.
      continue
    } else if (point.session_time === lastTime) {
      points[points.length - 1] = point
    } else {
      points.push(point)
    }
    lastTime = point.session_time
    lastDistance = point.lap_distance_m
  }
  if (points.length < 2) return null
  return { points, maxSessionTime: lastTime, maxDistance: lastDistance }
}

function interpolateDistance(progress: ProgressMap, sessionTime: number): number {
  const points = progress.points
  if (sessionTime < points[0].session_time || sessionTime > progress.maxSessionTime) return NaN
  let lo = 1, hi = points.length
  while (lo < hi) {
    const mid = (lo + hi) >> 1
    if (points[mid].session_time < sessionTime) lo = mid + 1
    else hi = mid
  }
  if (lo >= points.length) return points[points.length - 1].lap_distance_m
  const before = points[lo - 1], after = points[lo]
  const span = after.session_time - before.session_time
  const ratio = span > 0 ? (sessionTime - before.session_time) / span : 1
  return before.lap_distance_m + (after.lap_distance_m - before.lap_distance_m) * ratio
}

function interpolateElapsed(progress: ProgressMap, distance: number): number {
  const points = progress.points
  if (distance < 0 || distance > progress.maxDistance) return NaN
  let lo = 1, hi = points.length
  while (lo < hi) {
    const mid = (lo + hi) >> 1
    if (points[mid].lap_distance_m < distance) lo = mid + 1
    else hi = mid
  }
  if (lo >= points.length) return points[points.length - 1].current_lap_ms / 1000
  const before = points[lo - 1], after = points[lo]
  const span = after.lap_distance_m - before.lap_distance_m
  const ratio = span > 0 ? (distance - before.lap_distance_m) / span : 1
  return (before.current_lap_ms + (after.current_lap_ms - before.current_lap_ms) * ratio) / 1000
}

function nearestIndex(data: SeriesData, x: number): number {
  if (data.length === 0) return -1
  const after = data.lowerBoundX(x)
  if (after === 0) return 0
  if (after === data.length) return data.length - 1
  return x - data.xAt(after - 1) <= data.xAt(after) - x ? after - 1 : after
}

function syncSource(buffer: AlignedDataBuffer, rows: any[], source: AnalyzeSource, origin: number, rebuild: boolean, scratch: Float64Array): boolean {
  const defs = METRICS_BY_SOURCE[source]
  if (rebuild) buffer.clear()
  if (rows.length === 0) {
    // Source slices are published independently and can be transiently empty.
    // Preserve the accumulated lap unless the store explicitly changed the
    // lap revision (in which case the rebuild above already cleared it).
    return rebuild
  }
  const lastX = buffer.length ? buffer.lastX : -Infinity
  let lo = 0, hi = rows.length
  while (lo < hi) {
    const mid = (lo + hi) >> 1
    if (rows[mid].session_time - origin <= lastX) lo = mid + 1
    else hi = mid
  }
  const appendStart = lo
  let changed = false

  // The same timestamp can continue arriving across separate renderer
  // updates. Replace the buffered value with the final row published for that
  // timestamp instead of retaining the first value or appending a duplicate X.
  if (buffer.length && lo > 0 && rows[lo - 1].session_time - origin === lastX) {
    const row = rows[lo - 1]
    let differs = false
    for (let channel = 0; channel < defs.length; channel++) {
      const def = defs[channel]
      const value = def.getValue(row)
      const normalized = Number.isFinite(value) ? (value - def.min) / (def.max - def.min) : NaN
      scratch[channel] = normalized
      const previous = buffer.yAt(channel, buffer.length - 1)
      const stored = Math.fround(normalized)
      if (stored !== previous && !(Number.isNaN(stored) && Number.isNaN(previous))) differs = true
    }
    if (differs) {
      buffer.replaceLast(scratch)
      changed = true
    }
  }

  for (let i = appendStart; i < rows.length;) {
    // Packet sources commonly publish several rows at the exact same session
    // time while the lap timer is stopped (most visibly at 0:00.000). Keep the
    // final row for that timestamp so every source has a strictly unique X.
    let next = i + 1
    while (next < rows.length && rows[next].session_time === rows[i].session_time) next++
    const row = rows[next - 1]
    for (let channel = 0; channel < defs.length; channel++) {
      const def = defs[channel]
      const value = def.getValue(row)
      scratch[channel] = Number.isFinite(value) ? (value - def.min) / (def.max - def.min) : NaN
    }
    buffer.append(row.session_time - origin, scratch)
    changed = true
    i = next
  }
  // Do not trim to the first row of a current publication. The five source
  // slices are published independently and one can briefly contain only its
  // newest row while lap metadata catches up. Lap changes and explicit seeks
  // rebuild the whole buffer explicitly, so front-trimming here is both
  // unnecessary and the cause of visible full-lap/one-point flicker.
  return rebuild || changed
}

function syncSourceDistance(
  buffer: AlignedDataBuffer,
  rows: any[],
  source: AnalyzeSource,
  progress: ProgressMap | null,
  rebuild: boolean,
  scratch: Float64Array,
  cursor: { value: number },
): boolean {
  if (rebuild) { buffer.clear(); cursor.value = -Infinity }
  if (!progress || rows.length === 0) return rebuild
  const defs = METRICS_BY_SOURCE[source]
  let lo = 0, hi = rows.length
  while (lo < hi) {
    const mid = (lo + hi) >> 1
    if (rows[mid].session_time <= cursor.value) lo = mid + 1
    else hi = mid
  }
  let changed = false
  for (let i = lo; i < rows.length;) {
    let next = i + 1
    while (next < rows.length && rows[next].session_time === rows[i].session_time) next++
    const row = rows[next - 1]
    if (row.session_time > progress.maxSessionTime) break
    cursor.value = row.session_time
    const distance = interpolateDistance(progress, row.session_time)
    if (!Number.isFinite(distance)) { i = next; continue }
    for (let channel = 0; channel < defs.length; channel++) {
      const def = defs[channel]
      const value = def.getValue(row)
      scratch[channel] = Number.isFinite(value) ? (value - def.min) / (def.max - def.min) : NaN
    }
    if (buffer.length && distance === buffer.lastX) buffer.replaceLast(scratch)
    else if (!buffer.length || distance > buffer.lastX) buffer.append(distance, scratch)
    changed = true
    i = next
  }
  return rebuild || changed
}

interface DeltaSampleState {
  revision: string
  samples: Array<[number, number]>
  renderedCount: number
  renderedRange: number
}

function rewriteDeltaRange(
  positive: AlignedDataBuffer,
  negative: AlignedDataBuffer,
  samples: Array<[number, number]>,
  count: number,
  range: number,
) {
  const positiveValues: number[] = []
  const negativeValues: number[] = []
  let previous: [number, number] | null = null
  for (let index = 0; index < count; index++) {
    const sample = samples[index]
    const delta = sample[1]
    if (previous && Math.sign(previous[1]) !== Math.sign(delta) && previous[1] !== 0 && delta !== 0) {
      positiveValues.push(0.5)
      negativeValues.push(0.5)
    }
    const normalized = 0.5 + delta / (2 * range)
    positiveValues.push(delta >= 0 ? normalized : NaN)
    negativeValues.push(delta <= 0 ? normalized : NaN)
    previous = sample
  }
  positive.replaceChannel(0, positiveValues)
  negative.replaceChannel(0, negativeValues)
}

function syncDelta(
  positive: AlignedDataBuffer,
  negative: AlignedDataBuffer,
  current: ProgressMap | null,
  comparison: ProgressMap | null,
  state: DeltaSampleState,
): { range: number; changed: boolean } {
  if (!current || !comparison) {
    const changed = positive.length > 0 || negative.length > 0
    state.samples.length = 0
    state.renderedCount = 0
    state.renderedRange = 0
    positive.clear()
    negative.clear()
    return { range: 0.5, changed }
  }
  const maxDistance = Math.min(current.maxDistance, comparison.maxDistance)
  if (maxDistance <= 0) return { range: state.renderedRange || 0.5, changed: false }
  const startDistance = state.samples.length
    ? state.samples[state.samples.length - 1][0] + DELTA_GRID_METRES
    : 0
  for (let distance = startDistance; distance <= maxDistance; distance += DELTA_GRID_METRES) {
    const currentElapsed = interpolateElapsed(current, distance)
    const comparisonElapsed = interpolateElapsed(comparison, distance)
    const delta = currentElapsed - comparisonElapsed
    if (!Number.isFinite(delta)) continue
    state.samples.push([distance, delta])
  }
  const maxAbs = state.samples.reduce((max, sample) => Math.max(max, Math.abs(sample[1])), 0)
  const range = Math.max(0.5, Math.ceil(maxAbs * 10) / 10)
  const newRevision = state.renderedCount === 0 && (positive.length > 0 || negative.length > 0)
  if (newRevision) {
    positive.clear()
    negative.clear()
  } else if (state.renderedCount > 0 && state.renderedRange !== range) {
    rewriteDeltaRange(positive, negative, state.samples, state.renderedCount, range)
  }
  if (state.renderedCount >= state.samples.length) {
    const changed = state.renderedRange !== range
    state.renderedRange = range
    return { range, changed }
  }
  const positiveY = new Float64Array(1)
  const negativeY = new Float64Array(1)
  let previous: [number, number] | null = state.renderedCount > 0
    ? state.samples[state.renderedCount - 1]
    : null
  for (let index = state.renderedCount; index < state.samples.length; index++) {
    const [distance, delta] = state.samples[index]
    if (previous && Math.sign(previous[1]) !== Math.sign(delta) && previous[1] !== 0 && delta !== 0) {
      const zeroDistance = previous[0] + (distance - previous[0]) * Math.abs(previous[1]) / (Math.abs(previous[1]) + Math.abs(delta))
      positiveY[0] = negativeY[0] = 0.5
      positive.append(zeroDistance, positiveY)
      negative.append(zeroDistance, negativeY)
    }
    const normalized = 0.5 + delta / (2 * range)
    positiveY[0] = delta >= 0 ? normalized : NaN
    negativeY[0] = delta <= 0 ? normalized : NaN
    positive.append(distance, positiveY)
    negative.append(distance, negativeY)
    previous = [distance, delta]
  }
  state.renderedCount = state.samples.length
  state.renderedRange = range
  return { range, changed: true }
}

function blendColor(hex: string, isDark: boolean): string {
  const match = /^#([0-9a-f]{6})$/i.exec(hex)
  if (!match) return hex
  const value = Number.parseInt(match[1], 16)
  const bg = isDark ? [0x12, 0x14, 0x1f] : [0xff, 0xff, 0xff]
  const rgb = [(value >> 16) & 255, (value >> 8) & 255, value & 255]
  return `#${rgb.map((channel, i) => Math.round(channel * 0.35 + bg[i] * 0.65).toString(16).padStart(2, '0')).join('')}`
}

function fmtLapTime(seconds: number): string {
  return `${Math.floor(seconds / 60)}:${(seconds % 60).toFixed(1).padStart(4, '0')}`
}

function fmtDistance(metres: number): string {
  return `${Math.round(metres)} m`
}

export default function AnalyzeTimeChart({
  isDark, current, currentRevision, comparison, selected, primaryLabel, comparisonLabel,
  distanceMode, trackLengthM, deltaPositiveColor, deltaNegativeColor,
  zoomEnabled, controlsRef,
}: Props) {
  const { tooltipRef, show, hide } = useChartTooltip()
  const containerRef = useRef<HTMLDivElement>(null)
  const hostRef = useRef<HTMLDivElement>(null)
  const chartRef = useRef<TChart | null>(null)
  const buffersRef = useRef<Buffers | null>(null)
  const seriesRef = useRef<SeriesRecord | null>(null)
  const axisCfgRef = useRef<{ current: AxisConfig } | null>(null)
  const deltaSeriesRef = useRef<{ positive: any; negative: any } | null>(null)
  const deltaRangeRef = useRef(0.5)
  const deltaSamplesRef = useRef<DeltaSampleState>({ revision: '', samples: [], renderedCount: 0, renderedRange: 0 })
  const deltaColorsRef = useRef({ positive: deltaPositiveColor, negative: deltaNegativeColor })
  const selectedRef = useRef(selected)
  const isDarkRef = useRef(isDark)
  const currentRef = useRef(current)
  const comparisonRef = useRef(comparison)
  const primaryLabelRef = useRef(primaryLabel)
  const comparisonLabelRef = useRef(comparisonLabel)
  const zoomEnabledRef = useRef(zoomEnabled)
  const distanceModeRef = useRef(distanceMode)
  const fullXRangeRef = useRef({ min: 0, max: 1 })
  const revisionsRef = useRef<Record<string, string>>({})
  const originsRef = useRef<Record<string, number>>({})
  const distanceCursorsRef = useRef<Record<string, { value: number }>>({})
  const scratchRef = useRef<Record<AnalyzeSource, Float64Array>>(Object.fromEntries(SOURCES.map(source => [source, new Float64Array(METRICS_BY_SOURCE[source].length)])) as Record<AnalyzeSource, Float64Array>)
  selectedRef.current = selected
  isDarkRef.current = isDark
  currentRef.current = current
  comparisonRef.current = comparison
  primaryLabelRef.current = primaryLabel
  comparisonLabelRef.current = comparisonLabel
  zoomEnabledRef.current = zoomEnabled
  distanceModeRef.current = distanceMode
  deltaColorsRef.current = { positive: deltaPositiveColor, negative: deltaNegativeColor }

  useEffect(() => {
    const host = hostRef.current
    if (!host) return
    const buffers = makeBuffers()
    buffersRef.current = buffers
    const axisCfg = { current: {
      axisColor: '#7c8098', gridColor: 'rgba(255,255,255,0.04)', borderColor: '#1e2136',
      font: '10px "Cascadia Code", ui-monospace, monospace', xTickSpacePx: 80,
      xTickFormat: fmtLapTime, yTickValues: () => [], yTickFormat: () => '',
      xGap: 2, yGap: 4, showYGrid: true, extraYAxes: [],
    } satisfies AxisConfig }
    axisCfgRef.current = axisCfg
    const rawSeries: any[] = []
    for (const role of ['comparison', 'current'] as Role[]) {
      for (const def of ANALYZE_METRICS) {
        const channel = METRICS_BY_SOURCE[def.source].findIndex(candidate => candidate.id === def.id)
        rawSeries.push({
          name: `${role}:${def.id}`, color: def.defaultColor, visible: false,
          lineWidth: role === 'comparison' ? 1.25 : 1.75,
          lineType: def.lineType === 'step' ? TimeChart.LineType.Step : TimeChart.LineType.Line,
          stepLocation: def.lineType === 'step' ? 1 : 0,
          data: buffers[role][def.source].series[channel],
        })
      }
    }
    const deltaSeries = {
      positive: {
        name: 'delta:positive', color: deltaPositiveColor, visible: false, lineWidth: 2,
        lineType: TimeChart.LineType.Line, data: buffers.deltaPositive.series[0],
      },
      negative: {
        name: 'delta:negative', color: deltaNegativeColor, visible: false, lineWidth: 2,
        lineType: TimeChart.LineType.Line, data: buffers.deltaNegative.series[0],
      },
    }
    rawSeries.push(deltaSeries.positive, deltaSeries.negative)
    deltaSeriesRef.current = deltaSeries
    const chart = new TimeChart.core(host, {
      paddingTop: TOP_PADDING, paddingRight: 12, paddingBottom: 24, paddingLeft: 12,
      renderPaddingTop: TOP_PADDING, renderPaddingRight: 12, renderPaddingBottom: 24, renderPaddingLeft: 12,
      yRange: { min: 0, max: 1 }, lineWidth: 1.5, series: rawSeries,
      plugins: {
        lineChart: corePlugins.lineChart, crosshair: corePlugins.crosshair,
        nearestPoint: corePlugins.nearestPoint, axis: createAxisPlugin(axisCfg),
      } as any,
    } as any)
    chartRef.current = chart

    const applyXDomain = (requestedMin: number, requestedMax: number) => {
      if (!zoomEnabledRef.current) return
      const full = fullXRangeRef.current
      const fullExtent = Math.max(0, full.max - full.min)
      if (fullExtent <= 0) return
      const minExtent = Math.min(distanceModeRef.current ? MIN_ZOOM_METRES : MIN_ZOOM_SECONDS, fullExtent)
      const extent = Math.min(fullExtent, Math.max(minExtent, requestedMax - requestedMin))
      let min = requestedMin - (extent - (requestedMax - requestedMin)) / 2
      min = Math.max(full.min, Math.min(min, full.max - extent))
      chart.options.xRange = null
      chart.model.xScale.domain([min, min + extent])
      chart.model.requestRedraw()
    }
    const zoomBy = (factor: number, anchor?: number) => {
      if (!zoomEnabledRef.current) return
      const [min, max] = chart.model.xScale.domain().map(Number)
      const center = anchor ?? (min + max) / 2
      applyXDomain(center + (min - center) * factor, center + (max - center) * factor)
    }
    const panBy = (fraction: number) => {
      if (!zoomEnabledRef.current) return
      const [min, max] = chart.model.xScale.domain().map(Number)
      const delta = (max - min) * fraction
      applyXDomain(min + delta, max + delta)
    }
    const resetZoom = () => {
      const full = fullXRangeRef.current
      chart.options.xRange = { ...full }
      chart.model.xScale.domain([full.min, full.max])
      chart.model.requestRedraw()
    }
    const controls: AnalyzeChartControls = {
      zoomIn: () => zoomBy(0.7),
      zoomOut: () => zoomBy(1 / 0.7),
      panLeft: () => panBy(-0.2),
      panRight: () => panBy(0.2),
      reset: resetZoom,
    }
    controlsRef.current = controls

    const interactionNode = chart.contentBoxDetector.node
    let dragPointer: number | null = null
    let dragX = 0
    let dragDomain: [number, number] = [0, 1]
    const onWheel = (event: WheelEvent) => {
      if (!zoomEnabledRef.current) return
      event.preventDefault()
      const rect = interactionNode.getBoundingClientRect()
      if (event.ctrlKey || event.metaKey) {
        const [min, max] = chart.model.xScale.domain().map(Number)
        const ratio = rect.width > 0 ? Math.max(0, Math.min(1, (event.clientX - rect.left) / rect.width)) : 0.5
        const anchor = min + (max - min) * ratio
        zoomBy(Math.max(0.5, Math.min(2, Math.exp(event.deltaY * 0.002))), anchor)
      } else if (rect.width > 0) {
        const [min, max] = chart.model.xScale.domain().map(Number)
        const delta = (event.deltaX + event.deltaY) * (max - min) / rect.width
        applyXDomain(min + delta, max + delta)
      }
    }
    const onPointerDown = (event: PointerEvent) => {
      if (!zoomEnabledRef.current || event.button !== 0) return
      dragPointer = event.pointerId
      dragX = event.clientX
      dragDomain = chart.model.xScale.domain().map(Number) as [number, number]
      interactionNode.setPointerCapture(event.pointerId)
      interactionNode.style.cursor = 'grabbing'
    }
    const onPointerMove = (event: PointerEvent) => {
      if (dragPointer !== event.pointerId || interactionNode.clientWidth <= 0) return
      const extent = dragDomain[1] - dragDomain[0]
      const delta = -(event.clientX - dragX) * extent / interactionNode.clientWidth
      applyXDomain(dragDomain[0] + delta, dragDomain[1] + delta)
    }
    const stopDrag = (event: PointerEvent) => {
      if (dragPointer !== event.pointerId) return
      dragPointer = null
      if (interactionNode.hasPointerCapture(event.pointerId)) interactionNode.releasePointerCapture(event.pointerId)
      interactionNode.style.cursor = zoomEnabledRef.current ? 'grab' : ''
    }
    interactionNode.addEventListener('wheel', onWheel, { passive: false })
    interactionNode.addEventListener('pointerdown', onPointerDown)
    interactionNode.addEventListener('pointermove', onPointerMove)
    interactionNode.addEventListener('pointerup', stopDrag)
    interactionNode.addEventListener('pointercancel', stopDrag)
    interactionNode.addEventListener('dblclick', resetZoom)
    const records: SeriesRecord = { comparison: new Map(), current: new Map() }
    for (const option of chart.options.series) {
      if (option.name.startsWith('delta:')) continue
      const [role, id] = option.name.split(':') as [Role, string]
      records[role].set(id, option)
    }
    seriesRef.current = records

    const move = (contentX: number, contentY: number) => {
      const x = (chart.model.xScale as any).invert(contentX + chart.options.paddingLeft) as number
      const rows: string[] = [`<div style="color:var(--text-secondary);margin-bottom:4px">${distanceModeRef.current ? fmtDistance(x) : fmtLapTime(x)}</div>`]
      const appendRole = (role: Role, lap: AnalyzeLapData | null, heading: string) => {
        if (!lap) return
        rows.push(`<div style="color:var(--text-secondary);font-size:10px;margin:${rows.length > 1 ? '5px' : '0'} 0 2px">${heading}</div>`)
        for (const item of selectedRef.current) {
          if (!item.visible) continue
          const def = ANALYZE_METRIC_BY_ID.get(item.metricId)
          const option = seriesRef.current?.[role].get(item.metricId)
          if (!def || !option) continue
          const index = nearestIndex(option.data, x)
          const normalized = index >= 0 ? option.data.yAt(index) : NaN
          const value = def.min + normalized * (def.max - def.min)
          const display = Number.isFinite(value) ? def.format(value) : '—'
          const color = role === 'comparison' ? blendColor(item.color, isDarkRef.current) : item.color
          rows.push(`<div><span style="color:${color}">${def.label}</span>: ${display}</div>`)
        }
      }
      appendRole('current', currentRef.current, primaryLabelRef.current ?? `CURRENT L${currentRef.current.lapNum || '—'}`)
      appendRole('comparison', comparisonRef.current, comparisonRef.current ? comparisonLabelRef.current ?? `COMPARE L${comparisonRef.current.lapNum}` : '')
      const deltaSeries = deltaSeriesRef.current
      if (distanceModeRef.current && deltaSeries?.positive.visible) {
        const index = nearestIndex(deltaSeries.positive.data, x)
        const positive = index >= 0 ? deltaSeries.positive.data.yAt(index) : NaN
        const negative = index >= 0 ? deltaSeries.negative.data.yAt(index) : NaN
        const normalized = Number.isFinite(positive) ? positive : negative
        const delta = (normalized - 0.5) * 2 * deltaRangeRef.current
        const color = delta >= 0 ? deltaColorsRef.current.positive : deltaColorsRef.current.negative
        rows.push(`<div style="margin-top:5px"><span style="color:${color}">Delta</span>: ${Number.isFinite(delta) ? `${delta >= 0 ? '+' : ''}${delta.toFixed(3)} s` : '—'}</div>`)
      }
      show(rows.join(''), contentX + chart.options.paddingLeft, contentY + 4, chart.clientWidth, chart.clientHeight)
    }
    const stopTooltipSync = chart.nearestPoint.updated.on(() => {
      const pointer = chart.nearestPoint.lastPointerPos
      if (!pointer) { hide(); return }
      move(pointer.x - chart.options.paddingLeft, pointer.y - chart.options.paddingTop)
    })
    return () => {
      interactionNode.removeEventListener('wheel', onWheel)
      interactionNode.removeEventListener('pointerdown', onPointerDown)
      interactionNode.removeEventListener('pointermove', onPointerMove)
      interactionNode.removeEventListener('pointerup', stopDrag)
      interactionNode.removeEventListener('pointercancel', stopDrag)
      interactionNode.removeEventListener('dblclick', resetZoom)
      if (controlsRef.current === controls) controlsRef.current = null
      stopTooltipSync(); chart.dispose()
      chartRef.current = null; buffersRef.current = null; seriesRef.current = null; axisCfgRef.current = null
      deltaSeriesRef.current = null
    }
    // Stable chart lifetime; all changing inputs are applied imperatively below.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

  useEffect(() => {
    const chart = chartRef.current
    const records = seriesRef.current
    const axisHolder = axisCfgRef.current
    if (!chart || !records || !axisHolder) return
    const selectedDefs = selected.flatMap(item => {
      const def = ANALYZE_METRIC_BY_ID.get(item.metricId)
      return def ? [{ item, def }] : []
    })
    const visibleDefs = selectedDefs.filter(({ item }) => item.visible)
    const visibleIds = new Set(visibleDefs.map(({ def }) => def.id))
    for (const def of ANALYZE_METRICS) {
      const item = selectedDefs.find(candidate => candidate.def.id === def.id)?.item
      const visible = visibleIds.has(def.id)
      const currentOption = records.current.get(def.id)
      const comparisonOption = records.comparison.get(def.id)
      if (currentOption) { currentOption.visible = visible; currentOption.color = item?.color ?? def.defaultColor }
      if (comparisonOption) { comparisonOption.visible = visible && !!comparison; comparisonOption.color = blendColor(item?.color ?? def.defaultColor, isDark) }
    }
    // WebGL paints later series over earlier ones, so reverse the sidebar order:
    // the first row in the sidebar is drawn last and remains visually on top.
    const deltaItem = selected.find(item => item.metricId === 'delta')
    const showDelta = deltaItem?.visible !== false && distanceMode && !!comparison
    const deltaSeries = deltaSeriesRef.current
    if (deltaSeries) {
      deltaSeries.positive.visible = showDelta
      deltaSeries.positive.color = deltaPositiveColor
      deltaSeries.negative.visible = showDelta
      deltaSeries.negative.color = deltaNegativeColor
    }
    const drawOrder = [...selected].reverse().flatMap(item => {
      if (!item.visible) return []
      if (item.metricId === 'delta') return showDelta && deltaSeries ? [deltaSeries.positive, deltaSeries.negative] : []
      const currentOption = records.current.get(item.metricId)
      const comparisonOption = records.comparison.get(item.metricId)
      return comparisonOption && comparison ? [comparisonOption, currentOption].filter(Boolean) : [currentOption].filter(Boolean)
    })
    const visibleOptions = new Set(drawOrder)
    const hidden = chart.options.series.filter(option => {
      return !visibleOptions.has(option)
    })
    // Keep the array identity stable for every plugin/renderer that received it
    // at chart construction while still updating the live GPU draw order.
    chart.options.series.splice(0, chart.options.series.length, ...drawOrder, ...hidden)

    const seenScales = new Set<string>()
    const axes: Array<
      | { kind: 'delta'; item: AnalyzeSeriesConfig }
      | { kind: 'metric'; item: AnalyzeSeriesConfig; def: (typeof ANALYZE_METRICS)[number] }
    > = []
    for (const item of selected) {
      if (!item.visible || !item.showYAxis) continue
      if (item.metricId === 'delta') {
        if (showDelta) axes.push({ kind: 'delta', item })
        continue
      }
      const def = ANALYZE_METRIC_BY_ID.get(item.metricId)
      if (!def || seenScales.has(def.scaleKey)) continue
      seenScales.add(def.scaleKey)
      axes.push({ kind: 'metric', item, def })
    }
    const axisCount = axes.length
    const leftCount = Math.ceil(axisCount / 2)
    const rightCount = Math.floor(axisCount / 2)
    const left = axisCount > 0 ? Math.max(44, 12 + leftCount * 54) : 12
    const right = axisCount > 0 ? Math.max(12, 12 + rightCount * 54) : 12
    const axis = isDark ? '#7c8098' : '#6b7280'
    const first = axes[0]
    const extraYAxes = axes.slice(1).map((entry, index) => {
      const axisIndex = index + 1
      const side = axisIndex % 2 === 1 ? 'right' as const : 'left' as const
      const slot = Math.floor(axisIndex / 2)
      return {
        side, offset: 4 + slot * 54,
        color: entry.kind === 'delta' ? deltaPositiveColor : entry.item.color,
        colorForValue: entry.kind === 'delta'
          ? (normalized: number) => normalized < 0.5 ? deltaNegativeColor : normalized > 0.5 ? deltaPositiveColor : axis
          : undefined,
        values: Y_TICKS,
        format: (normalized: number) => entry.kind === 'delta'
          ? `${((normalized - 0.5) * 2 * deltaRangeRef.current).toFixed(1)}s`
          : entry.def.axisFormat(entry.def.min + normalized * (entry.def.max - entry.def.min)),
      }
    })
    axisHolder.current = {
      axisColor: axis,
      yAxisColor: first?.kind === 'delta' ? deltaPositiveColor : first?.item.color ?? axis,
      gridColor: isDark ? 'rgba(255,255,255,0.04)' : 'rgba(0,0,0,0.07)',
      borderColor: isDark ? '#1e2136' : '#d0d5e0',
      font: '10px "Cascadia Code", ui-monospace, monospace',
      xTickSpacePx: 80, xTickFormat: distanceMode ? fmtDistance : fmtLapTime,
      yTickValues: () => first ? Y_TICKS : [],
      yTickColor: first?.kind === 'delta'
        ? normalized => normalized < 0.5 ? deltaNegativeColor : normalized > 0.5 ? deltaPositiveColor : axis
        : undefined,
      yTickFormat: normalized => first?.kind === 'delta'
        ? `${((normalized - 0.5) * 2 * deltaRangeRef.current).toFixed(1)}s`
        : first?.kind === 'metric' ? first.def.axisFormat(first.def.min + normalized * (first.def.max - first.def.min)) : '',
      xGap: 2, yGap: 4, showYGrid: axisCount > 0,
      extraYAxes,
    }
    Object.assign(chart.options, { paddingLeft: left, paddingRight: right, renderPaddingLeft: left, renderPaddingRight: right })
    chart.contentBoxDetector.setPadding(left, right, chart.options.paddingTop, chart.options.paddingBottom)
    hostRef.current?.style.setProperty('--background-overlay', isDark ? '#12141f' : '#ffffff')
    if (hostRef.current) hostRef.current.style.color = axis
    chart.update()
    chart.model.resize(chart.clientWidth, chart.clientHeight)
  }, [comparison, deltaNegativeColor, deltaPositiveColor, distanceMode, isDark, selected])

  useEffect(() => {
    const chart = chartRef.current
    const buffers = buffersRef.current
    if (!chart || !buffers) return
    const progressByRole: Record<Role, ProgressMap | null> = {
      current: distanceMode ? buildProgressMap(current) : null,
      comparison: distanceMode ? buildProgressMap(comparison) : null,
    }
    let changed = false
    for (const role of ['current', 'comparison'] as Role[]) {
      const lap = role === 'current' ? current : comparison
      for (const source of SOURCES) {
        // Current-lap rows, lap metadata and the playback-state cursor arrive on
        // separate channels. Keep the origin fixed for the lifetime of a lap;
        // otherwise a one-frame metadata mismatch shifts every X value and
        // alternates the chart between a full lap and a one-point rebuild.
        const revision = lap
          ? role === 'current'
            ? `${distanceMode ? 'distance' : 'time'}:${currentRevision}:${source}`
            : `${distanceMode ? 'distance' : 'time'}:${lap.lapNum}:${lap.startSessionTime}:${source}`
          : `${distanceMode ? 'distance' : 'time'}:none:${source}`
        const revisionKey = `${role}:${source}`
        let rebuild = revisionsRef.current[revisionKey] !== revision
        revisionsRef.current[revisionKey] = revision
        if (rebuild) {
          originsRef.current[revisionKey] = lap?.startSessionTime ?? 0
        }
        const rows = lap ? rowsFor(lap, source) : []
        const buffer = buffers[role][source]
        if (distanceMode) {
          const cursor = distanceCursorsRef.current[revisionKey] ??= { value: -Infinity }
          if (syncSourceDistance(
            buffer, rows, source, progressByRole[role], rebuild,
            scratchRef.current[source], cursor,
          )) changed = true
          continue
        }
        if (rows.length === 0) {
          if (syncSource(buffer, rows, source, 0, rebuild, scratchRef.current[source])) changed = true
          // Only an explicit revision is allowed to invalidate the origin.
          // A normal cross-channel empty publication must remain a no-op.
          if (role === 'current' && rebuild) originsRef.current[revisionKey] = NaN
          continue
        }
        let origin = originsRef.current[revisionKey]
        if (!Number.isFinite(origin)) {
          origin = lap?.startSessionTime ?? 0
          originsRef.current[revisionKey] = origin
          rebuild = true
        }
        if (syncSource(buffer, rows, source, origin, rebuild, scratchRef.current[source])) changed = true
      }
    }
    if (distanceMode) {
      const deltaRevision = `${currentRevision}:${comparison?.lapNum ?? 0}:${comparison?.startSessionTime ?? 0}`
      if (deltaSamplesRef.current.revision !== deltaRevision) {
        deltaSamplesRef.current = { revision: deltaRevision, samples: [], renderedCount: 0, renderedRange: 0 }
      }
      const deltaResult = syncDelta(
        buffers.deltaPositive, buffers.deltaNegative,
        progressByRole.current, progressByRole.comparison,
        deltaSamplesRef.current,
      )
      deltaRangeRef.current = deltaResult.range
      if (deltaResult.changed) changed = true
    } else if (buffers.deltaPositive.length || buffers.deltaNegative.length) {
      deltaSamplesRef.current = { revision: '', samples: [], renderedCount: 0, renderedRange: 0 }
      buffers.deltaPositive.clear()
      buffers.deltaNegative.clear()
      changed = true
    }
    if (!changed) return
    let max = distanceMode && trackLengthM > 0 ? trackLengthM : 1
    for (const role of ['current', 'comparison'] as Role[]) {
      for (const source of SOURCES) {
        const buffer = buffers[role][source]
        if (buffer.length) max = Math.max(max, buffer.lastX)
      }
    }
    chart.options.xRange = { min: 0, max }
    fullXRangeRef.current = { min: 0, max }
    chart.model.requestRedraw()
  }, [comparison, current, currentRevision, distanceMode, trackLengthM])

  useEffect(() => {
    const node = chartRef.current?.contentBoxDetector.node
    if (node) node.style.cursor = zoomEnabled ? 'grab' : ''
    if (!zoomEnabled) controlsRef.current?.reset()
  }, [controlsRef, zoomEnabled])

  useEffect(() => {
    const container = containerRef.current
    if (!container) return
    let animationFrame = 0
    const observer = new ResizeObserver(() => {
      if (animationFrame) return
      animationFrame = requestAnimationFrame(() => {
        animationFrame = 0
        chartRef.current?.onResize()
      })
    })
    observer.observe(container)
    return () => {
      observer.disconnect()
      cancelAnimationFrame(animationFrame)
    }
  }, [])

  return <div className="absolute inset-0" ref={containerRef}><div ref={hostRef} className="absolute inset-0" /><div ref={tooltipRef} style={TOOLTIP_STYLE} /></div>
}
