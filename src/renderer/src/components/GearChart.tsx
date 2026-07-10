import { useMemo, useRef, useCallback } from 'react'
import UPlotReact from 'uplot-react'
import uPlot from 'uplot'
import type { TelemetryRow } from '../types'
import { useSize } from '../hooks/useSize'
import { useChartTooltip, TOOLTIP_STYLE } from '../hooks/useChartTooltip'
import GraphTable, { type GraphTableColumn } from './GraphTable'

interface Props {
  data: TelemetryRow[]
  isDark: boolean
  view?: 'chart' | 'table'
}

const TABLE_COLS: GraphTableColumn[] = [
  { header: 'Gear', color: '#5794F2', format: v => String(Math.round(v)) },
]

const GEAR_BANDS = [
  { y1: 0.5, y2: 2.5, fill: 'rgba(31,96,196,0.2)'   },
  { y1: 2.5, y2: 4.5, fill: 'rgba(212,173,4,0.2)'   },
  { y1: 4.5, y2: 6.5, fill: 'rgba(196,125,14,0.2)'  },
  { y1: 6.5, y2: 8.5, fill: 'rgba(196,22,42,0.2)'   },
]

const REF_LINES = [2, 4, 6]

function fmtTime(s: number) {
  const m = Math.floor(s / 60)
  const sec = Math.floor(s % 60)
  return `${m}:${String(sec).padStart(2, '0')}`
}

export default function GearChart({ data, isDark, view = 'chart' }: Props) {
  const { ref: sizeRef, width, height } = useSize()
  const { tooltipRef, show, hide } = useChartTooltip()
  const mountedRef = useRef(false)
  const visible = width > 0 && height > 0
  if (visible) mountedRef.current = true

  const uData = useMemo((): uPlot.AlignedData => {
    const ts = new Float64Array(data.length)
    const g  = new Float64Array(data.length)
    data.forEach((d, i) => {
      ts[i] = d.session_time
      g[i]  = d.gear
    })
    return [ts, g]
  }, [data])

  const opts = useMemo((): uPlot.Options => {
    const axisColor   = isDark ? '#7c8098' : '#6b7280'
    const gridColor   = isDark ? 'rgba(255,255,255,0.04)' : 'rgba(0,0,0,0.07)'
    const refColor    = isDark ? 'rgba(255,255,255,0.1)'  : 'rgba(0,0,0,0.1)'
    const borderColor = isDark ? '#1e2136' : '#d0d5e0'

    const bandsPlugin: uPlot.Plugin = {
      hooks: {
        draw: (u) => {
          const ctx = u.ctx
          ctx.save()
          GEAR_BANDS.forEach(({ y1, y2, fill }) => {
            const top = u.valToPos(y2, 'y', true)
            const bot = u.valToPos(y1, 'y', true)
            ctx.fillStyle = fill
            ctx.fillRect(u.bbox.left, top, u.bbox.width, bot - top)
          })
          ctx.strokeStyle = refColor
          ctx.lineWidth = 1
          ctx.setLineDash([4, 4])
          REF_LINES.forEach((y) => {
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
          const ts = (u.data[0] as Float64Array)[idx]
          const g  = (u.data[1] as Float64Array)[idx]
          show([
            `<div style="color:${axisColor};margin-bottom:4px">${fmtTime(ts)}</div>`,
            `<div style="color:#5794F2">Gear ${g}</div>`,
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
        y: { range: [0.5, 8.5] },
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
          size: 28,
          ticks: { show: false },
          grid:  { show: false },
          gap: 4,
          splits: [1, 2, 3, 4, 5, 6, 7, 8],
          values: (_u, splits) => splits.map(v => String(v)),
        },
      ],
      series: [
        {},
        {
          label: 'Gear',
          stroke: '#5794F2',
          width: 2,
          paths: uPlot.paths.stepped!({ align: 1 }),
          points: { show: false },
        },
      ],
      plugins: [bandsPlugin, ttPlugin],
    }
  }, [width, height, isDark])

  const onCreate = useCallback((u: uPlot) => {
    u.over.addEventListener('mouseleave', hide)
  }, [hide])

  return (
    <div className="bg-[var(--bg-panel)] p-4 h-full flex flex-col">
      <h2 className="text-[11px] text-[var(--text-secondary)] uppercase tracking-widest mb-3 shrink-0">Gear</h2>
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
