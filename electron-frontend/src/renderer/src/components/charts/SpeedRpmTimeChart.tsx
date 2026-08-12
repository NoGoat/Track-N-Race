import { useEffect, useRef } from 'react'
import { useSize } from '../../hooks/useSize'
import { useChartTooltip, TOOLTIP_STYLE } from '../../hooks/useChartTooltip'
import { formatChartDeltaTooltip } from '../../lib/chartDeltaTooltip'
import { useTimeChartScroll } from '../../hooks/useTimeChartScroll'
import { TimeChart, corePlugins, type TChart } from '../../lib/timechart/tc'
import { createAxisPlugin, type AxisConfig } from '../../lib/timechart/axisPlugin'
import { AlignedDataBuffer, type SeriesData } from '../../lib/timechart/engine/core/alignedData'
import type { StatusRow, TelemetryRow } from '../../types'
import { useChartCoordinates } from '../../lib/chartCoordinates'
import { playbackDebug } from '../../lib/playbackDebug'
import { subscribeAllLapsData } from '../../stores/telemetryStore'

interface Props {
  isDark: boolean
  telemetry: readonly TelemetryRow[]
  statuses: readonly StatusRow[]
  comparisonTelemetry?: readonly TelemetryRow[]
  comparisonStatuses?: readonly StatusRow[]
  windowSeconds: number
  xTickFormat: (x: number) => string
  tooltipFormat: (x: number, values: number[], comparisonValues?: number[]) => string
}

const SPEED = '#37872D'
const RPM = '#C4162A'
const ERS = '#FADE2A'
const Y_TICKS = [0, 0.25, 0.5, 0.75, 1]

function nearestIndex(data: SeriesData, x: number): number {
  if (data.length === 0) return -1
  const after = data.lowerBoundX(x)
  if (after === 0) return 0
  if (after === data.length) return data.length - 1
  return x - data.xAt(after - 1) <= data.xAt(after) - x ? after - 1 : after
}

function lowerBoundTime(rows: readonly { session_time: number }[], value: number): number {
  let lo = 0, hi = rows.length
  while (lo < hi) {
    const mid = (lo + hi) >> 1
    if (rows[mid].session_time <= value) lo = mid + 1
    else hi = mid
  }
  return lo
}

interface SyncCursor {
  lastSessionTime: number
  statusFirstSessionTime: number
  statusLength: number
}

function syncTelemetry(
  buffer: AlignedDataBuffer,
  telemetry: readonly TelemetryRow[],
  statuses: readonly StatusRow[],
  values: Float64Array,
  getX: (row: TelemetryRow) => number,
  cursor: SyncCursor,
  rebuild: boolean,
): boolean {
  const statusFirstSessionTime = statuses[0]?.session_time ?? Infinity
  // Telemetry and status history can be restored by separate selective V4
  // backfills. If telemetry wins that race, the existing points are initially
  // aligned against no status rows. Revisit them when an older status prefix
  // arrives; ordinary live status appends keep the incremental fast path.
  const statusBackfilled = !rebuild && buffer.length > 0 && statuses.length > 0 && (
    cursor.statusLength === 0 || statusFirstSessionTime < cursor.statusFirstSessionTime
  )
  rebuild ||= statusBackfilled
  if (rebuild) {
    buffer.clear()
    cursor.lastSessionTime = -Infinity
  }
  cursor.statusFirstSessionTime = statusFirstSessionTime
  cursor.statusLength = statuses.length
  if (telemetry.length === 0) {
    return rebuild
  }

  const needsRebuild = !rebuild && buffer.length > 0 && (
    getX(telemetry[telemetry.length - 1]) < buffer.lastX ||
    getX(telemetry[0]) < buffer.firstX
  )
  if (needsRebuild) {
    buffer.clear()
    cursor.lastSessionTime = -Infinity
  }

  const appendStart = lowerBoundTime(telemetry, cursor.lastSessionTime)
  let statusIndex = Math.max(0, lowerBoundTime(statuses, telemetry[appendStart]?.session_time ?? 0) - 1)
  for (let i = appendStart; i < telemetry.length; i++) {
    const row = telemetry[i]
    while (statusIndex + 1 < statuses.length && statuses[statusIndex + 1].session_time <= row.session_time) statusIndex++
    values[0] = row.speed_kph / 380
    values[1] = row.rpm / 16000
    values[2] = (statuses[statusIndex]?.ers_pct ?? 0) / 100
    const x = getX(row)
    if (!Number.isFinite(x)) break
    if (buffer.length && x === buffer.lastX) buffer.replaceLast(values)
    else if (!buffer.length || x > buffer.lastX) buffer.append(x, values)
    cursor.lastSessionTime = row.session_time
  }

  const trim = buffer.lowerBoundX(getX(telemetry[0]))
  if (trim > 0) buffer.evictFront(trim)
  return rebuild || needsRebuild || appendStart < telemetry.length || trim > 0
}

