import { useMemo, useRef, useCallback } from 'react'
import UPlotReact from 'uplot-react'
import uPlot from 'uplot'
import type { MotionRow } from '../types'
import { useSize } from '../hooks/useSize'
import { useChartTooltip, TOOLTIP_STYLE } from '../hooks/useChartTooltip'
import { useScrollScale } from '../hooks/useScrollScale'
import GraphTable, { type GraphTableColumn } from './GraphTable'

interface Props {
  data: MotionRow[]
  isDark: boolean
  view?: 'chart' | 'table'
  windowSeconds?: number
}

const COLOR_LAT  = '#F0A500'
const COLOR_LONG = '#5794F2'

const TABLE_COLS: GraphTableColumn[] = [
  { header: 'Lateral',      color: COLOR_LAT,  format: v => `${v.toFixed(2)}g` },
  { header: 'Longitudinal', color: COLOR_LONG, format: v => `${v.toFixed(2)}g` },
]

function fmtTime(s: number) {
  const m = Math.floor(s / 60)
  const sec = Math.floor(s % 60)
  return `${m}:${String(sec).padStart(2, '0')}`
}

export default function GForceChart({ data, isDark, view = 'chart', windowSeconds = 30 }: Props) {
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
  )

  const uData = useMemo((): uPlot.AlignedData => {
    const ts   = new Float64Array(data.length)
    const lat  = new Float64Array(data.length)
    const long = new Float64Array(data.length)
    data.forEach((d, i) => {
      ts[i]   = d.session_time
      lat[i]  = d.g_lat
      long[i] = d.g_long
    })
    return [ts, lat, long]
  }, [data])

  const opts = useMemo((): uPlot.Options => {
    const axisColor   = isDark ? '#7c8098' : '#6b7280'
    const gridColor   = isDark ? 'rgba(255,255,255,0.04)' : 'rgba(0,0,0,0.07)'
    const zeroColor   = isDark ? 'rgba(255,255,255,0.2)'  : 'rgba(0,0,0,0.18)'
    const refColor    = isDark ? 'rgba(255,255,255,0.04)' : 'rgba(0,0,0,0.07)'
    const borderColor = isDark ? '#1e2136' : '#d0d5e0'

    const refLinesPlugin: uPlot.Plugin = {
      hooks: {
        draw: (u) => {
          const ctx = u.ctx
          ctx.save()
          ctx.strokeStyle = zeroColor
          ctx.lineWidth = 1
          ctx.setLineDash([])
          const y0 = u.valToPos(0, 'y', true)
          ctx.beginPath()
          ctx.moveTo(u.bbox.left, y0)
          ctx.lineTo(u.bbox.left + u.bbox.width, y0)
          ctx.stroke()
          ctx.strokeStyle = refColor
          ctx.setLineDash([4, 4]);
          [4, -4].forEach((y) => {
            const yPos = u.valToPos(y, 'y', true)
            ctx.beginPath()
            ctx.moveTo(u.bbox.left, yPos)
            ctx.lineTo(u.bbox.left + u.bbox.width, yPos)
            ctx.stroke()
          })
          ctx.restore()
        },
      },
    }

    const ttPlugin: uPlot.Plugin = {
      hooks: {
        setCursor: (u) => {
          const idx = u.cursor.idx
          if (idx == null) { hide(); return }
          const ts   = (u.data[0] as Float64Array)[idx]
          const lat  = (u.data[1] as Float64Array)[idx]
          const long = (u.data[2] as Float64Array)[idx]
          show([
            `<div style="color:${axisColor};margin-bottom:4px">${fmtTime(ts)}</div>`,
            `<div><span style="color:${COLOR_LAT}">Lateral</span>: ${lat.toFixed(2)} g</div>`,
            `<div><span style="color:${COLOR_LONG}">Longitudinal</span>: ${long.toFixed(2)} g</div>`,
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
        y: { range: [-6, 6] },
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
          size: 36,
          ticks: { show: false },
          grid:  { show: false },
          gap: 4,
          splits: [-6, -4, -2, 0, 2, 4, 6],
          values: (_u, splits) => splits.map(v => `${v}g`),
        },
      ],
      series: [
        {},
        { label: 'Lateral',       stroke: COLOR_LAT,  width: 1.5, points: { show: false } },
        { label: 'Longitudinal',  stroke: COLOR_LONG, width: 1.5, points: { show: false } },
      ],
      plugins: [refLinesPlugin, ttPlugin],
    }
  }, [width, height, isDark])

  const onCreate = useCallback((u: uPlot) => {
    attach(u)
    u.over.addEventListener('mouseleave', hide)
  }, [hide, attach])

  return (
    <div className="bg-[var(--bg-panel)] p-4 h-full flex flex-col">
      <div className="flex items-center justify-between mb-3 shrink-0">
        <h2 className="text-[11px] text-[var(--text-secondary)] uppercase tracking-widest">G-Force</h2>
        {view !== 'table' && (
          <div className="flex gap-4 text-xs">
            <span style={{ color: COLOR_LAT }}>— Lateral</span>
            <span style={{ color: COLOR_LONG }}>— Longitudinal</span>
            <span className="text-[var(--text-secondary)]">+ve = right / accel</span>
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
              {mountedRef.current && <UPlotReact options={opts} data={uData} onCreate={onCreate} onDelete={detach} resetScales={false} />}
            </div>
            <div ref={tooltipRef} style={TOOLTIP_STYLE} />
          </>
        )}
      </div>
    </div>
  )
}
