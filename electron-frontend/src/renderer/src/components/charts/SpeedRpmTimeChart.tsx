import { useEffect, useRef } from 'react'
import { useSize } from '../../hooks/useSize'
import { useChartTooltip, TOOLTIP_STYLE } from '../../hooks/useChartTooltip'
import { useTimeChartScroll } from '../../hooks/useTimeChartScroll'
import { TimeChart, corePlugins, type TChart } from '../../lib/timechart/tc'
import { createAxisPlugin, type AxisConfig } from '../../lib/timechart/axisPlugin'
import type { DataPoint } from '../../lib/timechart/dataBridge'
import { AlignedDataBuffer, type SeriesData } from '../../lib/timechart/engine/core/alignedData'

export interface SpeedRpmSeriesSet {
  reference: [DataPoint[], DataPoint[], DataPoint[]]
  current: [DataPoint[], DataPoint[], DataPoint[]]
}

interface Props {
  isDark: boolean
  data: SpeedRpmSeriesSet
  revision: string
  scrolling: boolean
  windowSeconds: number
  xTickFormat: (x: number) => string
  tooltipFormat: (x: number, reference: number[], current: number[]) => string
}

const SPEED = '#37872D', RPM = '#C4162A', ERS = '#FADE2A'
const Y_TICKS = [0, 0.25, 0.5, 0.75, 1]

function nearestIndex(data: SeriesData, x: number): number {
  if (data.length === 0) return -1
  const after = data.lowerBoundX(x)
  if (after === 0) return 0
  if (after === data.length) return data.length - 1
  return x - data.xAt(after - 1) <= data.xAt(after) - x ? after - 1 : after
}

function referenceColors(isDark: boolean) {
  // Pre-blended equivalents of the old 35%-opacity uPlot strokes. Encoding
  // the muted result directly avoids WebGL canvas alpha/compositing variance.
  return isDark
    ? { speed: '#1f3c24', rpm: '#501722', ers: '#65581c' }
    : { speed: '#b9d5b5', rpm: '#e8b9c0', ers: '#fbed97' }
}

function syncGroup(buffer: AlignedDataBuffer, sources: readonly DataPoint[][], rebuild: boolean, values: Float64Array): boolean {
  const timeline = sources[0]
  if (rebuild || (buffer.length > 0 && timeline.length > 0 && timeline[timeline.length - 1].x < buffer.lastX)) {
    buffer.clear()
  }
  if (timeline.length === 0) {
    if (buffer.length > 0) { buffer.clear(); return true }
    return rebuild
  }
  const lastX = buffer.length > 0 ? buffer.lastX : -Infinity
  let lo = 0, hi = timeline.length
  while (lo < hi) { const mid = (lo + hi) >> 1; if (timeline[mid].x <= lastX) lo = mid + 1; else hi = mid }
  const appendStart = lo
  for (let i = appendStart; i < timeline.length; i++) {
    for (let channel = 0; channel < sources.length; channel++) values[channel] = sources[channel][i]?.y ?? NaN
    buffer.append(timeline[i].x, values)
  }
  const trim = buffer.lowerBoundX(timeline[0].x)
  if (trim > 0) buffer.evictFront(trim)
  return rebuild || appendStart < timeline.length || trim > 0
}

