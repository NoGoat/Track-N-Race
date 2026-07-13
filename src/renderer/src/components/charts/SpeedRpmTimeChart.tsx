import { useEffect, useRef } from 'react'
import { useSize } from '../../hooks/useSize'
import { useChartTooltip, TOOLTIP_STYLE } from '../../hooks/useChartTooltip'
import { useTimeChartScroll } from '../../hooks/useTimeChartScroll'
import { TimeChart, corePlugins, type TChart } from '../../lib/timechart/tc'
import { createAxisPlugin, type AxisConfig } from '../../lib/timechart/axisPlugin'
import type { DataPoint } from '../../lib/timechart/dataBridge'

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

function referenceColors(isDark: boolean) {
  // Pre-blended equivalents of the old 35%-opacity uPlot strokes. Encoding
  // the muted result directly avoids WebGL canvas alpha/compositing variance.
  return isDark
    ? { speed: '#1f3c24', rpm: '#501722', ers: '#65581c' }
    : { speed: '#b9d5b5', rpm: '#e8b9c0', ers: '#fbed97' }
}

function syncBuffer(buffer: DataPoint[], source: DataPoint[], rebuild: boolean): boolean {
  if (rebuild || (buffer.length > 0 && source.length > 0 && source[source.length - 1].x < buffer[buffer.length - 1].x)) {
    if (buffer.length) buffer.splice(0, buffer.length)
  }
  if (source.length === 0) {
    if (buffer.length) { buffer.splice(0, buffer.length); return true }
    return rebuild
  }
  const lastX = buffer.length ? buffer[buffer.length - 1].x : -Infinity
  let lo = 0, hi = source.length
  while (lo < hi) { const mid = (lo + hi) >> 1; if (source[mid].x <= lastX) lo = mid + 1; else hi = mid }
  for (let i = lo; i < source.length; i++) buffer.push(source[i])
  if (buffer.length > source.length + 2048) {
    let trim = 0
    while (trim < buffer.length && buffer[trim].x < source[0].x) trim++
    if (trim) buffer.splice(0, trim)
  }
  return rebuild || lo < source.length
}

function sample(buffer: DataPoint[], x: number): number {
  if (!buffer.length || x < buffer[0].x || x > buffer[buffer.length - 1].x) return NaN
  let lo = 0, hi = buffer.length - 1
  while (lo < hi) { const mid = (lo + hi) >> 1; if (buffer[mid].x < x) lo = mid + 1; else hi = mid }
  if (lo > 0 && Math.abs(buffer[lo - 1].x - x) <= Math.abs(buffer[lo].x - x)) lo--
  return buffer[lo].y
}

export default function SpeedRpmTimeChart({ isDark, data, revision, scrolling, windowSeconds, xTickFormat, tooltipFormat }: Props) {
  const { ref: sizeRef, width, height } = useSize()
  const { tooltipRef, show, hide } = useChartTooltip()
  const hostRef = useRef<HTMLDivElement>(null)
  const chartRef = useRef<TChart | null>(null)
  const buffersRef = useRef<DataPoint[][]>([])
  const dirtyRef = useRef(false)
  const revisionRef = useRef('')
  const tooltipRefFn = useRef(tooltipFormat)
  tooltipRefFn.current = tooltipFormat
  const xFormatRef = useRef(xTickFormat)
  xFormatRef.current = xTickFormat

  const latestT = scrolling && data.current[0].length ? data.current[0][data.current[0].length - 1].x : null
  const firstT = scrolling && data.current[0].length ? data.current[0][0].x : null
  const { attach, detach } = useTimeChartScroll(scrolling, latestT, firstT, windowSeconds, dirtyRef, { fastFrames: true, fullFps: 60 }, 'SpeedRpm')

  useEffect(() => {
    const host = hostRef.current
    if (!host) return
    const axis = isDark ? '#7c8098' : '#6b7280'
    const muted = referenceColors(isDark)
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
        { name: 'Ref Speed', color: muted.speed, lineWidth: 1.5, data: [] },
        { name: 'Ref RPM', color: muted.rpm, lineWidth: 1.5, data: [] },
        { name: 'Ref ERS', color: muted.ers, lineWidth: 1.5, data: [] },
        { name: 'Speed', color: SPEED, lineWidth: 1.5, data: [] },
        { name: 'RPM', color: RPM, lineWidth: 1.5, data: [] },
        { name: 'ERS', color: ERS, lineWidth: 1.5, data: [] },
      ],
      plugins: { lineChart: corePlugins.lineChart, crosshair: corePlugins.crosshair, nearestPoint: corePlugins.nearestPoint, axis: createAxisPlugin(axisCfg) } as any,
    } as any)
    chartRef.current = chart
    buffersRef.current = chart.options.series.map(s => s.data as unknown as DataPoint[])
    host.style.color = axis
    host.style.setProperty('--background-overlay', isDark ? '#12141f' : '#ffffff')

    const detector = chart.contentBoxDetector.node
    const move = (ev: MouseEvent) => {
      const rect = host.getBoundingClientRect()
      const px = ev.clientX - rect.left
      const x = (chart.model.xScale as any).invert(px) as number
      const b = buffersRef.current
      show(tooltipRefFn.current(x, [sample(b[0], x), sample(b[1], x), sample(b[2], x)], [sample(b[3], x), sample(b[4], x), sample(b[5], x)]), px, ev.clientY - rect.top, host.clientWidth, host.clientHeight)
    }
    detector.addEventListener('mousemove', move)
    detector.addEventListener('mouseleave', hide)
    attach(chart)
    return () => {
      detector.removeEventListener('mousemove', move)
      detector.removeEventListener('mouseleave', hide)
      detach(); chart.dispose(); chartRef.current = null; buffersRef.current = []
    }
    // chart lifetime is stable; live values flow through refs
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

  useEffect(() => {
    const chart = chartRef.current
    if (!chart) return
    const sources = [...data.reference, ...data.current]
    const rebuild = revisionRef.current !== revision
    revisionRef.current = revision
    let changed = false
    sources.forEach((source, i) => { if (syncBuffer(buffersRef.current[i], source, rebuild)) changed = true })
    if (!changed) return
    dirtyRef.current = true
    if (!scrolling) {
      const populated = sources.filter(s => s.length)
      const max = populated.length ? Math.max(...populated.map(s => s[s.length - 1].x)) : 1
      chart.options.xRange = { min: 0, max: Math.max(max, 1) }
      dirtyRef.current = false
      chart.model.update()
    }
  }, [data, revision, scrolling])

  useEffect(() => { if (chartRef.current && width > 0 && height > 0) chartRef.current.onResize() }, [width, height])

  return <div className="absolute inset-0" ref={sizeRef}><div ref={hostRef} className="absolute inset-0" /><div ref={tooltipRef} style={TOOLTIP_STYLE} /></div>
}
