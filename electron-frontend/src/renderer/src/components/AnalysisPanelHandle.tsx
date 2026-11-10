import { useEffect, useRef, useState, type KeyboardEvent, type PointerEvent } from 'react'

interface Props {
  /** The way the divider line runs: `vertical` is dragged left and right. */
  orientation: 'vertical' | 'horizontal'
  label: string
  value: number
  min: number
  max: number
  defaultValue: number
  /** Arrow key step; Shift moves five times as far. */
  step: number
  /** Dragging right or down makes the value smaller. */
  reversed?: boolean
  /** The value with the divider at this pointer position. */
  valueAt: (clientX: number, clientY: number) => number
  /** Live value while dragging, applied at most once per animation frame. */
  onPreview: (value: number) => void
  /** Final value, to persist. */
  onCommit: (value: number) => void
}

// A divider between two parts of a panel. A wider invisible grip around the
// line; double-click restores the default, arrow keys nudge.
export default function AnalysisPanelHandle({
  orientation, label, value, min, max, defaultValue, step, reversed = false, valueAt, onPreview, onCommit,
}: Props) {
  const [dragging, setDragging] = useState(false)
  const [hovered, setHovered] = useState(false)
  const [focused, setFocused] = useState(false)
  const latestRef = useRef(value)
  const frameRef = useRef(0)
  const pointerRef = useRef<number | null>(null)

  useEffect(() => { if (!dragging) latestRef.current = value }, [dragging, value])
  useEffect(() => () => { if (frameRef.current) cancelAnimationFrame(frameRef.current) }, [])

  const clamp = (next: number) => Math.min(max, Math.max(min, next))

  const startDrag = (event: PointerEvent<HTMLDivElement>) => {
    if (!event.isPrimary || event.button !== 0) return
    event.preventDefault()
    event.currentTarget.setPointerCapture(event.pointerId)
    pointerRef.current = event.pointerId
    setDragging(true)
  }

  const moveDrag = (event: PointerEvent<HTMLDivElement>) => {
    if (pointerRef.current !== event.pointerId) return
    latestRef.current = clamp(valueAt(event.clientX, event.clientY))
    if (frameRef.current) return
    frameRef.current = requestAnimationFrame(() => {
      frameRef.current = 0
      onPreview(latestRef.current)
    })
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
    const [back, forward] = orientation === 'vertical' ? ['ArrowLeft', 'ArrowRight'] : ['ArrowUp', 'ArrowDown']
    const direction = event.key === forward ? 1 : event.key === back ? -1 : 0
    if (!direction) return
    event.preventDefault()
    const next = clamp(latestRef.current + direction * (reversed ? -1 : 1) * step * (event.shiftKey ? 5 : 1))
    latestRef.current = next
    onCommit(next)
  }

  const vertical = orientation === 'vertical'
  return <div className={`relative z-20 shrink-0 ${vertical ? 'w-0' : 'h-px bg-[var(--border)]'}`}>
    <div
      role="separator"
      aria-orientation={orientation}
      aria-label={label}
      aria-valuemin={0}
      aria-valuemax={100}
      aria-valuenow={Math.round((value - min) / (max - min) * 100)}
      tabIndex={0}
      title="Drag to resize · Double-click to reset"
      onPointerDown={startDrag}
      onPointerMove={moveDrag}
      onPointerUp={finishDrag}
      onPointerCancel={finishDrag}
      onDoubleClick={() => {
        latestRef.current = defaultValue
        onCommit(defaultValue)
      }}
      onKeyDown={nudge}
      onPointerEnter={() => setHovered(true)}
      onPointerLeave={() => setHovered(false)}
      onFocus={() => setFocused(true)}
      onBlur={() => setFocused(false)}
      className={`absolute touch-none transition-colors focus-visible:outline-none ${vertical
        ? 'inset-y-0 -left-[3px] w-[7px] cursor-col-resize'
        : 'inset-x-0 -top-[3px] h-[7px] cursor-row-resize'}`}
      style={{ backgroundColor: dragging || hovered || focused ? 'color-mix(in srgb, var(--border-focus) 40%, transparent)' : undefined }}
    />
  </div>
}
