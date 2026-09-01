import { createElement, useRef, useCallback, useEffect } from 'react'
import { createPortal } from 'react-dom'

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

interface TooltipState {
  html: string
  visible: boolean
  left: string
  right: string
  top: string
  bottom: string
  width: number
  height: number
  cursorLeft: number
  cursorTop: number
  boundaryLeft: number
  boundaryTop: number
  viewportW: number
  viewportH: number
}

const GAP = 16
const PAD = 4

function placeTooltip(el: HTMLDivElement, state: TooltipState): void {
  const anchorLeft = state.boundaryLeft + state.cursorLeft
  const anchorTop = state.boundaryTop + state.cursorTop
  const preferredLeft = anchorLeft <= state.viewportW / 2
    ? anchorLeft + GAP
    : anchorLeft - GAP - state.width
  const preferredTop = anchorTop <= state.viewportH / 2
    ? anchorTop + GAP
    : anchorTop - GAP - state.height
  const left = `${Math.max(PAD, Math.min(preferredLeft, Math.max(PAD, state.viewportW - state.width - PAD)))}px`
  const top = `${Math.max(PAD, Math.min(preferredTop, Math.max(PAD, state.viewportH - state.height - PAD)))}px`
  if (left !== state.left) { el.style.left = left; state.left = left }
  if (top !== state.top) { el.style.top = top; state.top = top }
  if (state.right !== 'auto') { el.style.right = 'auto'; state.right = 'auto' }
  if (state.bottom !== 'auto') { el.style.bottom = 'auto'; state.bottom = 'auto' }
}

export function ChartTooltipPortal({
  tooltipRef,
  style = TOOLTIP_STYLE,
}: {
  tooltipRef: React.RefObject<HTMLDivElement | null>
  style?: React.CSSProperties
}) {
  const portal = document.getElementById('chart-tooltip-portal')
  return portal ? createPortal(createElement('div', { ref: tooltipRef, style }), portal) : null
}

export function useChartTooltip(boundaryRef: React.RefObject<HTMLElement | null>) {
  const tooltipRef = useRef<HTMLDivElement>(null)
  const stateRef = useRef<TooltipState>({
    html: '', visible: false, left: '', right: '', top: '', bottom: '',
    width: 0, height: 0, cursorLeft: 0, cursorTop: 0,
    boundaryLeft: 0, boundaryTop: 0, viewportW: window.innerWidth, viewportH: window.innerHeight,
  })

  useEffect(() => {
    const el = tooltipRef.current
    const boundary = boundaryRef.current
    if (!el || !boundary) return
    const updateBoundary = () => {
      const state = stateRef.current
      const rect = boundary.getBoundingClientRect()
      state.boundaryLeft = rect.left
      state.boundaryTop = rect.top
      state.viewportW = window.innerWidth
      state.viewportH = window.innerHeight
      if (state.visible) placeTooltip(el, state)
    }
    const tooltipObserver = new ResizeObserver(entries => {
      const entry = entries[0]
      if (!entry) return
      const rect = el.getBoundingClientRect()
      const state = stateRef.current
      if (rect.width === state.width && rect.height === state.height) return
      state.width = rect.width
      state.height = rect.height
      if (state.visible) placeTooltip(el, state)
    })
    const boundaryObserver = new ResizeObserver(updateBoundary)
    const updateVisibleBoundary = () => {
      if (stateRef.current.visible) updateBoundary()
    }
    updateBoundary()
    tooltipObserver.observe(el)
    boundaryObserver.observe(boundary)
    window.addEventListener('resize', updateBoundary)
    window.addEventListener('scroll', updateVisibleBoundary, true)
    return () => {
      tooltipObserver.disconnect()
      boundaryObserver.disconnect()
      window.removeEventListener('resize', updateBoundary)
      window.removeEventListener('scroll', updateVisibleBoundary, true)
    }
  }, [boundaryRef])

  const show = useCallback((
    html: string,
    cursorLeft: number,
    cursorTop: number,
  ) => {
    const el = tooltipRef.current
    if (!el) return
    const state = stateRef.current
    if (html !== state.html) {
      el.innerHTML = html
      state.html = html
    }
    if (!state.visible) {
      const rect = boundaryRef.current?.getBoundingClientRect()
      if (rect) {
        state.boundaryLeft = rect.left
        state.boundaryTop = rect.top
      }
      state.viewportW = window.innerWidth
      state.viewportH = window.innerHeight
      el.style.display = 'block'
      state.visible = true
    }
    state.cursorLeft = cursorLeft
    state.cursorTop = cursorTop
    // ResizeObserver caches the box size after content changes. Pointer moves
    // only perform arithmetic and style writes, avoiding synchronous layout on
    // this hot path while still clamping the complete tooltip to every edge.
    placeTooltip(el, state)
  }, [boundaryRef])

  const hide = useCallback(() => {
    const el = tooltipRef.current
    if (el && stateRef.current.visible) {
      el.style.display = 'none'
      stateRef.current.visible = false
    }
  }, [])

  return { tooltipRef, show, hide }
}
