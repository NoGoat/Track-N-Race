import { createRoot } from 'react-dom/client'
import { scan } from 'react-scan'
import 'uplot/dist/uPlot.min.css'
import './index.css'
import App from './App.tsx'

if (import.meta.env.DEV) {
  scan({ enabled: true })
}

document.addEventListener('visibilitychange', () => {
  window.playerBridge.setPageVisible(document.visibilityState === 'visible')
})

createRoot(document.getElementById('root')!).render(
  <App />,
)