export default function SpeedRpmTimeChart({ isDark, data, revision, scrolling, windowSeconds, xTickFormat, tooltipFormat }: Props) {
  const { ref: sizeRef, width, height } = useSize()
  const { tooltipRef, show, hide } = useChartTooltip()
  const hostRef = useRef<HTMLDivElement>(null)
  const chartRef = useRef<TChart | null>(null)
  const groupsRef = useRef<[AlignedDataBuffer, AlignedDataBuffer] | null>(null)
  const groupScratchRef = useRef<[Float64Array, Float64Array]>([new Float64Array(3), new Float64Array(3)])
  const dirtyRef = useRef(false)
  const revisionRef = useRef('')
  const tooltipRefFn = useRef(tooltipFormat)
  tooltipRefFn.current = tooltipFormat
  const xFormatRef = useRef(xTickFormat)
  xFormatRef.current = xTickFormat

  const latestT = scrolling && data.current[0].length ? data.current[0][data.current[0].length - 1].x : null
  const firstT = scrolling && data.current[0].length ? data.current[0][0].x : null
  const { attach, detach, wake } = useTimeChartScroll(scrolling, latestT, firstT, windowSeconds, dirtyRef, { fastFrames: true, fullFps: 60 }, 'SpeedRpm')

  useEffect(() => {
    const host = hostRef.current
    if (!host) return
    const axis = isDark ? '#7c8098' : '#6b7280'
    const muted = referenceColors(isDark)
    const referenceGroup = new AlignedDataBuffer(3)
    const currentGroup = new AlignedDataBuffer(3)
    groupsRef.current = [referenceGroup, currentGroup]
    const axisCfg: { current: AxisConfig } = { current: {
      axisColor: axis,
      yAxisColor: SPEED,
      gridColor: isDark ? 'rgba(255,255,255,0.04)' : 'rgba(0,0,0,0.07)',
      borderColor: isDark ? '#1e2136' : '#d0d5e0',
      font: '11px "Cascadia Code", ui-monospace, monospace',
      xTickSpacePx: 80,
      xTickFormat: x => xFormatRef.current(x),
      yTickValues: () => Y_TICKS,
      yTickFormat: v => String(Math.round(v * 380)),
      xGap: 2, yGap: 4, showYGrid: true,
      extraYAxes: [
        { side: 'right', offset: 4, color: RPM, values: Y_TICKS, format: v => v === 0 ? '0' : `${Math.round(v * 16)}k` },
        { side: 'right', offset: 54, color: ERS, values: Y_TICKS, format: v => `${Math.round(v * 100)}%` },
      ],
    } }
    const chart = new TimeChart.core(host, {
      paddingTop: 4, paddingRight: 100, paddingBottom: 22, paddingLeft: 44,
      renderPaddingTop: 4, renderPaddingRight: 100, renderPaddingBottom: 22, renderPaddingLeft: 44,
      yRange: { min: 0, max: 1 }, lineWidth: 1.5,
      series: [
        { name: 'Ref Speed', color: muted.speed, lineWidth: 1.5, data: referenceGroup.series[0] },
        { name: 'Ref RPM', color: muted.rpm, lineWidth: 1.5, data: referenceGroup.series[1] },
        { name: 'Ref ERS', color: muted.ers, lineWidth: 1.5, data: referenceGroup.series[2] },
        { name: 'Speed', color: SPEED, lineWidth: 1.5, data: currentGroup.series[0] },
        { name: 'RPM', color: RPM, lineWidth: 1.5, data: currentGroup.series[1] },
        { name: 'ERS', color: ERS, lineWidth: 1.5, data: currentGroup.series[2] },
      ],
      plugins: { lineChart: corePlugins.lineChart, crosshair: corePlugins.crosshair, nearestPoint: corePlugins.nearestPoint, axis: createAxisPlugin(axisCfg) } as any,
    } as any)
    chartRef.current = chart
    host.style.color = axis
    host.style.setProperty('--background-overlay', isDark ? '#12141f' : '#ffffff')

    const referenceValues = [NaN, NaN, NaN]
    const currentValues = [NaN, NaN, NaN]
    const move = (contentX: number, contentY: number) => {
      const px = contentX + 44
      const x = (chart.model.xScale as any).invert(px) as number
      const chartSeries = chart.options.series
      const referenceIndex = nearestIndex(chartSeries[0].data, x)
      const currentIndex = nearestIndex(chartSeries[3].data, x)
      for (let i = 0; i < 3; i++) {
        referenceValues[i] = referenceIndex >= 0 ? chartSeries[i].data.yAt(referenceIndex) : NaN
        currentValues[i] = currentIndex >= 0 ? chartSeries[i + 3].data.yAt(currentIndex) : NaN
      }
      const snappedX = currentIndex >= 0
        ? chartSeries[3].data.xAt(currentIndex)
        : referenceIndex >= 0 ? chartSeries[0].data.xAt(referenceIndex) : x
      show(tooltipRefFn.current(snappedX, referenceValues, currentValues), px, contentY + 4, chart.clientWidth, chart.clientHeight)
    }
    const stopMove = chart.contentBoxDetector.moved.on(move)
    const stopLeave = chart.contentBoxDetector.left.on(hide)
    attach(chart)
    return () => {
      stopMove()
      stopLeave()
      detach(); chart.dispose(); chartRef.current = null; groupsRef.current = null
    }
    // chart lifetime is stable; live values flow through refs
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

  useEffect(() => {
    const chart = chartRef.current
    const groups = groupsRef.current
    if (!chart || !groups) return
    const sources = [...data.reference, ...data.current]
    const rebuild = revisionRef.current !== revision
    revisionRef.current = revision
    let changed = false
    if (syncGroup(groups[0], data.reference, rebuild, groupScratchRef.current[0])) changed = true
    if (syncGroup(groups[1], data.current, rebuild, groupScratchRef.current[1])) changed = true
    if (!changed) return
    dirtyRef.current = true
    wake()
    if (!scrolling) {
      const populated = sources.filter(s => s.length)
      const max = populated.length ? Math.max(...populated.map(s => s[s.length - 1].x)) : 1
      chart.options.xRange = { min: 0, max: Math.max(max, 1) }
      dirtyRef.current = false
      chart.model.requestRedraw()
    }
  }, [data, revision, scrolling, wake])

  useEffect(() => { if (chartRef.current && width > 0 && height > 0) chartRef.current.onResize() }, [width, height])

  return <div className="absolute inset-0" ref={sizeRef}><div ref={hostRef} className="absolute inset-0" /><div ref={tooltipRef} style={TOOLTIP_STYLE} /></div>
}
