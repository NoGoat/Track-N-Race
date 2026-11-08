import { app, BrowserWindow, ipcMain } from 'electron'
import * as fs from 'fs'
import * as path from 'path'
import { configStore as store, configWasEmpty } from './configStore'
import { allowThroughFirewall, getFirewallStatus, listNetworkInterfaces, udpProbe } from './bridgeManager'

// First-time setup runs once, on a fresh install. Launching with
// --first-time-setup (or TNR_FIRST_TIME_SETUP=1) shows it again on any install:
//   npm start -- --first-time-setup
//   "Track-N-Race - Electron.exe" --first-time-setup
export const FIRST_TIME_SETUP_SWITCH = '--first-time-setup'
const SETUP_COMPLETED_KEY = 'setup.completed'

export function hasFirstTimeSetupSwitch(argv: readonly string[]): boolean {
  return argv.some(arg => arg.toLowerCase() === FIRST_TIME_SETUP_SWITCH)
}

// Installs that already had settings before this version are treated as set up,
// so only fresh installs see the wizard on their own.
if (!store.has(SETUP_COMPLETED_KEY)) store.set(SETUP_COMPLETED_KEY, !configWasEmpty)

let forced = hasFirstTimeSetupSwitch(process.argv) || process.env['TNR_FIRST_TIME_SETUP'] === '1'
if (forced) console.log('[setup] first-time setup requested by launch switch')

export interface FirstTimeSetupState {
  show: boolean
  forced: boolean
  defaultRecordingDirectory: string
}

function setupState(): FirstTimeSetupState {
  return {
    show: forced || store.get(SETUP_COMPLETED_KEY) === false,
    forced,
    defaultRecordingDirectory: path.join(app.getPath('documents'), 'Track N Race', 'Recordings'),
  }
}

// A second launch with the switch reopens setup in the running window.
export function requestFirstTimeSetup(win: BrowserWindow): void {
  forced = true
  if (!win.isDestroyed()) win.webContents.send('setup:open', setupState())
}

// Free bytes on the volume holding `directory`, walking up to the nearest
// folder that exists. null when it can't be read.
async function freeBytes(directory: string): Promise<number | null> {
  let current = path.resolve(directory)
  for (;;) {
    try {
      const stats = await fs.promises.statfs(current)
      return Number(stats.bavail) * Number(stats.bsize)
    } catch {
      const parent = path.dirname(current)
      if (parent === current) return null
      current = parent
    }
  }
}

export function registerFirstTimeSetupIpc(): void {
  ipcMain.on('setup:get-state', event => { event.returnValue = setupState() })
  ipcMain.handle('setup:complete', () => {
    forced = false
    store.set(SETUP_COMPLETED_KEY, true)
  })
  ipcMain.handle('setup:network-interfaces', () => listNetworkInterfaces())
  ipcMain.handle('setup:firewall-status', () => {
    const port = Number(store.get('udp.port', 20777))
    return getFirewallStatus(Number.isInteger(port) && port >= 1 && port <= 65535 ? port : 20777)
  })
  ipcMain.handle('setup:firewall-allow', () => allowThroughFirewall())
  ipcMain.handle('setup:udp-probe', () => udpProbe())
  ipcMain.handle('setup:free-bytes', (_event, directory: unknown) =>
    typeof directory === 'string' && directory ? freeBytes(directory) : null)
  ipcMain.handle('setup:ensure-directory', async (_event, directory: unknown) => {
    if (typeof directory !== 'string' || !directory) return false
    try {
      await fs.promises.mkdir(directory, { recursive: true })
      return true
    } catch (error) {
      console.error('[setup] could not create recording folder:', error)
      return false
    }
  })
}
