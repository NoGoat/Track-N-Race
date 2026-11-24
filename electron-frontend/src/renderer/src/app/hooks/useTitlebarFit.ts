import { useCallback, useEffect, useLayoutEffect, useRef, useState, type RefObject } from 'react'

/**
 * How far the title bar has had to give way to fit everything in:
 *  0 - nothing hidden
 *  1 - the "Track N Race" title is hidden
 *  2 - also, the two-driver picker shows three-letter codes instead of names
 */
export type TitlebarFit = 0 | 1 | 2

// Room the spacer must have beyond what a step frees before that step is
// undone, so the title bar does not flick back and forth at the boundary.
const HYSTERESIS_PX = 8
// The gap between the app icon and the title.
const TITLE_GAP_PX = 8

/**
 * The race-event banner in the middle of the title bar gets the room first as
 * the window widens: the file chips' track names, the title and the full
 * driver names only widen once it has this much.
 */
export const EVENTS_MIN_PX = 200

/**
 * Flex for the middle gap (`data-titlebar-spacer`): it starts at the banner's
 * width plus its padding and gives way far ahead of the squeezable parts, so
 * it is the first to shrink and the first to grow back.
 */
export const TITLEBAR_SPACER_STYLE = { flex: `1 1000 ${EVENTS_MIN_PX + 16}px` } as const

/**
 * Steps the title bar down one level at a time while its contents do not fit,
 * and back up once the room a step freed is spare again. The header marks the
 * parts this reads:
 *  - `data-titlebar-spacer`: the flexible gap in the middle that holds the
 *    race-event banner; a step is undone only if the banner keeps
 *    EVENTS_MIN_PX afterwards.
 *  - `data-titlebar-squeeze`: parts that shrink before overflowing (the file
 *    chips' track names); one narrower than its content (or its max-width) is
 *    being squeezed.
 *  - `data-titlebar-title-measure`: the title's text, measurable while hidden.
 *  - `data-titlebar-drivers-full` / `data-titlebar-drivers-label`: the driver
 *    picker's full-name label (measured, not shown) and the one it shows.
 *
 * `maxLevel` caps the steps (0 turns this off).
 */
export function useTitlebarFit(headerRef: RefObject<HTMLElement | null>, maxLevel: TitlebarFit): TitlebarFit {
  const [level, setLevel] = useState<TitlebarFit>(0)
  const effective = Math.min(level, maxLevel) as TitlebarFit
  const effectiveRef = useRef(effective)
  effectiveRef.current = effective
  const maxRef = useRef(maxLevel)
  maxRef.current = maxLevel

  const check = useCallback(() => {
    const header = headerRef.current
    const current = effectiveRef.current
    const max = maxRef.current
    if (!header || max === 0) return
    const overflowing = header.scrollWidth > header.clientWidth + 1 || isSqueezed(header)
    if (overflowing) {
      if (current < max) setLevel((current + 1) as TitlebarFit)
      return
    }
    if (current === 0) return
    const spacer = header.querySelector<HTMLElement>('[data-titlebar-spacer]')
    if (!spacer) return
    const style = getComputedStyle(spacer)
    const spare = spacer.clientWidth - (parseFloat(style.paddingLeft) || 0) - (parseFloat(style.paddingRight) || 0)
    if (spare > freedBy(header, current) + EVENTS_MIN_PX + HYSTERESIS_PX) setLevel((current - 1) as TitlebarFit)
  }, [headerRef])

  // After every render: the header's own contents may have changed.
  useLayoutEffect(() => { check() })

  // Any part of the header changing size (the window, an animated selector,
  // a file chip) is a reason to look again.
  const frameRef = useRef(0)
  useEffect(() => {
    const header = headerRef.current
    if (!header || maxLevel === 0) return
    const observer = new ResizeObserver(() => {
      cancelAnimationFrame(frameRef.current)
      frameRef.current = requestAnimationFrame(check)
    })
    observer.observe(header)
    for (const child of Array.from(header.children)) observer.observe(child)
    for (const part of Array.from(header.querySelectorAll('[data-titlebar-squeeze]'))) observer.observe(part)
    return () => {
      observer.disconnect()
      cancelAnimationFrame(frameRef.current)
    }
  })

  return effective
}

function isSqueezed(header: HTMLElement): boolean {
  for (const part of Array.from(header.querySelectorAll<HTMLElement>('[data-titlebar-squeeze]'))) {
    const cap = parseFloat(getComputedStyle(part).maxWidth)
    const wanted = Number.isFinite(cap) ? Math.min(part.scrollWidth, cap) : part.scrollWidth
    if (part.clientWidth + 1 < wanted) return true
  }
  return false
}

/** The width undoing `level` would take back. */
function freedBy(header: HTMLElement, level: TitlebarFit): number {
  if (level === 1) {
    const title = header.querySelector<HTMLElement>('[data-titlebar-title-measure]')
    const width = title?.offsetWidth ?? 0
    return width > 0 ? width + TITLE_GAP_PX : 0
  }
  const full = header.querySelector<HTMLElement>('[data-titlebar-drivers-full]')
  const shown = header.querySelector<HTMLElement>('[data-titlebar-drivers-label]')
  return Math.max(0, (full?.offsetWidth ?? 0) - (shown?.offsetWidth ?? 0))
}
