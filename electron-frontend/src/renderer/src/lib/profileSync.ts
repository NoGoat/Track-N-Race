// Bracket a synchronous region so the long-task observer can attribute a
// main-thread block to it without filling the performance measure buffer.
export function profileSync<T>(label: string, fn: () => T): T {
  if (!import.meta.env.DEV || typeof performance === 'undefined' || typeof performance.measure !== 'function') return fn()
  const start = performance.now()
  try {
    return fn()
  } finally {
    const duration = performance.now() - start
    if (duration >= 1) {
      try { performance.measure(`sync:${label}`, { start, duration }) } catch { /* buffer full / unsupported */ }
    }
  }
}
