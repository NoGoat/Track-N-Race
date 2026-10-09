import { useEffect, useRef, useState, type KeyboardEvent, type PointerEvent, type RefObject } from 'react'
import { DEFAULT_SPLIT_RATIO, clampSplitRatio } from '../lib/analyzeMetrics'

interface Props {
  /** Share of the container's width given to the pane before the handle. */
  ratio: number
  /** The flex row both panes live in; ratios are measured against its width. */
  containerRef: RefObject<HTMLElement | null>
  /** Live ratio while dragging or nudging, applied every animation frame. */
  onPreview: (ratio: number) => void
  /** Final ratio, to persist. */
  onCommit: (ratio: number) => void
}

const percent = (ratio: number) => Math.round(ratio * 100)

// The draggable divider between Split view's graphs and map. A 1 px rule with
// a wider invisible grip; while hovered, focused or dragged a small badge shows
// the split as "graphs / map" percentages. Double-click restores 50 / 50;
// arrow keys nudge by 1 % (Shift: 5 %).
export default function AnalysisSplitHandle({ ratio, containerRef, onPreview, onCommit }: Props) {
  const [dragging, setDragging] = useState(false)
  const [hovered, setHovered] = useState(false)
  const [focused, setFocused] = useState(false)
  const latestRef = useRef(ratio)
  const frameRef = useRef(0)
  const pointerRef = useRef<number | null>(null)

  useEffect(() => { if (!dragging) latestRef.current = ratio }, [dragging, ratio])
  useEffect(() => () => { if (frameRef.current) cancelAnimationFrame(frameRef.current) }, [])

  const preview = (next: number) => {
    latestRef.current = clampSplitRatio(next)
    if (frameRef.current) return
    // Resizing the graphs is costly; apply at most once per frame.
    frameRef.current = requestAnimationFrame(() => {
      frameRef.current = 0
      onPreview(latestRef.current)
    })
  }

  const ratioAt = (clientX: number) => {
    const rect = containerRef.current?.getBoundingClientRect()
    return rect && rect.width > 0 ? (clientX - rect.left) / rect.width : latestRef.current
  }

  const startDrag = (event: PointerEvent<HTMLDivElement>) => {
    if (!event.isPrimary || event.button !== 0) return
    event.preventDefault()
    event.currentTarget.setPointerCapture(event.pointerId)
    pointerRef.current = event.pointerId
    setDragging(true)
  }

  const moveDrag = (event: PointerEvent<HTMLDivElement>) => {
    if (pointerRef.current !== event.pointerId) return
    preview(ratioAt(event.clientX))
  }

  const finishDrag = (event: PointerEvent<HTMLDivElement>) => {
    if (pointerRef.current !== event.pointerId) return
    pointerRef.current = null
    setDragging(false)
    if (event.currentTarget.hasPointerCapture(event.pointerId)) {
      event.currentTarget.releasePointerCapture(event.pointerId)
    }
    if (frameRef.current) {
      cancelAnimationFrame(frameRef.current)
      frameRef.current = 0
    }
    onCommit(latestRef.current)
  }

  const nudge = (event: KeyboardEvent<HTMLDivElement>) => {
    const step = event.shiftKey ? 0.05 : 0.01
    const delta = event.key === 'ArrowLeft' ? -step : event.key === 'ArrowRight' ? step : 0
    if (!delta) return
    event.preventDefault()
    const next = clampSplitRatio(latestRef.current + delta)
    latestRef.current = next
    onCommit(next)
  }

  const shown = ratio
  const active = dragging || hovered || focused

  return <div className="relative z-20 w-px shrink-0 bg-[var(--border)]">
    <div
      role="separator"
      aria-orientation="vertical"
      aria-label="Resize graphs and map"
      aria-valuemin={20}
      aria-valuemax={80}
      aria-valuenow={percent(shown)}
      aria-valuetext={`Graphs ${percent(shown)}%, map ${100 - percent(shown)}%`}
      tabIndex={0}
      title="Drag to resize · Double-click for 50 / 50"
      onPointerDown={startDrag}
      onPointerMove={moveDrag}
      onPointerUp={finishDrag}
      onPointerCancel={finishDrag}
      onDoubleClick={() => {
        latestRef.current = DEFAULT_SPLIT_RATIO
        onCommit(DEFAULT_SPLIT_RATIO)
      }}
      onKeyDown={nudge}
      onPointerEnter={() => setHovered(true)}
      onPointerLeave={() => setHovered(false)}
      onFocus={() => setFocused(true)}
      onBlur={() => setFocused(false)}
      className="absolute inset-y-0 -left-[3px] w-[7px] cursor-col-resize touch-none transition-colors focus-visible:outline-none"
      style={{ backgroundColor: active ? 'color-mix(in srgb, var(--border-focus) 40%, transparent)' : undefined }}
    />
    <div
      aria-hidden="true"
      className={`pointer-events-none absolute left-1/2 top-2 -translate-x-1/2 whitespace-nowrap rounded border border-[var(--border)] bg-[var(--bg-panel)] px-1.5 py-0.5 font-mono text-[9px] tabular-nums text-[var(--text-secondary)] shadow-[0_2px_8px_rgba(0,0,0,0.25)] transition-opacity duration-150 ${
        active ? 'opacity-100' : 'opacity-0'
      }`}
    >{percent(shown)} / {100 - percent(shown)}</div>
  </div>
}
