import { useEffect, useLayoutEffect, useRef, useState } from 'react'

export const MODAL_EXIT_MS = 180

function animationsAreReduced(): boolean {
  return document.documentElement.dataset.reduceAnimations === 'true'
}

interface ModalPresenceOptions {
  /**
   * Keep this enabled for dialogs that should animate when first mounted open.
   * Persistent view selectors can disable it so a parent View Transition
   * snapshots their already-selected state instead of their enter frame.
   */
  animateInitialEnter?: boolean
}

export function useModalPresence(
  open: boolean,
  exitMs = MODAL_EXIT_MS,
  { animateInitialEnter = true }: ModalPresenceOptions = {},
) {
  const [mounted, setMounted] = useState(open)
  const [visible, setVisible] = useState(() => open && !animateInitialEnter)
  const exitTimerRef = useRef<number | null>(null)

  useLayoutEffect(() => {
    if (open) setMounted(true)
  }, [open])

  useEffect(() => {
    if (exitTimerRef.current !== null) {
      window.clearTimeout(exitTimerRef.current)
      exitTimerRef.current = null
    }

    if (open) {
      const frame = window.requestAnimationFrame(() => setVisible(true))
      return () => window.cancelAnimationFrame(frame)
    }

    setVisible(false)
    if (!mounted) return

    if (animationsAreReduced()) {
      setMounted(false)
      return
    }

    exitTimerRef.current = window.setTimeout(() => {
      setMounted(false)
      exitTimerRef.current = null
    }, exitMs)

    return () => {
      if (exitTimerRef.current !== null) {
        window.clearTimeout(exitTimerRef.current)
        exitTimerRef.current = null
      }
    }
  }, [exitMs, mounted, open])

  return { mounted, visible }
}

export function useModalPresenceValue<T>(value: T | null) {
  const lastValueRef = useRef<T | null>(value)
  const presence = useModalPresence(value !== null)
  if (value !== null) lastValueRef.current = value

  return {
    ...presence,
    value: lastValueRef.current,
  }
}
