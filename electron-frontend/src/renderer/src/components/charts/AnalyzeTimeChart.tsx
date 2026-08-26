import { useEffect, useRef, type MutableRefObject } from 'react'
import { ChartTooltipPortal, useChartTooltip } from '../../hooks/useChartTooltip'
import { ANALYZE_METRICS, ANALYZE_METRIC_BY_ID, type AnalyzeSeriesConfig, type AnalyzeSource } from '../../lib/analyzeMetrics'
import { createAxisPlugin, type AxisConfig } from '../../lib/timechart/axisPlugin'
import { TimeChart, corePlugins, type TChart } from '../../lib/timechart/tc'
import { AlignedDataBuffer, type SeriesData } from '../../lib/timechart/engine/core/alignedData'
import { buildLapProgressMap, findSectorSplits, interpolateLapElapsed, type LapProgressMap, type SectorSplit } from '../../lib/lapDelta'
import { formatChartDeltaTooltip } from '../../lib/chartDeltaTooltip'
import { themeSeriesColor } from '../../lib/themeColors'
import { getPlaybackCursorTime, subscribePlaybackCursor } from '../../lib/playbackCursor'
import type { AnalyzeLapData } from '../../types'

export interface AnalyzeTimeChartProps {
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
  realtimeCurrent: boolean
  controlsRef: MutableRefObject<AnalyzeChartControls | null>
  onInspectMap?: (elapsedSeconds: number) => void
  /** Restrict storage and GPU series to these metrics (used by stacked charts). */
  metricScope?: readonly string[]
  /** Compact panels reserve x-axis labels for the final chart only. */
  showXAxis?: boolean
  /** Let a parent synchronize pointer interactions across multiple panels. */
  interactionEnabled?: boolean
  /** Render selected metrics as vertical viewports on one WebGL canvas. */
  stackedMode?: boolean
  /** Hidden persistent chart modes must not leave their portaled tooltip visible. */
  tooltipEnabled?: boolean
  /** Show values from every stacked panel instead of only the hovered panel. */
  syncedTooltip?: boolean
  sectorBoundaries?: boolean
  sectorDelta?: boolean
}

export interface AnalyzeChartControls {
  zoomIn: () => void
  zoomOut: () => void
  panLeft: () => void
  panRight: () => void
  reset: () => void
  zoomByFactor: (factor: number, anchorRatio?: number) => void
  panByFraction: (fraction: number) => void
}

type Role = 'comparison' | 'current'
type SourceBuffers = Partial<Record<AnalyzeSource, AlignedDataBuffer>>
type Buffers = Record<Role, SourceBuffers> & {
  deltaPositive: AlignedDataBuffer
  deltaNegative: AlignedDataBuffer
}
type SeriesRecord = Record<Role, Map<string, any>>

const SOURCES: AnalyzeSource[] = ['telemetry', 'motion', 'motionEx', 'status', 'damage']
const Y_TICKS = [0, 0.25, 0.5, 0.75, 1]
const TOP_PADDING = 16
const STACKED_TOP_PADDING = 6
const STACKED_PANEL_GAP = 10
const MIN_ZOOM_SECONDS = 0.5
const MIN_ZOOM_METRES = 25
const DELTA_GRID_METRES = 5

function rowsFor(lap: AnalyzeLapData, source: AnalyzeSource): any[] {
  if (source === 'status') return lap.statusHistory
  if (source === 'damage') return lap.damageHistory
  return lap[source]
}

function makeBuffers(
  sources: readonly AnalyzeSource[],
  metricsBySource: Record<AnalyzeSource, typeof ANALYZE_METRICS>,
): Buffers {
  const makeRole = () => Object.fromEntries(sources.map(source => [source, new AlignedDataBuffer(metricsBySource[source].length)])) as SourceBuffers
  return {
    comparison: makeRole(), current: makeRole(),
    deltaPositive: new AlignedDataBuffer(1),
    deltaNegative: new AlignedDataBuffer(1),
  }
}

