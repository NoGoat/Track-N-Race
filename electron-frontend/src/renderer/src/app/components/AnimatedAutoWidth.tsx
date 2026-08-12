import { useLayoutEffect, useRef, useState } from 'react'

export default function AnimatedAutoWidth({
  measureKey, children, className = '',
}: {
  measureKey: string | number
  children: React.ReactNode
  className?: string
}) {
  const contentRef = useRef<HTMLDivElement>(null)
  const [width, setWidth] = useState<number | null>(null)

  useLayoutEffect(() => {
    const content = contentRef.current
    if (!content) return
    let frame = 0
    let disposed = false
    const measure = () => {
      frame = 0
      if (disposed) return
      // Measure the new value at its intrinsic width, then immediately return
      // the visible control to 100%. The outer wrapper owns the transition, so
      // the control background itself expands and contracts with that wrapper.
      const priorWidth = content.style.width
      content.style.width = 'max-content'
      const next = Math.ceil(content.getBoundingClientRect().width)
      content.style.width = priorWidth
      setWidth(current => current === next ? current : next)
    }
    const scheduleMeasure = () => {
      if (disposed) return
      if (frame) cancelAnimationFrame(frame)
      frame = requestAnimationFrame(measure)
    }
    measure()
    // Measure once more after React Select has settled its internal layout.
    // Do not continuously observe this node: an observer sees intermediate
    // widths during contraction and repeatedly retargets the transition.
    scheduleMeasure()
    const fontsReady = () => scheduleMeasure()
    const windowLoaded = () => scheduleMeasure()
    document.fonts?.ready.then(fontsReady)
    window.addEventListener('load', windowLoaded)
    return () => {
      disposed = true
      if (frame) cancelAnimationFrame(frame)
      window.removeEventListener('load', windowLoaded)
    }
  }, [measureKey])

  return (
    <div
      className={`titlebar-auto-width ${className}`}
      style={width === null ? undefined : { width }}
    >
      <div ref={contentRef} className="w-full">{children}</div>
    </div>
  )
}
