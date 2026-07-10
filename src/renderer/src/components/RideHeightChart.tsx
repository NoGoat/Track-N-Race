import { useMemo, useRef, useCallback } from 'react'
import UPlotReact from 'uplot-react'
import uPlot from 'uplot'
import type { MotionExRow } from '../types'
import { useSize } from '../hooks/useSize'
import { useChartTooltip, TOOLTIP_STYLE } from '../hooks/useChartTooltip'
import GraphTable, { type GraphTableColumn } from './GraphTable'

interface Props {
  data: MotionExRow[]
  isDark: boolean
  view?: 'chart' | 'table'
}

const COLOR_FRONT = '#73BF69'
const COLOR_REAR  = '#B877DB'

const TABLE_COLS: GraphTableColumn[] = [
  { header: 'Front', color: COLOR_FRONT, format: v => `${v.toFixed(1)}mm` },
  { header: 'Rear',  color: COLOR_REAR,  format: v => `${v.toFixed(1)}mm` },
]

function fmtTime(s: number) {
  const m = Math.floor(s / 60)
  const sec = Math.floor(s % 60)
  return `${m}:${String(sec).padStart(2, '0')}`
}

export default function RideHeightChart({ data, isDark, view = 'chart' }: Props) {
  const { ref: sizeRef, width, height } = useSize()
  const { tooltipRef, show, hide } = useChartTooltip()
  const mountedRef = useRef(false)
  const visible = width > 0 && height > 0
  if (visible) mountedRef.current = true

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

  const opts = useMemo((): uPlot.Options => {
    const axisColor   = isDark ? '#7c8098' : '#6b7280'
    const gridColor   = isDark ? 'rgba(255,255,255,0.04)' : 'rgba(0,0,0,0.07)'
    const borderColor = isDark ? '#1e2136' : '#d0d5e0'

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
      padding: [4, 16, 0, 4],
      legend: { show: false },
      cursor: { drag: { setScale: false } },
      scales: {
        y: { range: (_u, min, max) => [Math.max(0, min - 2), max + 2] },
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
          values: (_u, splits) => splits.map(v => `${v}mm`),
        },
      ],
      series: [
        {},
        { label: 'Front', stroke: COLOR_FRONT, width: 1.5, points: { show: false } },
        { label: 'Rear',  stroke: COLOR_REAR,  width: 1.5, points: { show: false } },
      ],
      plugins: [ttPlugin],
    }
  }, [width, height, isDark])

  const onCreate = useCallback((u: uPlot) => {
    u.over.addEventListener('mouseleave', hide)
  }, [hide])

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
              {mountedRef.current && <UPlotReact options={opts} data={uData} onCreate={onCreate} />}
            </div>
            <div ref={tooltipRef} style={TOOLTIP_STYLE} />
          </>
        )}
      </div>
    </div>
  )
}
