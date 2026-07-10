import { useRef, useState, useLayoutEffect, useCallback } from 'react'
import type uPlot from 'uplot'

// Raw-values table shown in place of a telemetry graph (the Chart→Table view mode
// ported from native_recorder's GraphTable). One leading time column + one column
// per series, oldest at the top / newest at the bottom, holding every sample in the
// data the chart already built. Auto-scrolls to the newest row unless the user has
// scrolled up to inspect history. Only the handful of on-screen rows are rendered
// (fixed-height virtualisation), so a 10-minute window of streaming data stays cheap.

export interface GraphTableColumn {
  header: string
  color?: string
  format: (v: number) => string
}

const ROW_H = 20
const OVERSCAN = 6

function fmtTime(s: number): string {
  const m = Math.floor(s / 60)
  const sec = s % 60
  return `${m}:${sec.toFixed(1).padStart(4, '0')}`
}

export default function GraphTable({ columns, data }: {
  columns: GraphTableColumn[]
  data: uPlot.AlignedData
}) {
  const scrollRef = useRef<HTMLDivElement>(null)
  const stickRef  = useRef(true)               // is the view pinned to the newest row?
  const [scrollTop, setScrollTop] = useState(0)
  const [viewH, setViewH]         = useState(0)

  const xs = (data[0] as Float64Array | undefined) ?? new Float64Array()
  const n  = xs.length

  useLayoutEffect(() => {
    const el = scrollRef.current
    if (!el) return
    const ro = new ResizeObserver(() => setViewH(el.clientHeight))
    ro.observe(el)
    setViewH(el.clientHeight)
    return () => ro.disconnect()
  }, [])

  // Keep the newest row in view as data streams in, unless the user scrolled up.
  useLayoutEffect(() => {
    const el = scrollRef.current
    if (el && stickRef.current) el.scrollTop = el.scrollHeight
  }, [n, viewH])

  const onScroll = useCallback(() => {
    const el = scrollRef.current
    if (!el) return
    stickRef.current = el.scrollHeight - el.scrollTop - el.clientHeight < ROW_H * 1.5
    setScrollTop(el.scrollTop)
  }, [])

  const gridCols = `76px ${columns.map(() => '1fr').join(' ')}`
  const total    = n * ROW_H
  const first    = Math.max(0, Math.floor(scrollTop / ROW_H) - OVERSCAN)
  const count    = Math.min(n - first, Math.ceil(viewH / ROW_H) + OVERSCAN * 2)

  const rows: React.ReactNode[] = []
  for (let k = 0; k < count; k++) {
    const i = first + k
    rows.push(
      <div
        key={i}
        style={{ position: 'absolute', top: i * ROW_H, height: ROW_H, left: 0, right: 0, gridTemplateColumns: gridCols }}
        className="grid items-center px-2 text-[11px] tabular-nums border-b border-[var(--border)]/40"
      >
        <span className="text-[var(--text-secondary)]">{fmtTime(xs[i])}</span>
        {columns.map((c, ci) => (
          <span key={ci} className="text-right pr-2 truncate" style={{ color: c.color }}>
            {c.format((data[ci + 1] as Float64Array)[i])}
          </span>
        ))}
      </div>,
    )
  }

  return (
    <div className="absolute inset-0 flex flex-col bg-[var(--bg-panel)] overflow-hidden">
      <div
        style={{ gridTemplateColumns: gridCols }}
        className="grid shrink-0 px-2 py-1 text-[10px] uppercase tracking-wider text-[var(--text-secondary)] border-b border-[var(--border)]"
      >
        <span>Time</span>
        {columns.map((c, ci) => (
          <span key={ci} className="text-right pr-2" style={{ color: c.color }}>{c.header}</span>
        ))}
      </div>
      <div ref={scrollRef} onScroll={onScroll} className="flex-1 min-h-0 overflow-y-auto relative">
        <div style={{ height: total, position: 'relative' }}>{rows}</div>
      </div>
    </div>
  )
}
