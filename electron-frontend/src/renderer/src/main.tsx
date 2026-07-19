import { createRoot } from 'react-dom/client'
import './index.css'
import App from './App'

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

createRoot(document.getElementById('root')!).render(
  <App />,
)
