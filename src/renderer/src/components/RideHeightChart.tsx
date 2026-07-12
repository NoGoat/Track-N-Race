import { useMemo, useRef, useCallback, useEffect } from 'react'
import UPlotReact from 'uplot-react'
import uPlot from 'uplot'
import { useSize } from '../hooks/useSize'
import { useChartTooltip, TOOLTIP_STYLE } from '../hooks/useChartTooltip'
import { useScrollScale } from '../hooks/useScrollScale'
import { createDrawProfilerPlugin } from '../hooks/useDrawProfiler'
import { useChartDataProfiler } from '../hooks/useChartDataProfiler'
import { useTelemetryStore } from '../stores/telemetryStore'
import GraphTable, { type GraphTableColumn } from './GraphTable'
import { usePixelAlignment } from '../lib/chartPixelPolicy'

interface Props {
  isDark: boolean
  view?: 'chart' | 'table'
  windowSeconds?: number
}

const COLOR_FRONT = '#73BF69'
const COLOR_REAR  = '#B877DB'

// Static y-range whose bounds only ever expand outward. Scanning the whole
// visible window for min/max every scroll tick (a uPlot auto-range function)
// is expensive at 60Hz; instead we only look at the newest sample each time
// data changes and push a bound out if it's been exceeded, so the axis
// expands but never rescans.
const INITIAL_UPPER_MM = 50
const UPPER_PADDING_MM = 5
const INITIAL_LOWER_MM = 0
const LOWER_PADDING_MM = 2

const TABLE_COLS: GraphTableColumn[] = [
  { header: 'Front', color: COLOR_FRONT, format: v => `${v.toFixed(1)}mm` },
  { header: 'Rear',  color: COLOR_REAR,  format: v => `${v.toFixed(1)}mm` },
]

function fmtTime(s: number) {
  const m = Math.floor(s / 60)
  const sec = Math.floor(s % 60)
  return `${m}:${String(sec).padStart(2, '0')}`
}

function computeYSplits(lower: number, upper: number): number[] {
  const step = (upper - lower) / 4
  const splits: number[] = []
  for (let i = 0; i <= 4; i++) splits.push(Math.round(lower + step * i))
  return splits
}

