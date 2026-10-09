import { initializeHost, startEngine } from './ipc'
async function start() {
  await initializeHost()
  await import('./bridges')
  const { mounted } = await import('../renderer/src/main')
  await mounted
  await startEngine()
}
void start().catch(error => {
  console.error('Tauri startup failed', error)
  const root = document.getElementById('root')!
  const title = document.createElement('h1')
  title.textContent = 'Track N Race could not start'
  const details = document.createElement('pre')
  details.textContent = String(error)
  root.replaceChildren(title, details)
})
