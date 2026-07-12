import { useMemo, useRef, useCallback } from 'react'
import UPlotReact from 'uplot-react'
import uPlot from 'uplot'
import { useSize } from '../hooks/useSize'
import { useChartTooltip, TOOLTIP_STYLE } from '../hooks/useChartTooltip'
import { useScrollScale } from '../hooks/useScrollScale'
import { createDrawProfilerPlugin } from '../hooks/useDrawProfiler'
import { useTelemetryStore } from '../stores/telemetryStore'
import GraphTable, { type GraphTableColumn } from './GraphTable'

interface Props {
  isDark: boolean
  view?: 'chart' | 'table'
  windowSeconds?: number
}

const COLOR_THROTTLE = '#37872D'
const COLOR_BRAKE    = '#C4162A'

// Brake is stored negated (see uData) — the table shows its magnitude as a percent.
const TABLE_COLS: GraphTableColumn[] = [
  { header: 'Throttle', color: COLOR_THROTTLE, format: v => `${Math.round(v * 100)}%` },
  { header: 'Brake',    color: COLOR_BRAKE,    format: v => `${Math.round(Math.abs(v) * 100)}%` },
]


function fmtTime(s: number) {
  const m = Math.floor(s / 60)
  const sec = Math.floor(s % 60)
  return `${m}:${String(sec).padStart(2, '0')}`
}

export default function InputsChart({ isDark, view = 'chart', windowSeconds = 30 }: Props) {
  const data = useTelemetryStore(s => s.telemetry)
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
    const ts       = new Float64Array(data.length)
    const throttle = new Float64Array(data.length)
    const brake    = new Float64Array(data.length)
    data.forEach((d, i) => {
      ts[i]       = d.session_time
      throttle[i] = d.throttle
      brake[i]    = -d.brake
    })
    return [ts, throttle, brake]
  }, [data])

  const opts = useMemo((): uPlot.Options => {
    const zeroColor = isDark ? 'rgba(255,255,255,0.25)' : 'rgba(0,0,0,0.18)'
    const gridColor = isDark ? 'rgba(255,255,255,0.04)' : 'rgba(0,0,0,0.07)'
    const axisColor = isDark ? '#7c8098' : '#6b7280'
    const borderColor = isDark ? '#1e2136' : '#d0d5e0'

    const zeroLinePlugin: uPlot.Plugin = {
      hooks: {
        draw: (u) => {
          const ctx = u.ctx
          ctx.save()
          ctx.strokeStyle = zeroColor
          ctx.lineWidth = 1
          ctx.setLineDash([])
          const yPos = u.valToPos(0, 'y', true)
          ctx.beginPath()
          ctx.moveTo(u.bbox.left, yPos)
          ctx.lineTo(u.bbox.left + u.bbox.width, yPos)
          ctx.stroke()
          ctx.restore()
        },
      },
    }

    const ttPlugin: uPlot.Plugin = {
      hooks: {
        setCursor: (u) => {
          const idx = u.cursor.idx
          if (idx == null) { hide(); return }
          const ts       = (u.data[0] as Float64Array)[idx]
          const throttle = (u.data[1] as Float64Array)[idx]
          const brake    = Math.abs((u.data[2] as Float64Array)[idx])
          show([
            `<div style="color:${axisColor};margin-bottom:4px">${fmtTime(ts)}</div>`,
            `<div><span style="color:${COLOR_THROTTLE}">Throttle</span>: ${Math.round(throttle * 100)}%</div>`,
            `<div><span style="color:${COLOR_BRAKE}">Brake</span>: ${Math.round(brake * 100)}%</div>`,
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
        y: { range: [-1, 1] },
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
          splits: [-1, -0.5, 0, 0.5, 1],
          values: (_u, splits) => splits.map(v => `${Math.round(Math.abs(v) * 100)}%`),
        },
      ],
      series: [
        {},
        {
          label: 'throttle',
          stroke: COLOR_THROTTLE,
          fill: 'rgba(55,135,45,0.2)',
          width: 1.5,
          paths: uPlot.paths.stepped!({ align: -1 }),
          points: { show: false },
        },
        {
          label: 'brake',
          stroke: COLOR_BRAKE,
          fill: 'rgba(196,22,42,0.2)',
          width: 1.5,
          paths: uPlot.paths.stepped!({ align: -1 }),
          points: { show: false },
        },
      ],
      plugins: [zeroLinePlugin, ttPlugin, createDrawProfilerPlugin('Inputs')],
    }
  }, [width, height, isDark])

  const onCreate = useCallback((u: uPlot) => {
    attach(u)
    u.over.addEventListener('mouseleave', hide)
  }, [hide, attach])

  return (
    <div className="bg-[var(--bg-panel)] p-4 h-full flex flex-col">
      <div className="flex items-center justify-between mb-3 shrink-0">
        <h2 className="text-[11px] text-[var(--text-secondary)] uppercase tracking-widest">Throttle</h2>
        {view !== 'table' && (
          <div className="flex gap-4 text-xs">
            <span style={{ color: COLOR_THROTTLE }}>▲ Throttle</span>
            <span style={{ color: COLOR_BRAKE }}>▼ Brake</span>
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