export default function RideHeightChart({ isDark, view = 'chart', windowSeconds = 30 }: Props) {
  const data = useTelemetryStore(s => s.motionEx)
  useChartDataProfiler('RideHeight', data)
  const { ref: sizeRef, width, height } = useSize()
  const { tooltipRef, show, hide } = useChartTooltip()
  const mountedRef = useRef(false)
  const visible = width > 0 && height > 0
  if (visible) mountedRef.current = true

  const { attach, detach } = useScrollScale(
    view !== 'table',
    data.length > 0 ? data[data.length - 1].session_time : null,
    data.length > 0 ? data[0].session_time : null,
    windowSeconds,
    undefined,
    'RideHeight',
  )

  // Bounds live in a ref, not React state: uplot-react's options diff is a
  // shallow Object.is over each top-level key, and scales/axes/series/plugins
  // are fresh literals every time the opts memo runs — so if bounds were
  // useState and opts depended on them, every bound change would read as
  // "everything changed" and uplot-react would destroy+recreate the whole
  // chart instance instead of just updating a scale. Since ride-height is a
  // continuously-varying signal, that was firing often enough to be the
  // actual source of the Misc-tab stutter. Bounds are applied to the live
  // uPlot instance imperatively instead, so `opts` never depends on them.
  const chartRef = useRef<uPlot | null>(null)
  const boundsRef = useRef({ lower: INITIAL_LOWER_MM, upper: INITIAL_UPPER_MM })
  useEffect(() => {
    if (data.length === 0) return
    const last = data[data.length - 1]
    const maxVal = Math.max(last.front_aero_height_mm, last.rear_aero_height_mm)
    const minVal = Math.min(last.front_aero_height_mm, last.rear_aero_height_mm)
    const b = boundsRef.current
    let changed = false
    if (maxVal > b.upper - UPPER_PADDING_MM) {
      b.upper = Math.ceil(maxVal + UPPER_PADDING_MM)
      changed = true
    }
    if (minVal < b.lower + LOWER_PADDING_MM) {
      b.lower = Math.floor(minVal - LOWER_PADDING_MM)
      changed = true
    }
    const u = chartRef.current
    if (changed && u) {
      u.setScale('y', { min: b.lower, max: b.upper })
    }
  }, [data])

  const uData = useMemo((): uPlot.AlignedData => {
    const ts    = new Float64Array(data.length)
    const front = new Float64Array(data.length)
    const rear  = new Float64Array(data.length)
    data.forEach((d, i) => {
      ts[i]    = d.session_time
      front[i] = d.front_aero_height_mm
      rear[i]  = d.rear_aero_height_mm
    })
    return [ts, front, rear]
  }, [data])

  // Keep uplot-react's data prop stable so its effect cannot call redraw() on
  // every publication. Feed data into uPlot without drawing; useScrollScale's
  // rAF loop observes the changed u.data and performs the one scheduled draw.
  const initialUDataRef = useRef<uPlot.AlignedData>(uData)
  useEffect(() => {
    if (view !== 'table') chartRef.current?.setData(uData, false)
  }, [uData, view])

  const opts = useMemo((): uPlot.Options => {
    const axisColor   = isDark ? '#7c8098' : '#6b7280'
    const gridColor   = isDark ? 'rgba(255,255,255,0.04)' : 'rgba(0,0,0,0.07)'
    const borderColor = isDark ? '#1e2136' : '#d0d5e0'
    const b = boundsRef.current

    const ttPlugin: uPlot.Plugin = {
      hooks: {
        setCursor: (u) => {
          const idx = u.cursor.idx
          if (idx == null) { hide(); return }
          const ts    = (u.data[0] as Float64Array)[idx]
          const front = (u.data[1] as Float64Array)[idx]
          const rear  = (u.data[2] as Float64Array)[idx]
          show([
            `<div style="color:${axisColor};margin-bottom:4px">${fmtTime(ts)}</div>`,
            `<div><span style="color:${COLOR_FRONT}">Front</span>: ${front.toFixed(1)} mm</div>`,
            `<div><span style="color:${COLOR_REAR}">Rear</span>:  ${rear.toFixed(1)} mm</div>`,
          ].join(''), u.cursor.left ?? 0, u.cursor.top ?? 0, width, height)
        },
      },
    }

    return {
      width,
      height,
      pxAlign: usePixelAlignment(windowSeconds),
      padding: [4, 16, 0, 4],
      legend: { show: false },
      cursor: { drag: { setScale: false } },
      scales: {
        y: { range: [b.lower, b.upper] },
      },
      axes: [
        {
          stroke: axisColor,
          font: '11px "Cascadia Code", ui-monospace, monospace',
          ticks: { show: false },
          grid:  { stroke: gridColor, width: 1 },
          gap: 2,
          size: 22,
          values: (_u, splits) => splits.map(fmtTime),
          space: 80,
          border: { stroke: borderColor, width: 1 },
        },
        {
          stroke: axisColor,
          font: '11px "Cascadia Code", ui-monospace, monospace',
          size: 40,
          ticks: { show: false },
          grid:  { show: false },
          gap: 4,
          // A function (not a fixed array) so it re-derives ticks from the
          // scale's actual current min/max whenever uPlot recomputes axes —
          // correct after u.setScale('y', ...) without us ever touching
          // axis.splits directly (uPlot internally normalizes an array-form
          // splits into a function at chart construction; overwriting that
          // with a plain array later crashes the next axis recompute).
          splits: (_u, _axisIdx, scaleMin, scaleMax) => computeYSplits(scaleMin, scaleMax),
          values: (_u, splits) => splits.map(v => `${v}mm`),
        },
      ],
      series: [
        {},
        { label: 'Front', stroke: COLOR_FRONT, width: 1.5, points: { show: false } },
        { label: 'Rear',  stroke: COLOR_REAR,  width: 1.5, points: { show: false } },
      ],
      plugins: [ttPlugin, createDrawProfilerPlugin('RideHeight')],
    }
  }, [width, height, isDark, windowSeconds])

  const onCreate = useCallback((u: uPlot) => {
    chartRef.current = u
    attach(u)
    u.over.addEventListener('mouseleave', hide)
  }, [hide, attach])

  const onDelete = useCallback(() => {
    chartRef.current = null
    detach()
  }, [detach])

  return (
    <div className="bg-[var(--bg-panel)] p-4 h-full flex flex-col">
      <div className="flex items-center justify-between mb-3 shrink-0">
        <h2 className="text-[11px] text-[var(--text-secondary)] uppercase tracking-widest">Ride Height</h2>
        {view !== 'table' && (
          <div className="flex gap-4 text-xs">
            <span style={{ color: COLOR_FRONT }}>— Front</span>
            <span style={{ color: COLOR_REAR }}>— Rear</span>
            <span className="text-[var(--text-secondary)]">plank edge above road</span>
          </div>
        )}
      </div>
      <div className="flex-1 min-h-0 relative" ref={sizeRef}>
        {data.length === 0 ? (
          <div className="absolute inset-0 flex items-center justify-center text-[var(--text-secondary)] text-sm">No data</div>
        ) : view === 'table' ? (
          <GraphTable columns={TABLE_COLS} data={uData} />
        ) : (
          <>
            <div style={{ position: 'absolute', inset: 0, display: visible ? undefined : 'none' }}>
              {mountedRef.current && <UPlotReact options={opts} data={initialUDataRef.current} onCreate={onCreate} onDelete={onDelete} resetScales={false} />}
            </div>
            <div ref={tooltipRef} style={TOOLTIP_STYLE} />
          </>
        )}
      </div>
    </div>
  )
}
