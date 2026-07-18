import { createRoot } from 'react-dom/client'
import { scan } from 'react-scan'
import './index.css'
import App from './App'

if (import.meta.env.DEV) {
  scan({ enabled: true })
}

document.addEventListener('visibilitychange', () => {
  window.playerBridge.setPageVisible(document.visibilityState === 'visible')
})

createRoot(document.getElementById('root')!).render(
  <App />,
)
