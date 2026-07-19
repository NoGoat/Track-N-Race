import { useEffect, useRef } from 'react'

interface TimedSample { session_time: number }

// Temporary diagnostic for comparing the actual data pressure presented to
// each live chart. Logs on data publications rather than animation-frame draws.
export function useChartDataProfiler(label: string, data: readonly TimedSample[]): void {
  const initialLastTime = data.length > 0 ? data[data.length - 1].session_time : Number.NEGATIVE_INFINITY
  const statsRef = useRef({
    startedAt: import.meta.env.DEV ? performance.now() : 0,
    updates: 0,
    newTimestamps: 0,
    previousLastTime: initialLastTime,
  })

  useEffect(() => {
    if (!import.meta.env.DEV) return
    const stats = statsRef.current
    stats.updates++

    const lastTime = data.length > 0 ? data[data.length - 1].session_time : Number.NEGATIVE_INFINITY
    if (Number.isFinite(lastTime) && lastTime > stats.previousLastTime) {
      // Count timestamps added since the preceding publication. Searching from
      // the end keeps the normal one-new-row case O(1).
      let added = 0
      for (let i = data.length - 1; i >= 0 && data[i].session_time > stats.previousLastTime; i--) added++
      stats.newTimestamps += added
      stats.previousLastTime = lastTime
    }

    const now = performance.now()
    const elapsed = now - stats.startedAt
    if (elapsed < 2000) return

    let duplicates = 0
    let backwards = 0
    for (let i = 1; i < data.length; i++) {
      const delta = data[i].session_time - data[i - 1].session_time
      if (delta === 0) duplicates++
      else if (delta < 0) backwards++
    }

    const first = data.length > 0 ? data[0].session_time : 0
    const last = data.length > 0 ? data[data.length - 1].session_time : 0
    const span = Math.max(0, last - first)
    const density = span > 0 ? (data.length - 1) / span : 0
    const seconds = elapsed / 1000

    // eslint-disable-next-line no-console
    console.log(
      `[perf:data] ${label}: samples=${data.length} span=${span.toFixed(2)}s density=${density.toFixed(1)}/s ` +
      `updates=${stats.updates} (${(stats.updates / seconds).toFixed(1)}/s) ` +
      `newTimestamps=${stats.newTimestamps} (${(stats.newTimestamps / seconds).toFixed(1)}/s) ` +
      `duplicates=${duplicates} backwards=${backwards}`
    )

    stats.startedAt = now
    stats.updates = 0
    stats.newTimestamps = 0
  }, [data, label])
}
