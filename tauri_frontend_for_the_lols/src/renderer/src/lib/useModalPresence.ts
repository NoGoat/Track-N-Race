import { useEffect, useRef, useState } from 'react'

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
  const transitionTargetRef = useRef<HTMLDivElement>(null)

  // Adjusted during render so an opening dialog mounts in the same commit and
  // a closing one starts its exit transition without an extra frame.
  if (open && !mounted) setMounted(true)
  if (!open && visible) setVisible(false)
  if (!open && mounted && animationsAreReduced()) setMounted(false)

  useEffect(() => {
    if (exitTimerRef.current !== null) {
      window.clearTimeout(exitTimerRef.current)
      exitTimerRef.current = null
    }

    if (open) {
      let cancelled = false
      let playFrame = 0
      let settleFrame = 0
      const prepareFrame = window.requestAnimationFrame(() => {
        const transitionTarget = transitionTargetRef.current
        if (transitionTarget) {
          // First commit the closed styles, then instantiate the entrance
          // transitions and hold them at frame zero. Chromium's compositor is
          // cold until the app has animated something; without the pause, its
          // startup work can consume the entire short entrance transition.
          void transitionTarget.offsetWidth
          transitionTarget.dataset.state = 'open'
          void transitionTarget.offsetWidth

          const entranceAnimations = transitionTarget.getAnimations({ subtree: true })
          for (const animation of entranceAnimations) {
            animation.pause()
            animation.currentTime = 0
          }

          setVisible(true)
          if (entranceAnimations.length > 0) {
            void Promise.all(entranceAnimations.map(animation => animation.ready.catch(() => animation))).then(() => {
              if (cancelled) return
              settleFrame = window.requestAnimationFrame(() => {
                playFrame = window.requestAnimationFrame(() => {
                  if (cancelled) return
                  for (const animation of entranceAnimations) {
                    if (animation.playState === 'paused') animation.play()
                  }
                })
              })
            })
          }
          return
        }
        setVisible(true)
      })
      return () => {
        cancelled = true
        window.cancelAnimationFrame(prepareFrame)
        window.cancelAnimationFrame(settleFrame)
        window.cancelAnimationFrame(playFrame)
      }
    }

    if (!mounted) return

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

  return { mounted, visible, transitionTargetRef }
}

export function useModalPresenceValue<T>(value: T | null) {
  // The last non-null value keeps the dialog's content on screen while it exits.
  const [lastValue, setLastValue] = useState<T | null>(value)
  const presence = useModalPresence(value !== null)
  if (value !== null && value !== lastValue) setLastValue(value)

  return {
    ...presence,
    value: value ?? lastValue,
  }
}
