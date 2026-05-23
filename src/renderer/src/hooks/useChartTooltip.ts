import { useRef, useCallback } from 'react'

export const TOOLTIP_STYLE: React.CSSProperties = {
  position: 'absolute',
  display: 'none',
  background: 'var(--bg-panel)',
  border: '1px solid var(--border)',
  borderRadius: 4,
  fontSize: 12,
  padding: '6px 10px',
  pointerEvents: 'none',
  zIndex: 10,
  color: 'var(--text-primary)',
  whiteSpace: 'nowrap',
  boxShadow: '0 4px 16px rgba(0,0,0,0.3)',
}

export function useChartTooltip() {
  const tooltipRef = useRef<HTMLDivElement>(null)

  const show = useCallback((
    html: string,
    cursorLeft: number,
    cursorTop: number,
    containerW: number,
    containerH: number,
  ) => {
    const el = tooltipRef.current
    if (!el) return
    el.innerHTML = html
    el.style.display = 'block'
    const tw = el.offsetWidth
    const th = el.offsetHeight
    const GAP = 16
    const PAD = 4
    const l = cursorLeft + GAP + tw > containerW
      ? Math.max(PAD, cursorLeft - tw - GAP)
      : cursorLeft + GAP
    const t = cursorTop + GAP + th > containerH
      ? Math.max(PAD, cursorTop - th - GAP)
      : cursorTop + GAP
    el.style.left = `${l}px`
    el.style.top  = `${t}px`
  }, [])

  const hide = useCallback(() => {
    const el = tooltipRef.current
    if (el) el.style.display = 'none'
  }, [])

  return { tooltipRef, show, hide }
}
