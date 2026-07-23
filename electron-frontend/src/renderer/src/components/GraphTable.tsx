import { useRef, useState, useLayoutEffect, useCallback } from 'react'
import { ChevronDown } from 'lucide-react'
import type { AlignedTable } from '../types'

// Raw-values table shown in place of a telemetry graph (the Chart→Table view mode
// ported from qt_frontend's GraphTable). One leading time column + one column
// per series, oldest at the top / newest at the bottom, holding every sample in the
// data the chart already built. Auto-scrolls to the newest row unless the user has
// scrolled up to inspect history. Only the handful of on-screen rows are rendered
// (fixed-height virtualisation), so a 10-minute window of streaming data stays cheap.

export interface GraphTableColumn {
  header: string
  color?: string
  format: (v: number) => string
}

const ROW_H = 26
const OVERSCAN = 6

function fmtTime(s: number): string {
  const m   = Math.floor(s / 60)
  const sec = Math.floor(s % 60)
  const ms  = Math.floor((s % 1) * 1000)
  return `${m}:${String(sec).padStart(2, '0')}.${String(ms).padStart(3, '0')}`
}

export default function GraphTable({ columns, data, edgePadRem = 1, noBorderTop = false }: {
  columns: GraphTableColumn[]
  data: AlignedTable
  // How far (in rem) the table should break out of its container's padding on the
  // left/right/bottom so it sits flush against the panel edge/border instead of
  // floating with a gap — matches the parent's own p-* padding (defaults to p-4's 1rem).
  edgePadRem?: number
  // Omit the top border — for callers whose container already has a border/divider
  // immediately above the table, where the default border-t would double up.
  noBorderTop?: boolean
}) {
  const scrollRef   = useRef<HTMLDivElement>(null)
  const pinnedRef   = useRef(true)              // are we pinned to the live edge?
  const frozenNRef  = useRef<number | null>(null) // row count snapshot while scrolled away
  const [scrollTop, setScrollTop] = useState(0)
  const [viewH, setViewH]         = useState(0)
  const [pinned, setPinned]       = useState(true) // mirrors pinnedRef; drives the "scroll to bottom" button

  const xs    = (data[0] as Float64Array | undefined) ?? new Float64Array()
  const liveN = xs.length
  // While scrolled away from the bottom, freeze the rendered row count at the
  // snapshot taken when the user scrolled off — new samples keep landing in `data`
  // in the background, but the table itself doesn't grow/shift under the user.
  const n = pinnedRef.current ? liveN : (frozenNRef.current ?? liveN)

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
    if (el && pinnedRef.current) el.scrollTop = el.scrollHeight
  }, [n, viewH])

  const onScroll = useCallback(() => {
    const el = scrollRef.current
    if (!el) return
    // Only the "Scroll to Bottom" button re-pins — manually scrolling back down to the
    // (frozen) bottom edge should NOT resume autoscroll on its own.
    const atBottom = el.scrollHeight - el.scrollTop - el.clientHeight < ROW_H * 1.5
    if (!atBottom && pinnedRef.current) {
      pinnedRef.current = false
      frozenNRef.current = liveN
      setPinned(false)
    }
    setScrollTop(el.scrollTop)
  }, [liveN])

  const scrollToBottom = useCallback(() => {
    pinnedRef.current = true
    frozenNRef.current = null
    setPinned(true)
    // Wait for the re-render (with the live row count) to size the scroller before jumping.
    requestAnimationFrame(() => {
      const el = scrollRef.current
      if (el) el.scrollTop = el.scrollHeight
    })
  }, [])

  // Time column + one column per series, all equal-width so they fill the available
  // width (left-aligned cells, Standings styling).
  const gridCols = Array(columns.length + 1).fill('minmax(0, 1fr)').join(' ')
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
        className={`grid items-center hover:bg-[var(--bg-hover)] ${i < n - 1 ? 'border-b border-[var(--border)]' : ''}`}
      >
        <span className="px-3 text-[13px] tabular-nums text-[var(--text-secondary)]">{fmtTime(xs[i])}</span>
        {columns.map((c, ci) => (
          <span key={ci} className="px-3 text-[13px] font-medium tabular-nums truncate" style={{ color: c.color }}>
            {c.format((data[ci + 1] as Float64Array)[i])}
          </span>
        ))}
      </div>,
    )
  }

  return (
    <div
      style={{ top: 0, left: `-${edgePadRem}rem`, right: `-${edgePadRem}rem`, bottom: `-${edgePadRem}rem` }}
      className={`absolute flex flex-col bg-[var(--bg-panel)] overflow-hidden ${noBorderTop ? '' : 'border-t border-[var(--border)]'}`}
    >
      <div className="overflow-x-auto flex-1 min-h-0 flex flex-col">
        <div
          style={{ gridTemplateColumns: gridCols }}
          className="grid shrink-0 border-b border-[var(--border)] bg-[var(--bg-panel)]"
        >
          <span className="px-3 py-1 text-[9px] uppercase tracking-widest text-[var(--text-secondary)] font-normal">Time</span>
          {columns.map((c, ci) => (
            <span key={ci} className="px-3 py-1 text-[9px] uppercase tracking-widest text-[var(--text-secondary)] font-normal">{c.header}</span>
          ))}
        </div>
        <div ref={scrollRef} onScroll={onScroll} className="flex-1 min-h-0 overflow-y-auto relative">
          <div style={{ height: total, position: 'relative' }}>{rows}</div>
        </div>
      </div>
      {!pinned && (
        <button
          onClick={scrollToBottom}
          className="absolute bottom-3 right-3 z-10 flex items-center gap-1 px-2.5 py-1 rounded-full border border-[var(--border)] bg-[var(--bg-panel)] text-[10px] text-[var(--text-secondary)] hover:text-[var(--text-primary)] hover:border-[var(--text-secondary)] shadow-lg transition-colors"
        >
          <ChevronDown size={12} />
          <span>Scroll to Bottom</span>
        </button>
      )}
    </div>
  )
}
