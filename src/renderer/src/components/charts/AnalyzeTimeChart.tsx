import { useEffect, useRef, type MutableRefObject } from 'react'
import { useSize } from '../../hooks/useSize'
import { useChartTooltip, TOOLTIP_STYLE } from '../../hooks/useChartTooltip'
import { ANALYZE_METRICS, ANALYZE_METRIC_BY_ID, type AnalyzeSeriesConfig, type AnalyzeSource } from '../../lib/analyzeMetrics'
import { createAxisPlugin, type AxisConfig } from '../../lib/timechart/axisPlugin'
import { TimeChart, corePlugins, type TChart } from '../../lib/timechart/tc'
import { AlignedDataBuffer, type SeriesData } from '../../lib/timechart/engine/core/alignedData'
import type { AnalyzeLapData } from '../../types'

interface Props {
  isDark: boolean
  current: AnalyzeLapData
  currentRevision: string | number
  comparison: AnalyzeLapData | null
  selected: AnalyzeSeriesConfig[]
  showYAxis: boolean
  primaryLabel?: string
  comparisonLabel?: string
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
type Buffers = Record<Role, Record<AnalyzeSource, AlignedDataBuffer>>
type SeriesRecord = Record<Role, Map<string, any>>

const SOURCES: AnalyzeSource[] = ['telemetry', 'motion', 'motionEx', 'status', 'damage']
const Y_TICKS = [0, 0.25, 0.5, 0.75, 1]
const TOP_PADDING = 16
const MIN_ZOOM_SECONDS = 0.5
const METRICS_BY_SOURCE = Object.fromEntries(SOURCES.map(source => [source, ANALYZE_METRICS.filter(metric => metric.source === source)])) as Record<AnalyzeSource, typeof ANALYZE_METRICS>

function rowsFor(lap: AnalyzeLapData, source: AnalyzeSource): any[] {
  if (source === 'status') return lap.statusHistory
  if (source === 'damage') return lap.damageHistory
  return lap[source]
}

function makeBuffers(): Buffers {
  const makeRole = () => Object.fromEntries(SOURCES.map(source => [source, new AlignedDataBuffer(METRICS_BY_SOURCE[source].length)])) as Record<AnalyzeSource, AlignedDataBuffer>
  return { comparison: makeRole(), current: makeRole() }
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

export default function AnalyzeTimeChart({
  isDark, current, currentRevision, comparison, selected, showYAxis, primaryLabel, comparisonLabel,
  zoomEnabled, controlsRef,
}: Props) {
  const { ref: sizeRef, width, height } = useSize()
  const { tooltipRef, show, hide } = useChartTooltip()
  const hostRef = useRef<HTMLDivElement>(null)
  const chartRef = useRef<TChart | null>(null)
  const buffersRef = useRef<Buffers | null>(null)
  const seriesRef = useRef<SeriesRecord | null>(null)
  const axisCfgRef = useRef<{ current: AxisConfig } | null>(null)
  const selectedRef = useRef(selected)
  const isDarkRef = useRef(isDark)
  const currentRef = useRef(current)
  const comparisonRef = useRef(comparison)
  const primaryLabelRef = useRef(primaryLabel)
  const comparisonLabelRef = useRef(comparisonLabel)
  const zoomEnabledRef = useRef(zoomEnabled)
  const fullXRangeRef = useRef({ min: 0, max: 1 })
  const revisionsRef = useRef<Record<string, string>>({})
  const originsRef = useRef<Record<string, number>>({})
  const scratchRef = useRef<Record<AnalyzeSource, Float64Array>>(Object.fromEntries(SOURCES.map(source => [source, new Float64Array(METRICS_BY_SOURCE[source].length)])) as Record<AnalyzeSource, Float64Array>)
  selectedRef.current = selected
  isDarkRef.current = isDark
  currentRef.current = current
  comparisonRef.current = comparison
  primaryLabelRef.current = primaryLabel
  comparisonLabelRef.current = comparisonLabel
  zoomEnabledRef.current = zoomEnabled

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
    const chart = new TimeChart.core(host, {
      paddingTop: TOP_PADDING, paddingRight: 12, paddingBottom: 24, paddingLeft: 12,
      renderPaddingTop: TOP_PADDING, renderPaddingRight: 12, renderPaddingBottom: 24, renderPaddingLeft: 12,
      yRange: { min: 0, max: 1 }, lineWidth: 1.5, series: rawSeries,
      plugins: { lineChart: corePlugins.lineChart, crosshair: corePlugins.crosshair, nearestPoint: corePlugins.nearestPoint, axis: createAxisPlugin(axisCfg) } as any,
    } as any)
    chartRef.current = chart

    const applyXDomain = (requestedMin: number, requestedMax: number) => {
      if (!zoomEnabledRef.current) return
      const full = fullXRangeRef.current
      const fullExtent = Math.max(0, full.max - full.min)
      if (fullExtent <= 0) return
      const minExtent = Math.min(MIN_ZOOM_SECONDS, fullExtent)
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
      const [role, id] = option.name.split(':') as [Role, string]
      records[role].set(id, option)
    }
    seriesRef.current = records

    const move = (contentX: number, contentY: number) => {
      const x = (chart.model.xScale as any).invert(contentX + chart.options.paddingLeft) as number
      const rows: string[] = [`<div style="color:var(--text-secondary);margin-bottom:4px">${fmtLapTime(x)}</div>`]
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
      show(rows.join(''), contentX + chart.options.paddingLeft, contentY + 4, chart.clientWidth, chart.clientHeight)
    }
    const stopMove = chart.contentBoxDetector.moved.on(move)
    const stopLeave = chart.contentBoxDetector.left.on(hide)
    return () => {
      interactionNode.removeEventListener('wheel', onWheel)
      interactionNode.removeEventListener('pointerdown', onPointerDown)
      interactionNode.removeEventListener('pointermove', onPointerMove)
      interactionNode.removeEventListener('pointerup', stopDrag)
      interactionNode.removeEventListener('pointercancel', stopDrag)
      interactionNode.removeEventListener('dblclick', resetZoom)
      if (controlsRef.current === controls) controlsRef.current = null
      stopMove(); stopLeave(); chart.dispose()
      chartRef.current = null; buffersRef.current = null; seriesRef.current = null; axisCfgRef.current = null
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
    // the first metric in the sidebar is drawn last and remains visually on top.
    const drawOrder = [...visibleDefs].reverse()
    const visibleComparison = drawOrder.map(({ def }) => records.comparison.get(def.id)).filter(Boolean)
    const visibleCurrent = drawOrder.map(({ def }) => records.current.get(def.id)).filter(Boolean)
    const hidden = chart.options.series.filter(option => {
      const id = option.name.slice(option.name.indexOf(':') + 1)
      return !visibleIds.has(id)
    })
    // Keep the array identity stable for every plugin/renderer that received it
    // at chart construction while still updating the live GPU draw order.
    chart.options.series.splice(0, chart.options.series.length, ...visibleComparison, ...visibleCurrent, ...hidden)

    const scales = visibleDefs.filter(({ def }, index, all) => all.findIndex(candidate => candidate.def.scaleKey === def.scaleKey) === index)
    const leftCount = showYAxis ? Math.ceil(scales.length / 2) : 0
    const rightCount = showYAxis ? Math.floor(scales.length / 2) : 0
    const left = showYAxis ? Math.max(44, 12 + leftCount * 54) : 12
    const right = showYAxis ? Math.max(12, 12 + rightCount * 54) : 12
    const axis = isDark ? '#7c8098' : '#6b7280'
    const first = scales[0]
    axisHolder.current = {
      axisColor: axis,
      yAxisColor: first?.item.color ?? axis,
      gridColor: isDark ? 'rgba(255,255,255,0.04)' : 'rgba(0,0,0,0.07)',
      borderColor: isDark ? '#1e2136' : '#d0d5e0',
      font: '10px "Cascadia Code", ui-monospace, monospace',
      xTickSpacePx: 80, xTickFormat: fmtLapTime,
      yTickValues: () => showYAxis && first ? Y_TICKS : [],
      yTickFormat: normalized => first ? first.def.axisFormat(first.def.min + normalized * (first.def.max - first.def.min)) : '',
      xGap: 2, yGap: 4, showYGrid: showYAxis,
      extraYAxes: showYAxis ? scales.slice(1).map(({ item, def }, index) => {
        const axisIndex = index + 1
        const side = axisIndex % 2 === 1 ? 'right' as const : 'left' as const
        const slot = Math.floor(axisIndex / 2)
        return {
          side, offset: 4 + slot * 54, color: item.color, values: Y_TICKS,
          format: (normalized: number) => def.axisFormat(def.min + normalized * (def.max - def.min)),
        }
      }) : [],
    }
    Object.assign(chart.options, { paddingLeft: left, paddingRight: right, renderPaddingLeft: left, renderPaddingRight: right })
    chart.contentBoxDetector.setPadding(left, right, chart.options.paddingTop, chart.options.paddingBottom)
    hostRef.current?.style.setProperty('--background-overlay', isDark ? '#12141f' : '#ffffff')
    if (hostRef.current) hostRef.current.style.color = axis
    chart.update()
    chart.model.resize(chart.clientWidth, chart.clientHeight)
  }, [comparison, isDark, selected, showYAxis])

  useEffect(() => {
    const chart = chartRef.current
    const buffers = buffersRef.current
    if (!chart || !buffers) return
    let changed = false
    for (const role of ['current', 'comparison'] as Role[]) {
      const lap = role === 'current' ? current : comparison
      for (const source of SOURCES) {
        // Current-lap rows, lap metadata and the playback-state cursor arrive on
        // separate channels. Keep the origin fixed for the lifetime of a lap;
        // otherwise a one-frame metadata mismatch shifts every X value and
        // alternates the chart between a full lap and a one-point rebuild.
        const revision = lap
          ? role === 'current' ? `${currentRevision}:${source}` : `${lap.lapNum}:${lap.startSessionTime}:${source}`
          : `none:${source}`
        const revisionKey = `${role}:${source}`
        let rebuild = revisionsRef.current[revisionKey] !== revision
        revisionsRef.current[revisionKey] = revision
        if (rebuild) {
          originsRef.current[revisionKey] = lap?.startSessionTime ?? 0
        }
        const rows = lap ? rowsFor(lap, source) : []
        const buffer = buffers[role][source]
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
    if (!changed) return
    let max = 1
    for (const role of ['current', 'comparison'] as Role[]) {
      for (const source of SOURCES) {
        const buffer = buffers[role][source]
        if (buffer.length) max = Math.max(max, buffer.lastX)
      }
    }
    chart.options.xRange = { min: 0, max }
    fullXRangeRef.current = { min: 0, max }
    chart.model.requestRedraw()
  }, [comparison, current, currentRevision])

  useEffect(() => {
    const node = chartRef.current?.contentBoxDetector.node
    if (node) node.style.cursor = zoomEnabled ? 'grab' : ''
    if (!zoomEnabled) controlsRef.current?.reset()
  }, [controlsRef, zoomEnabled])

  useEffect(() => {
    const chart = chartRef.current
    if (chart && width > 0 && height > 0) chart.onResize()
  }, [width, height])

  return <div className="absolute inset-0" ref={sizeRef}><div ref={hostRef} className="absolute inset-0" /><div ref={tooltipRef} style={TOOLTIP_STYLE} /></div>
}
