import { useLayoutEffect, useRef, useState } from 'react'

export function useSize(settleMs = 0) {
  const ref = useRef<HTMLDivElement>(null)
  const [size, setSize] = useState({ width: 0, height: 0 })
  useLayoutEffect(() => {
    if (!ref.current) return
    let timer: ReturnType<typeof setTimeout> | null = null
    let currentWidth = 0
    let currentHeight = 0
    let initialized = false
    const publish = (width: number, height: number) => {
      if (width === currentWidth && height === currentHeight) return
      currentWidth = width
      currentHeight = height
      setSize({ width, height })
    }
    const ro = new ResizeObserver(([e]) => {
      const width = Math.floor(e.contentRect.width)
      const height = Math.floor(e.contentRect.height)
      // Chart canvases already fill their host through CSS. During an active
      // window drag, leave their backing stores alone and publish only the
      // settled size; otherwise each observer tick reallocates three large
      // canvases and rerenders the owning React component. The first measured
      // size is immediate so mount layout is never delayed.
      if (!initialized || settleMs <= 0) {
        initialized = true
        publish(width, height)
        return
      }
      if (timer !== null) clearTimeout(timer)
      timer = setTimeout(() => {
        timer = null
        publish(width, height)
      }, settleMs)
    })
    ro.observe(ref.current)
    return () => {
      ro.disconnect()
      if (timer !== null) clearTimeout(timer)
    }
  }, [settleMs])
  return { ref, ...size }
}
