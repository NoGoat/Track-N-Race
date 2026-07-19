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
  const stateRef = useRef({ html: '', visible: false, left: '', right: '', top: '', bottom: '' })

  const show = useCallback((
    html: string,
    cursorLeft: number,
    cursorTop: number,
    containerW: number,
    containerH: number,
  ) => {
    const el = tooltipRef.current
    if (!el) return
    const state = stateRef.current
    if (html !== state.html) {
      el.innerHTML = html
      state.html = html
    }
    if (!state.visible) {
      el.style.display = 'block'
      state.visible = true
    }
    const GAP = 16
    const PAD = 4
    // Anchor away from the nearest pair of container edges. This keeps the
    // tooltip inside the chart without offsetWidth/offsetHeight reads, which
    // forced synchronous layout on every hover update.
    const left = cursorLeft <= containerW / 2 ? `${cursorLeft + GAP}px` : 'auto'
    const right = cursorLeft > containerW / 2 ? `${Math.max(PAD, containerW - cursorLeft + GAP)}px` : 'auto'
    const top = cursorTop <= containerH / 2 ? `${cursorTop + GAP}px` : 'auto'
    const bottom = cursorTop > containerH / 2 ? `${Math.max(PAD, containerH - cursorTop + GAP)}px` : 'auto'
    if (left !== state.left) { el.style.left = left; state.left = left }
    if (right !== state.right) { el.style.right = right; state.right = right }
    if (top !== state.top) { el.style.top = top; state.top = top }
    if (bottom !== state.bottom) { el.style.bottom = bottom; state.bottom = bottom }
  }, [])

  const hide = useCallback(() => {
    const el = tooltipRef.current
    if (el && stateRef.current.visible) {
      el.style.display = 'none'
      stateRef.current.visible = false
    }
  }, [])

  return { tooltipRef, show, hide }
}
