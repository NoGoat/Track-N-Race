import './index.css'
import { getDebugSettings } from './lib/debugSettings'

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
  // React Scan and WebGL stats are opt-in because they add instrumentation to
  // the render path. They must install before React DOM loads, so this part of
  // the Additional logging setting takes effect on the next application launch.
  if (getDebugSettings().additionalLogging) {
    const [{ scan }, { installStatsGlDiagnostics }] = await Promise.all([
      import('react-scan'),
      import('./diagnostics/statsGlDiagnostics'),
    ])
    scan({
      enabled: true,
      showToolbar: true,
      dangerouslyForceRunInProduction: true,
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
