import { useMemo, useRef, useCallback } from 'react'
import UPlotReact from 'uplot-react'
import uPlot from 'uplot'
import type { TelemetryRow } from '../types'
import { useSize } from '../hooks/useSize'
import { useChartTooltip, TOOLTIP_STYLE } from '../hooks/useChartTooltip'

interface Props {
  data: TelemetryRow[]
  isDark: boolean
}

const COLOR_STEER = '#BF5FFF'

function fmtTime(s: number) {
  const m = Math.floor(s / 60)
  const sec = Math.floor(s % 60)
  return `${m}:${String(sec).padStart(2, '0')}`
}

function fmtSteer(v: number): string {
  const pct = Math.abs(v * 100).toFixed(0)
  if (Math.abs(v) < 0.005) return '0%'
  return v < 0 ? `${pct}% L` : `${pct}% R`
}

export default function SteeringChart({ data, isDark }: Props) {
  const { ref: sizeRef, width, height } = useSize()
  const { tooltipRef, show, hide } = useChartTooltip()
  const mountedRef = useRef(false)
  const visible = width > 0 && height > 0
  if (visible) mountedRef.current = true

  const uData = useMemo((): uPlot.AlignedData => {
    const ts    = new Float64Array(data.length)
    const steer = new Float64Array(data.length)
    data.forEach((d, i) => {
      ts[i]    = d.session_time
      steer[i] = d.steering
    })
    return [ts, steer]
  }, [data])

  const opts = useMemo((): uPlot.Options => {
    const axisColor   = isDark ? '#7c8098' : '#6b7280'
    const gridColor   = isDark ? 'rgba(255,255,255,0.04)' : 'rgba(0,0,0,0.07)'
    const zeroColor   = isDark ? 'rgba(255,255,255,0.25)' : 'rgba(0,0,0,0.18)'
    const borderColor = isDark ? '#1e2136' : '#d0d5e0'

    const refLinePlugin: uPlot.Plugin = {
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
          ctx.restore()
        },
      },
    }

    const ttPlugin: uPlot.Plugin = {
      hooks: {
        setCursor: (u) => {
          const idx = u.cursor.idx
          if (idx == null) { hide(); return }
          const ts    = (u.data[0] as Float64Array)[idx]
          const steer = (u.data[1] as Float64Array)[idx]
          show([
            `<div style="color:${axisColor};margin-bottom:4px">${fmtTime(ts)}</div>`,
            `<div><span style="color:${COLOR_STEER}">Steering</span>: ${fmtSteer(steer)}</div>`,
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
          font: '11px ui-monospace, monospace',
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
          font: '11px ui-monospace, monospace',
          size: 52,
          ticks: { show: false },
          grid:  { show: false },
          gap: 4,
          splits: [-1, -0.5, 0, 0.5, 1],
          values: (_u, splits) => splits.map(v => {
            if (Math.abs(v) < 0.01) return '0%'
            const pct = Math.round(Math.abs(v) * 100)
            return v < 0 ? `${pct}%L` : `${pct}%R`
          }),
        },
      ],
      series: [
        {},
        {
          label: 'Steering',
          stroke: COLOR_STEER,
          width: 1.5,
          points: { show: false },
        },
      ],
      plugins: [refLinePlugin, ttPlugin],
    }
  }, [width, height, isDark])

  const onCreate = useCallback((u: uPlot) => {
    u.over.addEventListener('mouseleave', hide)
  }, [hide])

  return (
    <div className="bg-[var(--bg-panel)] p-4 h-full flex flex-col">
      <div className="flex items-center justify-between mb-3 shrink-0">
        <h2 className="text-[11px] text-[var(--text-secondary)] uppercase tracking-widest">Steering</h2>
        <div className="flex gap-4 text-xs">
          <span style={{ color: COLOR_STEER }}>— Input</span>
          <span className="text-[var(--text-secondary)]">L = left / R = right</span>
        </div>
      </div>
      <div className="flex-1 min-h-0 relative" ref={sizeRef}>
        {data.length === 0 ? (
          <div className="absolute inset-0 flex items-center justify-center text-[var(--text-secondary)] text-sm">No data</div>
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