function interpolateDistance(progress: LapProgressMap, sessionTime: number): number {
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

function nearestIndex(data: SeriesData, x: number): number {
  if (data.length === 0) return -1
  const after = data.lowerBoundX(x)
  if (after === 0) return 0
  if (after === data.length) return data.length - 1
  return x - data.xAt(after - 1) <= data.xAt(after) - x ? after - 1 : after
}

function syncSource(
  buffer: AlignedDataBuffer,
  rows: any[],
  defs: typeof ANALYZE_METRICS,
  origin: number,
  rebuild: boolean,
  scratch: Float64Array,
  maxSessionTime: number,
): boolean {
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
  if (buffer.length && lo > 0 && rows[lo - 1].session_time <= maxSessionTime && rows[lo - 1].session_time - origin === lastX) {
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
    if (row.session_time > maxSessionTime) break
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
  defs: typeof ANALYZE_METRICS,
  progress: LapProgressMap | null,
  rebuild: boolean,
  scratch: Float64Array,
  cursor: { value: number },
  maxSessionTime: number,
): boolean {
  if (rebuild) { buffer.clear(); cursor.value = -Infinity }
  if (!progress || rows.length === 0) return rebuild
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
    if (row.session_time > progress.maxSessionTime || row.session_time > maxSessionTime) break
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
    if (!Number.isFinite(delta)) {
      positiveValues.push(NaN)
      negativeValues.push(NaN)
      previous = null
      continue
    }
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
  current: LapProgressMap | null,
  comparison: LapProgressMap | null,
  state: DeltaSampleState,
  currentMaxDistance: number,
  sectorDelta: boolean,
  sectorStarts: readonly number[],
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
  const maxDistance = Math.min(current.maxDistance, comparison.maxDistance, currentMaxDistance)
  if (maxDistance <= 0) return { range: state.renderedRange || 0.5, changed: false }
  let lastSampleDistance = -DELTA_GRID_METRES
  for (let index = state.samples.length - 1; index >= 0; index--) {
    if (!Number.isFinite(state.samples[index][1])) continue
    lastSampleDistance = state.samples[index][0]
    break
  }
  let lastStoredX = state.samples.length ? state.samples[state.samples.length - 1][0] : -Infinity
  const startDistance = lastSampleDistance + DELTA_GRID_METRES
  for (let distance = startDistance; distance <= maxDistance; distance += DELTA_GRID_METRES) {
    const currentElapsed = interpolateLapElapsed(current, distance)
    const comparisonElapsed = interpolateLapElapsed(comparison, distance)
    let currentBase = 0
    let comparisonBase = 0
    if (sectorDelta) {
      let sectorStart = 0
      for (const boundary of sectorStarts) {
        if (boundary > distance) break
        sectorStart = boundary
      }
      if (sectorStart > 0) {
        currentBase = interpolateLapElapsed(current, sectorStart)
        comparisonBase = interpolateLapElapsed(comparison, sectorStart)
      }
    }
    const delta = (currentElapsed - currentBase) - (comparisonElapsed - comparisonBase)
    if (!Number.isFinite(delta)) continue
    if (sectorDelta) {
      for (const boundary of sectorStarts) {
        if (boundary > lastStoredX && boundary <= distance) {
          let previousSectorStart = 0
          for (const priorBoundary of sectorStarts) {
            if (priorBoundary >= boundary) break
            previousSectorStart = priorBoundary
          }
          const boundaryCurrentElapsed = interpolateLapElapsed(current, boundary)
          const boundaryComparisonElapsed = interpolateLapElapsed(comparison, boundary)
          const previousCurrentBase = previousSectorStart > 0
            ? interpolateLapElapsed(current, previousSectorStart)
            : 0
          const previousComparisonBase = previousSectorStart > 0
            ? interpolateLapElapsed(comparison, previousSectorStart)
            : 0
          const completedSectorDelta =
            (boundaryCurrentElapsed - previousCurrentBase) -
            (boundaryComparisonElapsed - previousComparisonBase)
          if (Number.isFinite(completedSectorDelta)) {
            // Preserve both values at the boundary while the NaN between them
            // breaks the WebGL strip: completed-sector delta, gap, new-sector 0.
            state.samples.push([boundary, completedSectorDelta])
          }
          state.samples.push([boundary, NaN])
          state.samples.push([boundary, 0])
          lastStoredX = boundary
        }
      }
    }
    state.samples.push([distance, delta])
    lastStoredX = distance
  }
  const maxAbs = state.samples.reduce((max, sample) =>
    Number.isFinite(sample[1]) ? Math.max(max, Math.abs(sample[1])) : max, 0)
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
    if (!Number.isFinite(delta)) {
      positiveY[0] = negativeY[0] = NaN
      positive.append(distance, positiveY)
      negative.append(distance, negativeY)
      previous = null
      continue
    }
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
  const bg = isDark ? [0x12, 0x14, 0x1f] : [0xeb, 0xea, 0xe6]
  const rgb = [(value >> 16) & 255, (value >> 8) & 255, value & 255]
  return `#${rgb.map((channel, i) => Math.round(channel * 0.35 + bg[i] * 0.65).toString(16).padStart(2, '0')).join('')}`
}

function fmtLapTime(seconds: number): string {
  return `${Math.floor(seconds / 60)}:${(seconds % 60).toFixed(1).padStart(4, '0')}`
}

function fmtDistance(metres: number): string {
  return `${Math.round(metres)} m`
}

function resolvedSectorSplits(primary: AnalyzeLapData, comparison: AnalyzeLapData | null): SectorSplit[] {
  const bySector = new Map(findSectorSplits(comparison).map(split => [split.afterSector, split]))
  for (const split of findSectorSplits(primary)) bySector.set(split.afterSector, split)
  return ([1, 2] as const).flatMap(sector => {
    const split = bySector.get(sector)
    return split ? [split] : []
  })
}

export default function AnalyzeTimeChart({
  isDark, current, currentRevision, comparison, selected, primaryLabel, comparisonLabel,
  distanceMode, trackLengthM, deltaPositiveColor, deltaNegativeColor,
  zoomEnabled, realtimeCurrent, controlsRef, onInspectMap,
  metricScope, showXAxis = true, interactionEnabled = true, stackedMode = false,
  tooltipEnabled = true, syncedTooltip = false, sectorBoundaries = false, sectorDelta = false,
}: AnalyzeTimeChartProps) {
  // Series topology is fixed for the lifetime of this chart. Stacked mode
  // includes every metric as channels on one shared WebGL canvas.
  const scopedMetricsRef = useRef(metricScope
    ? ANALYZE_METRICS.filter(def => metricScope.includes(def.id))
    : ANALYZE_METRICS)
  const metricsBySourceRef = useRef(Object.fromEntries(SOURCES.map(source => [
    source,
    scopedMetricsRef.current.filter(metric => metric.source === source),
  ])) as Record<AnalyzeSource, typeof ANALYZE_METRICS>)
  const activeSourcesRef = useRef(SOURCES.filter(source => metricsBySourceRef.current[source].length > 0))
  const themedDeltaPositive = themeSeriesColor(deltaPositiveColor, isDark)
  const themedDeltaNegative = themeSeriesColor(deltaNegativeColor, isDark)
  const containerRef = useRef<HTMLDivElement>(null)
  const { tooltipRef, show, hide } = useChartTooltip(containerRef)
  const hostRef = useRef<HTMLDivElement>(null)
  const chartRef = useRef<TChart | null>(null)
  const buffersRef = useRef<Buffers | null>(null)
  const seriesRef = useRef<SeriesRecord | null>(null)
  const axisCfgRef = useRef<{ current: AxisConfig } | null>(null)
  const deltaSeriesRef = useRef<{ positive: any; negative: any } | null>(null)
  const deltaRangeRef = useRef(0.5)
  const deltaSamplesRef = useRef<DeltaSampleState>({ revision: '', samples: [], renderedCount: 0, renderedRange: 0 })
  const deltaColorsRef = useRef({ positive: themedDeltaPositive, negative: themedDeltaNegative })
  const selectedRef = useRef(selected)
  const isDarkRef = useRef(isDark)
  const currentRef = useRef(current)
  const comparisonRef = useRef(comparison)
  const primaryLabelRef = useRef(primaryLabel)
  const comparisonLabelRef = useRef(comparisonLabel)
  const zoomEnabledRef = useRef(zoomEnabled)
  const interactionEnabledRef = useRef(interactionEnabled)
  const tooltipEnabledRef = useRef(tooltipEnabled)
  const syncedTooltipRef = useRef(syncedTooltip)
  const distanceModeRef = useRef(distanceMode)
  const onInspectMapRef = useRef(onInspectMap)
  const fullXRangeRef = useRef({ min: 0, max: 1 })
  const revisionsRef = useRef<Record<string, string>>({})
  const originsRef = useRef<Record<string, number>>({})
  const distanceCursorsRef = useRef<Record<string, { value: number }>>({})
  const lastRealtimeCutoffRef = useRef(-Infinity)
  const syncPlaybackCursorRef = useRef<(() => void) | null>(null)
  const scratchRef = useRef<Partial<Record<AnalyzeSource, Float64Array>>>(Object.fromEntries(activeSourcesRef.current.map(source => [source, new Float64Array(metricsBySourceRef.current[source].length)])))
  selectedRef.current = selected
  isDarkRef.current = isDark
  currentRef.current = current
  comparisonRef.current = comparison
  primaryLabelRef.current = primaryLabel
  comparisonLabelRef.current = comparisonLabel
  zoomEnabledRef.current = zoomEnabled
  interactionEnabledRef.current = interactionEnabled
  tooltipEnabledRef.current = tooltipEnabled
  syncedTooltipRef.current = syncedTooltip
  distanceModeRef.current = distanceMode
  onInspectMapRef.current = onInspectMap
  deltaColorsRef.current = { positive: themedDeltaPositive, negative: themedDeltaNegative }

  useEffect(() => {
    const host = hostRef.current
    if (!host) return
    const scopedMetrics = scopedMetricsRef.current
    const metricsBySource = metricsBySourceRef.current
    const buffers = makeBuffers(activeSourcesRef.current, metricsBySource)
    buffersRef.current = buffers
    const axisCfg = { current: {
      axisColor: '#7c8098', gridColor: 'rgba(255,255,255,0.04)', borderColor: '#1e2136',
      font: '10px "Cascadia Code", ui-monospace, monospace', xTickSpacePx: 80,
      xTickFormat: showXAxis ? fmtLapTime : () => '', yTickValues: () => [], yTickFormat: () => '',
      xGap: 2, yGap: 4, showYGrid: true, extraYAxes: [],
    } satisfies AxisConfig }
    axisCfgRef.current = axisCfg
    const rawSeries: any[] = []
    for (const role of ['comparison', 'current'] as Role[]) {
      for (const def of scopedMetrics) {
        const channel = metricsBySource[def.source].findIndex(candidate => candidate.id === def.id)
        rawSeries.push({
          name: `${role}:${def.id}`, color: themeSeriesColor(def.defaultColor, isDark), visible: false,
          lineWidth: role === 'comparison' ? 1.25 : 1.75,
          lineType: def.lineType === 'step' ? TimeChart.LineType.Step : TimeChart.LineType.Line,
          stepLocation: def.lineType === 'step' ? 1 : 0,
          data: buffers[role][def.source]!.series[channel],
        })
      }
    }
    const deltaSeries = {
      positive: {
        name: 'delta:positive', color: themedDeltaPositive, visible: false, lineWidth: 2,
        lineType: TimeChart.LineType.Line, data: buffers.deltaPositive.series[0], viewport: undefined,
      },
      negative: {
        name: 'delta:negative', color: themedDeltaNegative, visible: false, lineWidth: 2,
        lineType: TimeChart.LineType.Line, data: buffers.deltaNegative.series[0], viewport: undefined,
      },
    }
    rawSeries.push(deltaSeries.positive, deltaSeries.negative)
    deltaSeriesRef.current = deltaSeries
    const paddingTop = stackedMode ? STACKED_TOP_PADDING : TOP_PADDING
    const paddingLeft = stackedMode ? 48 : 12
    const paddingBottom = showXAxis ? 24 : 8
    const chart = new TimeChart.core(host, {
      paddingTop, paddingRight: 12, paddingBottom, paddingLeft,
      renderPaddingTop: paddingTop, renderPaddingRight: 12, renderPaddingBottom: paddingBottom, renderPaddingLeft: paddingLeft,
      yRange: { min: 0, max: 1 }, lineWidth: 1.5, series: rawSeries,
      plugins: {
        lineChart: corePlugins.lineChart, crosshair: corePlugins.crosshair,
        nearestPoint: corePlugins.nearestPoint,
        axis: createAxisPlugin(axisCfg),
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
      zoomByFactor: (factor, anchorRatio = 0.5) => {
        const [min, max] = chart.model.xScale.domain().map(Number)
        zoomBy(factor, min + (max - min) * Math.max(0, Math.min(1, anchorRatio)))
      },
      panByFraction: panBy,
    }
    controlsRef.current = controls

    const interactionNode = chart.contentBoxDetector.node
    let dragPointer: number | null = null
    let dragX = 0
    let dragDomain: [number, number] = [0, 1]
    let lastRightClick = { at: -Infinity, x: 0, y: 0 }
    const onWheel = (event: WheelEvent) => {
      if (!interactionEnabledRef.current || !zoomEnabledRef.current) return
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
      if (event.button === 2 && onInspectMapRef.current) {
        event.preventDefault()
        const now = performance.now()
        const isDouble = now - lastRightClick.at <= 500 &&
          Math.hypot(event.clientX - lastRightClick.x, event.clientY - lastRightClick.y) <= 12
        if (!isDouble) {
          lastRightClick = { at: now, x: event.clientX, y: event.clientY }
          return
        }
        lastRightClick.at = -Infinity
        const rect = interactionNode.getBoundingClientRect()
        const contentX = Math.max(0, Math.min(rect.width, event.clientX - rect.left))
        const chartX = (chart.model.xScale as any).invert(contentX + chart.options.paddingLeft) as number
        const lap = currentRef.current
        const elapsed = distanceModeRef.current
          ? (() => {
              const progress = buildLapProgressMap(lap)
              return progress ? interpolateLapElapsed(progress, chartX) : NaN
            })()
          : chartX
        if (Number.isFinite(elapsed)) {
          const duration = Math.max(0, lap.endSessionTime - lap.startSessionTime)
          onInspectMapRef.current(Math.max(0, Math.min(duration, elapsed)))
        }
        return
      }
      if (!interactionEnabledRef.current || !zoomEnabledRef.current || event.button !== 0) return
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
      interactionNode.style.cursor = interactionEnabledRef.current && zoomEnabledRef.current ? 'grab' : ''
    }
    const preventContextMenu = (event: MouseEvent) => {
      if (onInspectMapRef.current) event.preventDefault()
    }
    const onDoubleClick = (event: MouseEvent) => {
      if (event.button === 0) resetZoom()
    }
    interactionNode.addEventListener('wheel', onWheel, { passive: false })
    interactionNode.addEventListener('pointerdown', onPointerDown)
    interactionNode.addEventListener('pointermove', onPointerMove)
    interactionNode.addEventListener('pointerup', stopDrag)
    interactionNode.addEventListener('pointercancel', stopDrag)
    interactionNode.addEventListener('contextmenu', preventContextMenu)
    interactionNode.addEventListener('dblclick', onDoubleClick)
    const records: SeriesRecord = { comparison: new Map(), current: new Map() }
    for (const option of chart.options.series) {
      if (option.name.startsWith('delta:')) continue
      const [role, id] = option.name.split(':') as [Role, string]
      records[role].set(id, option)
    }
    seriesRef.current = records

    const move = (contentX: number, contentY: number) => {
      if (!tooltipEnabledRef.current) { hide(); return }
      const x = (chart.model.xScale as any).invert(contentX + chart.options.paddingLeft) as number
      const rows: string[] = [`<div style="color:var(--text-secondary);margin-bottom:4px">${distanceModeRef.current ? fmtDistance(x) : fmtLapTime(x)}</div>`]
      let hasValue = false
      let hoveredMetricId: string | null = null
      if (stackedMode) {
        const panels = selectedRef.current.filter(item =>
          item.visible && (item.metricId !== 'delta' || (distanceModeRef.current && comparisonRef.current !== null)),
        )
        const contentHeight = chart.clientHeight - chart.options.paddingTop - chart.options.paddingBottom
        const panelIndex = Math.floor(contentY / contentHeight * panels.length)
        hoveredMetricId = panels[Math.max(0, Math.min(panels.length - 1, panelIndex))]?.metricId ?? null
      }
      const appendRole = (role: Role, lap: AnalyzeLapData | null, heading: string) => {
        if (!lap) return
        const roleRows: string[] = []
        let roleHasValue = false
        for (const item of selectedRef.current) {
          if (!item.visible) continue
          if (hoveredMetricId && !syncedTooltipRef.current && item.metricId !== hoveredMetricId) continue
          const def = ANALYZE_METRIC_BY_ID.get(item.metricId)
          const option = seriesRef.current?.[role].get(item.metricId)
          if (!def || !option) continue
          const index = nearestIndex(option.data, x)
          const normalized = index >= 0 ? option.data.yAt(index) : NaN
          const value = def.min + normalized * (def.max - def.min)
          if (Number.isFinite(value)) roleHasValue = true
          const display = Number.isFinite(value) ? def.format(value) : '—'
          const color = role === 'comparison' ? blendColor(item.color, isDarkRef.current) : item.color
          roleRows.push(`<div><span style="color:${color}">${def.label}</span>: ${display}</div>`)
        }
        if (roleHasValue) {
          rows.push(`<div style="color:var(--text-secondary);font-size:10px;margin:${rows.length > 1 ? '5px' : '0'} 0 2px">${heading}</div>`, ...roleRows)
          hasValue = true
        }
      }
      appendRole('current', currentRef.current, primaryLabelRef.current ?? `CURRENT L${currentRef.current.lapNum || '—'}`)
      appendRole('comparison', comparisonRef.current, comparisonRef.current ? comparisonLabelRef.current ?? `COMPARE L${comparisonRef.current.lapNum}` : '')
      const deltaSeries = deltaSeriesRef.current
      if (distanceModeRef.current && deltaSeries?.positive.visible &&
        (!hoveredMetricId || syncedTooltipRef.current || hoveredMetricId === 'delta')) {
        const index = nearestIndex(deltaSeries.positive.data, x)
        const positive = index >= 0 ? deltaSeries.positive.data.yAt(index) : NaN
        const negative = index >= 0 ? deltaSeries.negative.data.yAt(index) : NaN
        const normalized = Number.isFinite(positive) ? positive : negative
        const delta = (normalized - 0.5) * 2 * deltaRangeRef.current
        if (Number.isFinite(delta)) {
          rows.push(formatChartDeltaTooltip(delta, deltaColorsRef.current.positive, deltaColorsRef.current.negative))
          hasValue = true
        }
      }
      if (!hasValue) { hide(); return }
      show(rows.join(''), contentX + chart.options.paddingLeft, contentY + chart.options.paddingTop + 4)
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
      interactionNode.removeEventListener('contextmenu', preventContextMenu)
      interactionNode.removeEventListener('dblclick', onDoubleClick)
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
    for (const def of scopedMetricsRef.current) {
      const item = selectedDefs.find(candidate => candidate.def.id === def.id)?.item
      const visible = visibleIds.has(def.id)
      const currentOption = records.current.get(def.id)
      const comparisonOption = records.comparison.get(def.id)
      const color = themeSeriesColor(item?.color ?? def.defaultColor, isDark)
      if (currentOption) { currentOption.visible = visible; currentOption.color = color }
      if (comparisonOption) { comparisonOption.visible = visible && !!comparison; comparisonOption.color = blendColor(color, isDark) }
    }
    // WebGL paints later series over earlier ones, so reverse the sidebar order:
    // the first row in the sidebar is drawn last and remains visually on top.
    const deltaItem = selected.find(item => item.metricId === 'delta')
    const showDelta = !!deltaItem && deltaItem.visible !== false && distanceMode && !!comparison
    const deltaSeries = deltaSeriesRef.current
    if (deltaSeries) {
      deltaSeries.positive.visible = showDelta
      deltaSeries.positive.color = themedDeltaPositive
      deltaSeries.negative.visible = showDelta
      deltaSeries.negative.color = themedDeltaNegative
    }
    const panelItems = selected.filter(item =>
      item.visible && (item.metricId !== 'delta' || showDelta),
    )
    const splitValues = sectorBoundaries
      ? resolvedSectorSplits(current, comparison).map(split => distanceMode ? split.distance : split.elapsedSeconds)
      : []
    const fullLapEnd = distanceMode
      ? trackLengthM > 0
        ? trackLengthM
        : Math.max(
            current.lapProgress.at(-1)?.lap_distance_m ?? 0,
            comparison?.lapProgress.at(-1)?.lap_distance_m ?? 0,
          )
      : Math.max(
          current.endSessionTime - current.startSessionTime,
          comparison ? comparison.endSessionTime - comparison.startSessionTime : 0,
        )
    const sectorEnds = [...splitValues, fullLapEnd]
    const xTickValues = sectorBoundaries
      ? (min: number, max: number) => {
          const values = [min, ...splitValues.filter(value => value > min && value < max), max]
          return values.filter((value, index) => index === 0 || Math.abs(value - values[index - 1]) > 1e-6)
        }
      : undefined
    const xTickFormat = !showXAxis
      ? () => ''
      : sectorBoundaries
        ? (value: number) => {
            const sectorIndex = sectorEnds.findIndex(end => Math.abs(end - value) < 1e-3)
            return sectorIndex >= 0 ? `S${sectorIndex + 1}` : ''
          }
        : distanceMode ? fmtDistance : fmtLapTime
    for (const option of chart.options.series) option.viewport = undefined
    if (stackedMode) {
      panelItems.forEach((item, index) => {
        const viewport = {
          top: index / panelItems.length,
          bottom: (index + 1) / panelItems.length,
          gapAfter: index < panelItems.length - 1 ? STACKED_PANEL_GAP : 0,
        }
        if (item.metricId === 'delta') {
          if (deltaSeries) {
            deltaSeries.positive.viewport = viewport
            deltaSeries.negative.viewport = viewport
          }
          return
        }
        const currentOption = records.current.get(item.metricId)
        const comparisonOption = records.comparison.get(item.metricId)
        if (currentOption) currentOption.viewport = viewport
        if (comparisonOption) comparisonOption.viewport = viewport
      })
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

    const axis = isDark ? '#7c8098' : '#596168'
    if (stackedMode) {
      const left = panelItems.some(item => item.showYAxis) ? 48 : 12
      axisHolder.current = {
        axisColor: axis,
        gridColor: isDark ? 'rgba(255,255,255,0.04)' : 'rgba(0,0,0,0.07)',
        borderColor: isDark ? '#1e2136' : '#afb1ae',
        font: '10px "Cascadia Code", ui-monospace, monospace',
        xTickSpacePx: 80,
        xTickFormat,
        xTickValues,
        yTickValues: () => [],
        yTickFormat: () => '',
        xGap: 2,
        yGap: 4,
        showYGrid: true,
        panels: panelItems.map((item, index) => {
          const def = ANALYZE_METRIC_BY_ID.get(item.metricId)
          const isDelta = item.metricId === 'delta'
          return {
            top: index / panelItems.length,
            bottom: (index + 1) / panelItems.length,
            showYAxis: item.showYAxis,
            yAxisColor: isDelta ? themedDeltaPositive : themeSeriesColor(item.color, isDark),
            yTickValues: Y_TICKS,
            yTickColor: isDelta
              ? (normalized: number) => normalized < 0.5 ? themedDeltaNegative : normalized > 0.5 ? themedDeltaPositive : axis
              : undefined,
            yTickFormat: (normalized: number) => isDelta
              ? `${((normalized - 0.5) * 2 * deltaRangeRef.current).toFixed(1)}s`
              : def ? def.axisFormat(def.min + normalized * (def.max - def.min)) : '',
            gapAfter: index < panelItems.length - 1 ? STACKED_PANEL_GAP : 0,
          }
        }),
      }
      const paddingBottom = showXAxis ? 24 : 8
      Object.assign(chart.options, {
        paddingLeft: left, paddingRight: 12, paddingBottom,
        renderPaddingLeft: left, renderPaddingRight: 12, renderPaddingBottom: paddingBottom,
      })
      chart.contentBoxDetector.setPadding(left, 12, chart.options.paddingTop, paddingBottom)
      hostRef.current?.style.setProperty('--background-overlay', isDark ? '#12141f' : '#f1f0ec')
      if (hostRef.current) hostRef.current.style.color = axis
      chart.update()
      chart.model.resize(chart.clientWidth, chart.clientHeight)
      return
    }

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
    const first = axes[0]
    const extraYAxes = axes.slice(1).map((entry, index) => {
      const axisIndex = index + 1
      const side = axisIndex % 2 === 1 ? 'right' as const : 'left' as const
      const slot = Math.floor(axisIndex / 2)
      return {
        side, offset: 4 + slot * 54,
        color: entry.kind === 'delta' ? themedDeltaPositive : themeSeriesColor(entry.item.color, isDark),
        colorForValue: entry.kind === 'delta'
          ? (normalized: number) => normalized < 0.5 ? themedDeltaNegative : normalized > 0.5 ? themedDeltaPositive : axis
          : undefined,
        values: Y_TICKS,
        format: (normalized: number) => entry.kind === 'delta'
          ? `${((normalized - 0.5) * 2 * deltaRangeRef.current).toFixed(1)}s`
          : entry.def.axisFormat(entry.def.min + normalized * (entry.def.max - entry.def.min)),
      }
    })
    axisHolder.current = {
      axisColor: axis,
      yAxisColor: first?.kind === 'delta' ? themedDeltaPositive : themeSeriesColor(first?.item.color ?? axis, isDark),
      gridColor: isDark ? 'rgba(255,255,255,0.04)' : 'rgba(0,0,0,0.07)',
      borderColor: isDark ? '#1e2136' : '#afb1ae',
      font: '10px "Cascadia Code", ui-monospace, monospace',
      xTickSpacePx: 80, xTickFormat,
      xTickValues,
      yTickValues: () => first ? Y_TICKS : [],
      yTickColor: first?.kind === 'delta'
        ? normalized => normalized < 0.5 ? themedDeltaNegative : normalized > 0.5 ? themedDeltaPositive : axis
        : undefined,
      yTickFormat: normalized => first?.kind === 'delta'
        ? `${((normalized - 0.5) * 2 * deltaRangeRef.current).toFixed(1)}s`
        : first?.kind === 'metric' ? first.def.axisFormat(first.def.min + normalized * (first.def.max - first.def.min)) : '',
      xGap: 2, yGap: 4, showYGrid: axisCount > 0,
      extraYAxes,
    }
    const paddingBottom = showXAxis ? 24 : 8
    Object.assign(chart.options, {
      paddingLeft: left, paddingRight: right, paddingBottom,
      renderPaddingLeft: left, renderPaddingRight: right, renderPaddingBottom: paddingBottom,
    })
    chart.contentBoxDetector.setPadding(left, right, chart.options.paddingTop, paddingBottom)
    hostRef.current?.style.setProperty('--background-overlay', isDark ? '#12141f' : '#f1f0ec')
    if (hostRef.current) hostRef.current.style.color = axis
    chart.update()
    chart.model.resize(chart.clientWidth, chart.clientHeight)
  }, [comparison, current, distanceMode, isDark, sectorBoundaries, selected, showXAxis, themedDeltaNegative, themedDeltaPositive])

  useEffect(() => {
    let animationFrame = 0
    const unsubscribe = subscribePlaybackCursor(() => {
      if (!realtimeCurrent || animationFrame) return
      animationFrame = requestAnimationFrame(() => {
        animationFrame = 0
        syncPlaybackCursorRef.current?.()
      })
    })
    return () => {
      unsubscribe()
      if (animationFrame) cancelAnimationFrame(animationFrame)
    }
  }, [realtimeCurrent])

  useEffect(() => {
    const chart = chartRef.current
    const buffers = buffersRef.current
    if (!chart || !buffers) return
    // The indexed lap is immutable. Build its distance lookup once when the
    // cache/revision changes, not on every playback-cursor animation frame.
    const progressByRole: Record<Role, LapProgressMap | null> = {
      current: distanceMode ? buildLapProgressMap(current) : null,
      comparison: distanceMode ? buildLapProgressMap(comparison) : null,
    }
    const useSectorDelta = sectorBoundaries && sectorDelta
    const sectorStarts = useSectorDelta ? resolvedSectorSplits(current, comparison).map(split => split.distance) : []
    const syncData = () => {
      const currentCutoff = realtimeCurrent ? getPlaybackCursorTime() ?? -Infinity : Infinity
      const cursorRewound = realtimeCurrent &&
        Number.isFinite(currentCutoff) &&
        Number.isFinite(lastRealtimeCutoffRef.current) &&
        currentCutoff < lastRealtimeCutoffRef.current
      lastRealtimeCutoffRef.current = realtimeCurrent ? currentCutoff : -Infinity
      if (cursorRewound) {
        // Cursor notifications can beat the seek-flush revision by one frame.
        // Force the current buffers to rebuild immediately so future samples
        // from the old cursor are never left visible during that gap.
        for (const source of activeSourcesRef.current) revisionsRef.current[`current:${source}`] = ''
        deltaSamplesRef.current = { revision: '', samples: [], renderedCount: 0, renderedRange: 0 }
      }
      let changed = false
      for (const role of ['current', 'comparison'] as Role[]) {
        const lap = role === 'current' ? current : comparison
        const maxSessionTime = role === 'current' ? currentCutoff : Infinity
        for (const source of activeSourcesRef.current) {
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
          const buffer = buffers[role][source]!
          const defs = metricsBySourceRef.current[source]
          if (distanceMode) {
            const cursor = distanceCursorsRef.current[revisionKey] ??= { value: -Infinity }
            if (syncSourceDistance(
              buffer, rows, defs, progressByRole[role], rebuild,
              scratchRef.current[source]!, cursor, maxSessionTime,
            )) changed = true
            continue
          }
          if (rows.length === 0) {
            if (syncSource(buffer, rows, defs, 0, rebuild, scratchRef.current[source]!, maxSessionTime)) changed = true
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
          if (syncSource(buffer, rows, defs, origin, rebuild, scratchRef.current[source]!, maxSessionTime)) changed = true
        }
      }
      if (distanceMode) {
        const deltaRevision = `${currentRevision}:${comparison?.lapNum ?? 0}:${comparison?.startSessionTime ?? 0}:${useSectorDelta ? sectorStarts.join(',') : 'lap'}`
        if (deltaSamplesRef.current.revision !== deltaRevision) {
          deltaSamplesRef.current = { revision: deltaRevision, samples: [], renderedCount: 0, renderedRange: 0 }
        }
        const currentProgress = progressByRole.current
        const cursorDistance = realtimeCurrent && currentProgress
          ? interpolateDistance(currentProgress, currentCutoff)
          : Infinity
        const currentMaxDistance = realtimeCurrent
          ? Number.isFinite(cursorDistance) ? cursorDistance : 0
          : Infinity
        const deltaResult = syncDelta(
          buffers.deltaPositive, buffers.deltaNegative,
          currentProgress, progressByRole.comparison,
          deltaSamplesRef.current,
          currentMaxDistance,
          useSectorDelta,
          sectorStarts,
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
        const lap = role === 'current' ? current : comparison
        const cutoff = role === 'current' ? currentCutoff : Infinity
        // Derive a common domain from every loaded source, even when this
        // compact panel buffers only one metric. All stacked panels therefore
        // remain pixel-aligned despite different packet cadences.
        if (!distanceMode && lap) {
          for (const source of SOURCES) {
            const rows = rowsFor(lap, source)
            if (!rows.length) continue
            max = Math.max(max, Math.min(rows[rows.length - 1].session_time, cutoff) - lap.startSessionTime)
          }
        }
        for (const source of activeSourcesRef.current) {
          const buffer = buffers[role][source]!
          if (buffer.length) max = Math.max(max, buffer.lastX)
        }
      }
      chart.options.xRange = { min: 0, max }
      fullXRangeRef.current = { min: 0, max }
      chart.model.requestRedraw()
    }
    syncPlaybackCursorRef.current = syncData
    syncData()
    return () => {
      if (syncPlaybackCursorRef.current === syncData) syncPlaybackCursorRef.current = null
    }
  }, [comparison, current, currentRevision, distanceMode, realtimeCurrent, sectorBoundaries, sectorDelta, trackLengthM])

  useEffect(() => {
    if (!tooltipEnabled) hide()
  }, [hide, tooltipEnabled])

  useEffect(() => {
    hide()
  }, [hide, syncedTooltip])

  useEffect(() => {
    const node = chartRef.current?.contentBoxDetector.node
    if (node) node.style.cursor = interactionEnabled && zoomEnabled ? 'grab' : ''
    if (!zoomEnabled) controlsRef.current?.reset()
  }, [controlsRef, interactionEnabled, zoomEnabled])

  useEffect(() => {
    const container = containerRef.current
    if (!container) return
    let resizeTimer: ReturnType<typeof setTimeout> | null = null
    const observer = new ResizeObserver(() => {
      if (resizeTimer !== null) clearTimeout(resizeTimer)
      resizeTimer = setTimeout(() => {
        resizeTimer = null
        chartRef.current?.onResize()
      }, 120)
    })
    observer.observe(container)
    return () => {
      observer.disconnect()
      if (resizeTimer !== null) clearTimeout(resizeTimer)
    }
  }, [])

  return <>
    <div className="absolute inset-0" ref={containerRef}><div ref={hostRef} className="absolute inset-0" /></div>
    <ChartTooltipPortal tooltipRef={tooltipRef} />
  </>
}
