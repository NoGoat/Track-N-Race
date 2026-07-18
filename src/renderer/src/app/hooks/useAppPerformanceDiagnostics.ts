import { useCallback, useEffect, useLayoutEffect, useRef } from 'react'
import type { Tab } from '../appConfig'

export function useAppPerformanceDiagnostics(tab: Tab) {
  const tabRef = useRef(tab)
  tabRef.current = tab

  useEffect(() => {
    if (typeof PerformanceObserver === 'undefined') return
    let observer: PerformanceObserver | null = null
    try {
      observer = new PerformanceObserver(list => {
        for (const entry of list.getEntries()) {
          const end = entry.startTime + entry.duration
          const overlapping = (performance.getEntriesByType('measure') as PerformanceMeasure[])
            .filter(measure => measure.name.startsWith('sync:')
              && measure.startTime < end
              && measure.startTime + measure.duration > entry.startTime)
            .sort((a, b) => b.duration - a.duration)
          const attribution = overlapping.length
            ? overlapping.slice(0, 6).map(measure => `${measure.name.slice(5)}=${measure.duration.toFixed(1)}ms`).join(', ')
            : 'no sync region (browser-internal: GC / paint / layout)'
          console.log(`[perf] longtask ${entry.duration.toFixed(1)}ms on tab="${tabRef.current}" @ ${entry.startTime.toFixed(0)}ms → ${attribution}`)
        }
        try { performance.clearMeasures() } catch { /* ignore */ }
      })
      observer.observe({ entryTypes: ['longtask'] })
    } catch { /* longtask not supported */ }
    return () => observer?.disconnect()
  }, [])

  const renderStartRef = useRef(0)
  renderStartRef.current = performance.now()
  const renderCountRef = useRef(0)
  const renderCostRef = useRef(0)
  const renderLogRef = useRef(performance.now())
  renderCountRef.current++

  useLayoutEffect(() => {
    const duration = performance.now() - renderStartRef.current
    renderCostRef.current += duration
    if (duration >= 1 && typeof performance.measure === 'function') {
      try { performance.measure('sync:react-render+commit', { start: renderStartRef.current, duration }) } catch { /* ignore */ }
    }
    const now = performance.now()
    const elapsed = now - renderLogRef.current
    if (elapsed >= 2000) {
      console.log(`[perf] App renders=${renderCountRef.current} (${(renderCountRef.current / elapsed * 1000).toFixed(0)}/s) total-render+commit=${renderCostRef.current.toFixed(0)}ms/${(elapsed / 1000).toFixed(1)}s tab="${tabRef.current}"`)
      renderCountRef.current = 0
      renderCostRef.current = 0
      renderLogRef.current = now
    }
  })

  const onAppRender = useCallback((_id: string, phase: string, actualDuration: number, baseDuration: number) => {
    if (actualDuration >= 5) {
      console.log(`[perf] react-render-phase ${phase} actual=${actualDuration.toFixed(1)}ms base=${baseDuration.toFixed(1)}ms tab="${tabRef.current}"`)
    }
  }, [])

  const measureAppBody = useCallback(() => {
    if (typeof performance !== 'undefined' && typeof performance.measure === 'function') {
      const duration = performance.now() - renderStartRef.current
      if (duration >= 1) {
        try { performance.measure('sync:app-body', { start: renderStartRef.current, duration }) } catch { /* ignore */ }
      }
    }
  }, [])

  return { onAppRender, measureAppBody }
}
