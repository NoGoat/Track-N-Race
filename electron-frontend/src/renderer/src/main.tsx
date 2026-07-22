import './index.css'

// React 19's development build emits User Timing measures for component
// renders. This 60–120 Hz UI can otherwise retain tens of thousands of
// structured PerformanceEntry details in Chromium and exhaust renderer memory.
if (import.meta.env.DEV) {
  window.setInterval(() => {
    performance.clearMeasures()
    performance.clearMarks()
  }, 250)
}

document.addEventListener('visibilitychange', () => {
  window.playerBridge.setPageVisible(document.visibilityState === 'visible')
})

async function bootstrap(): Promise<void> {
  // React Scan is always available while developing. Production bundles only
  // include it when the release-candidate build explicitly opts in.
  if (__ENABLE_PERFORMANCE_DIAGNOSTICS__) {
    const [{ scan }, { installStatsGlDiagnostics }] = await Promise.all([
      import('react-scan'),
      import('./diagnostics/statsGlDiagnostics'),
    ])
    scan({
      enabled: true,
      showToolbar: true,
      dangerouslyForceRunInProduction: __ENABLE_PERFORMANCE_DIAGNOSTICS__,
    })
    installStatsGlDiagnostics()
  }

  // React Scan must install its instrumentation hook before React DOM loads.
  const [{ createRoot }, { default: App }] = await Promise.all([
    import('react-dom/client'),
    import('./App'),
  ])

  createRoot(document.getElementById('root')!).render(
    <App />,
  )
}

void bootstrap()