export default function SpeedRpmTimeChart({ isDark, telemetry, statuses, comparisonTelemetry, comparisonStatuses, windowSeconds, xTickFormat, tooltipFormat }: Props) {
  const coordinates = useChartCoordinates()
  const { ref: sizeRef, width, height } = useSize()
  const { tooltipRef, show, hide } = useChartTooltip()
  const hostRef = useRef<HTMLDivElement>(null)
  const chartRef = useRef<TChart | null>(null)
  const axisCfgRef = useRef<{ current: AxisConfig } | null>(null)
  const bufferRef = useRef<AlignedDataBuffer | null>(null)
  const comparisonBufferRef = useRef<AlignedDataBuffer | null>(null)
  const scratchRef = useRef(new Float64Array(3))
  const syncRef = useRef<SyncCursor & { lapRevision: number }>({
    lastSessionTime: -Infinity,
    statusFirstSessionTime: Infinity,
    statusLength: 0,
    lapRevision: coordinates.lapRevision,
  })
  const comparisonSyncRef = useRef<SyncCursor>({
    lastSessionTime: -Infinity,
    statusFirstSessionTime: Infinity,
    statusLength: 0,
  })
  const comparisonLapRef = useRef<number | null>(null)
  const dirtyRef = useRef(false)
  const tooltipFormatRef = useRef(tooltipFormat)
  tooltipFormatRef.current = tooltipFormat
  const xFormatRef = useRef(xTickFormat)
  xFormatRef.current = xTickFormat

  const latestT = telemetry.length ? coordinates.getX(telemetry[telemetry.length - 1]) : null
  const firstT = telemetry.length ? coordinates.getX(telemetry[0]) : null
  const { attach, detach, wake, acceptDataRange } = useTimeChartScroll(!coordinates.distanceMode, latestT, firstT, coordinates.distanceMode ? Math.max(coordinates.trackLengthM, 1) : windowSeconds, dirtyRef, { fastFrames: true, fullFps: coordinates.allLapsMode ? 12 : 60, accumulateFromStart: coordinates.allLapsMode })

  useEffect(() => {
    const host = hostRef.current
    if (!host) return
    const axis = isDark ? '#7c8098' : '#6b7280'
    const buffer = new AlignedDataBuffer(3)
    const comparisonBuffer = new AlignedDataBuffer(3)
    bufferRef.current = buffer
    comparisonBufferRef.current = comparisonBuffer
    const blend = (hex: string) => {
      const value = Number.parseInt(hex.slice(1), 16)
      const bg = isDark ? [0x12, 0x14, 0x1f] : [0xff, 0xff, 0xff]
      const rgb = [(value >> 16) & 255, (value >> 8) & 255, value & 255]
      return `#${rgb.map((channel, i) => Math.round(channel * 0.35 + bg[i] * 0.65).toString(16).padStart(2, '0')).join('')}`
    }
    const axisCfg: { current: AxisConfig } = { current: {
      axisColor: axis,
      yAxisColor: SPEED,
      gridColor: isDark ? 'rgba(255,255,255,0.04)' : 'rgba(0,0,0,0.07)',
      borderColor: isDark ? '#1e2136' : '#d0d5e0',
      font: '11px "Cascadia Code", ui-monospace, monospace',
      xTickSpacePx: 80,
      xTickFormat: x => coordinates.distanceMode || coordinates.allLapsMode ? coordinates.formatX(x) : xFormatRef.current(x),
      xTickValues: coordinates.xTickValues,
      xTickAnchor: coordinates.allLapsMode ? 'start' : 'middle',
      xLabelOffset: coordinates.allLapsMode ? 4 : 0,
      yTickValues: () => Y_TICKS,
      yTickFormat: v => String(Math.round(v * 380)),
      xGap: 2, yGap: 4, showYGrid: true,
      extraYAxes: [
        { side: 'right', offset: 4, color: RPM, values: Y_TICKS, format: v => v === 0 ? '0' : `${Math.round(v * 16)}k` },
        { side: 'right', offset: 54, color: ERS, values: Y_TICKS, format: v => `${Math.round(v * 100)}%` },
      ],
    } }
    axisCfgRef.current = axisCfg
    const chart = new TimeChart.core(host, {
      paddingTop: 4, paddingRight: 100, paddingBottom: 22, paddingLeft: 44,
      renderPaddingTop: 4, renderPaddingRight: 100, renderPaddingBottom: 22, renderPaddingLeft: 44,
      yRange: { min: 0, max: 1 }, lineWidth: 1.5,
      series: [
        { name: 'Previous Speed', color: blend(SPEED), lineWidth: 1, visible: comparisonTelemetry != null, data: comparisonBuffer.series[0] },
        { name: 'Previous RPM', color: blend(RPM), lineWidth: 1, visible: comparisonTelemetry != null, data: comparisonBuffer.series[1] },
        { name: 'Previous ERS', color: blend(ERS), lineWidth: 1, visible: comparisonTelemetry != null, data: comparisonBuffer.series[2] },
        { name: 'Speed', color: SPEED, lineWidth: 1.5, data: buffer.series[0] },
        { name: 'RPM', color: RPM, lineWidth: 1.5, data: buffer.series[1] },
        { name: 'ERS', color: ERS, lineWidth: 1.5, data: buffer.series[2] },
      ],
      plugins: { lineChart: corePlugins.lineChart, crosshair: corePlugins.crosshair, nearestPoint: corePlugins.nearestPoint, axis: createAxisPlugin(axisCfg) } as any,
    } as any)
    chartRef.current = chart
    host.style.color = axis
    host.style.setProperty('--background-overlay', isDark ? '#12141f' : '#ffffff')

    const tooltipValues = [NaN, NaN, NaN]
    const comparisonTooltipValues = [NaN, NaN, NaN]
    const stopTooltipSync = chart.nearestPoint.updated.on(() => {
      const pointer = chart.nearestPoint.lastPointerPos
      if (!pointer) { hide(); return }
      const px = pointer.x - chart.options.paddingLeft + 44
      const x = (chart.model.xScale as any).invert(px) as number
      const index = nearestIndex(chart.options.series[3].data, x)
      if (index < 0) { hide(); return }
      for (let i = 0; i < 3; i++) tooltipValues[i] = chart.options.series[i + 3].data.yAt(index)
      const comparisonData = chart.options.series[0].data
      const comparisonIndex = nearestIndex(comparisonData, x)
      const pointX = comparisonIndex >= 0 ? comparisonData.xAt(comparisonIndex) : chart.options.series[3].data.xAt(index)
      const comparisonValues = comparisonIndex >= 0
        ? comparisonTooltipValues
        : undefined
      if (comparisonValues) {
        for (let i = 0; i < 3; i++) comparisonValues[i] = chart.options.series[i].data.yAt(comparisonIndex)
      }
      const html = tooltipFormatRef.current(pointX, tooltipValues, comparisonValues) + formatChartDeltaTooltip(coordinates.getDeltaAtDistance(pointX))
      show(html, px, pointer.y - chart.options.paddingTop + 4, chart.clientWidth, chart.clientHeight)
    })
    attach(chart)
    return () => {
      stopTooltipSync()
      detach()
      chart.dispose()
      chartRef.current = null
      bufferRef.current = null
      comparisonBufferRef.current = null
      axisCfgRef.current = null
    }
    // Theme changes remount this component; live values flow through refs.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

  useEffect(() => {
    const cfg = axisCfgRef.current
    if (!cfg) return
    cfg.current = {
      ...cfg.current,
      xTickFormat: x => coordinates.distanceMode || coordinates.allLapsMode ? coordinates.formatX(x) : xFormatRef.current(x),
      xTickValues: coordinates.xTickValues,
      xTickAnchor: coordinates.allLapsMode ? 'start' : 'middle',
      xLabelOffset: coordinates.allLapsMode ? 4 : 0,
    }
    chartRef.current?.model.requestRedraw()
  }, [coordinates.allLapsMode, coordinates.axisRevision, coordinates.distanceMode, coordinates.formatX, coordinates.xTickValues])

  useEffect(() => {
    const buffer = comparisonBufferRef.current
    const chart = chartRef.current
    if (!buffer || !chart) return
    const visible = coordinates.comparisonMode && comparisonTelemetry != null
    for (let i = 0; i < 3; i++) chart.options.series[i].visible = visible
    if (!visible) {
      if (buffer.length) buffer.clear()
      comparisonLapRef.current = null
      comparisonSyncRef.current.lastSessionTime = -Infinity
      comparisonSyncRef.current.statusFirstSessionTime = Infinity
      comparisonSyncRef.current.statusLength = 0
      chart.model.requestRedraw()
      return
    }
    const comparisonLap = coordinates.lapData?.lapNum ?? null
    const rebuild = comparisonLapRef.current !== comparisonLap
    if (rebuild) {
      playbackDebug('speed-chart-comparison-rebuild', {
        previousLap: comparisonLapRef.current,
        comparisonLap,
        telemetryRows: comparisonTelemetry.length,
        statusRows: comparisonStatuses?.length ?? 0,
        bufferRowsBeforeClear: buffer.length,
      })
    }
    comparisonLapRef.current = comparisonLap
    syncTelemetry(
      buffer, comparisonTelemetry, comparisonStatuses ?? [], scratchRef.current,
      coordinates.getComparisonX, comparisonSyncRef.current, rebuild,
    )
    chart.model.requestRedraw()
  }, [comparisonStatuses, comparisonTelemetry, coordinates])

  const syncRowsRef = useRef<() => void>(() => {})
  syncRowsRef.current = () => {
    const buffer = bufferRef.current
    if (!buffer) return
    const rebuild = coordinates.distanceMode && syncRef.current.lapRevision !== coordinates.lapRevision
    if (rebuild) {
      playbackDebug('speed-chart-lap-revision', {
        previousRevision: syncRef.current.lapRevision,
        revision: coordinates.lapRevision,
        mode: coordinates.mode,
        telemetryRows: telemetry.length,
        telemetryFirstTime: telemetry[0]?.session_time ?? null,
        telemetryLastTime: telemetry[telemetry.length - 1]?.session_time ?? null,
        firstX: telemetry.length ? coordinates.getX(telemetry[0]) : null,
        lastX: telemetry.length ? coordinates.getX(telemetry[telemetry.length - 1]) : null,
        statusRows: statuses.length,
        bufferRowsBeforeClear: buffer.length,
        cursorSessionTime: syncRef.current.lastSessionTime,
      })
    }
    syncRef.current.lapRevision = coordinates.lapRevision
    if (!syncTelemetry(buffer, telemetry, statuses, scratchRef.current, coordinates.getX, syncRef.current, rebuild)) return
    if (telemetry.length) acceptDataRange(coordinates.getX(telemetry[telemetry.length - 1]), coordinates.getX(telemetry[0]))
    if (rebuild) {
      playbackDebug('speed-chart-lap-revision-synced', {
        revision: coordinates.lapRevision,
        bufferRows: buffer.length,
        bufferFirstX: buffer.length ? buffer.firstX : null,
        bufferLastX: buffer.length ? buffer.lastX : null,
        cursorSessionTime: syncRef.current.lastSessionTime,
      })
    }
    dirtyRef.current = true
    if (coordinates.distanceMode && chartRef.current) {
      const max = coordinates.trackLengthM > 0 ? coordinates.trackLengthM : buffer.lastX
      chartRef.current.options.xRange = { min: 0, max: Math.max(max, 1) }
      chartRef.current.model.requestRedraw()
      dirtyRef.current = false
    } else wake()
  }

  useEffect(() => {
    syncRowsRef.current()
  }, [coordinates, telemetry, statuses, wake])

  useEffect(() => {
    if (!coordinates.allLapsMode) return
    return subscribeAllLapsData(() => syncRowsRef.current())
  }, [coordinates.allLapsMode])

  useEffect(() => {
    if (chartRef.current && width > 0 && height > 0) chartRef.current.onResize()
  }, [width, height])

  return <div className="absolute inset-0" ref={sizeRef}><div ref={hostRef} className="absolute inset-0" /><div ref={tooltipRef} style={TOOLTIP_STYLE} /></div>
}
