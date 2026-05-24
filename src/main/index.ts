import { app, BrowserWindow, shell, ipcMain, Menu, globalShortcut, nativeTheme, dialog } from 'electron'
import * as path from 'path'
import * as fs from 'fs'
import { join } from 'path'
import { execSync } from 'child_process'
import Store from 'electron-store'
import { startUdpReceiver, stopUdpReceiver } from './udpReceiver'
import { setOverride, getProtocolConfig } from './protocolDispatcher'
import type { ProtocolOverride } from './protocolDispatcher'
import { initSessionRecorder } from './sessionRecorder'
import { loadFile, play, pause, seek, setSpeed, closePlayer, setOnPlayerStateChange } from './sessionPlayer'

import iconTransparent from '../../build/icon_transparent.ico?asset'
import iconTransparentLight from '../../build/icon_transparent_light.ico?asset'

const store = new Store()

// Helper to extract .tnrd or .trnd file paths from command-line arguments
function getFilePathFromArgs(argv: string[]): string | null {
  for (const arg of argv) {
    const lower = arg.toLowerCase()
    if (lower.endsWith('.tnrd') || lower.endsWith('.trnd')) {
      try {
        if (fs.existsSync(arg)) {
          return path.resolve(arg)
        }
      } catch (e) {}
    }
  }
  return null
}

// Global tracking of file path to open on startup
let startupFilePath = getFilePathFromArgs(process.argv)
let macStartupFilePath: string | null = null

async function openTelemetryFile(filePath: string): Promise<boolean> {
  stopUdpReceiver() // Suspend live UDP
  const success = await loadFile(filePath)
  if (!success) {
    startUdpReceiver({
      port: store.get('udp.port', 20777) as number,
      bindAddress: store.get('udp.bindAddress', '0.0.0.0') as string,
    })
  }
  return success
}

async function promptAndOpenTelemetryFile(win: BrowserWindow, filePath: string) {
  if (win && !win.isDestroyed()) {
    if (win.isMinimized()) win.restore()
    win.focus()

    const { response } = await dialog.showMessageBox(win, {
      type: 'question',
      buttons: ['Yes', 'No'],
      defaultId: 0,
      cancelId: 1,
      title: 'Open Session File',
      message: 'Are you sure you want to open this file?',
      detail: 'Opening it will stop the event bridge and if you have an session right now with the game, it will be closed.',
      noLink: true
    })

    if (response === 0) {
      await openTelemetryFile(filePath)
    }
  }
}

// Single Instance Lock
const gotSingleInstanceLock = app.requestSingleInstanceLock()
if (!gotSingleInstanceLock) {
  app.quit()
} else {
  app.on('second-instance', (_event, commandLine) => {
    const win = BrowserWindow.getAllWindows()[0]
    if (win) {
      const filePath = getFilePathFromArgs(commandLine)
      if (filePath) {
        promptAndOpenTelemetryFile(win, filePath)
      } else {
        if (win.isMinimized()) win.restore()
        win.focus()
      }
    }
  })
}

// Support drag/drop or open-file events on macOS for completeness
app.on('open-file', (event, filePath) => {
  event.preventDefault()
  if (app.isReady()) {
    const win = BrowserWindow.getAllWindows()[0]
    if (win) {
      promptAndOpenTelemetryFile(win, filePath)
    } else {
      openTelemetryFile(filePath)
    }
  } else {
    macStartupFilePath = filePath
  }
})

export function broadcastToWindows(row: Record<string, unknown>): void {
  for (const win of BrowserWindow.getAllWindows()) {
    if (!win.isDestroyed()) win.webContents.send('telemetry', row)
  }
}

ipcMain.on('store-get', (event, key: string, defaultValue: unknown) => {
  event.returnValue = store.get(key, defaultValue)
})

ipcMain.on('store-set', (_event, key: string, value: unknown) => {
  store.set(key, value)
})

ipcMain.handle('dialog:showOpenDialog', async () => {
  const { canceled, filePaths } = await dialog.showOpenDialog({
    properties: ['openDirectory']
  })
  if (canceled) {
    return null
  } else {
    return filePaths[0]
  }
})

ipcMain.handle('dialog:showOpenDialogTNRD', async () => {
  const { canceled, filePaths } = await dialog.showOpenDialog({
    filters: [{ name: 'Track N Race Data', extensions: ['tnrd', 'trnd'] }],
    properties: ['openFile']
  })
  if (canceled) return null
  return filePaths[0]
})

// Player IPC
ipcMain.handle('player:load', async (_event, filePath: string) => {
  return openTelemetryFile(filePath)
})

ipcMain.on('player:play', () => play())
ipcMain.on('player:pause', () => pause())
ipcMain.on('player:seek', (_event, pct: number) => seek(pct))
ipcMain.on('player:setSpeed', (_event, mult: number) => setSpeed(mult))
ipcMain.on('player:close', () => {
  closePlayer()
  startUdpReceiver({
    port: store.get('udp.port', 20777) as number,
    bindAddress: store.get('udp.bindAddress', '0.0.0.0') as string,
  })
})

ipcMain.on('udp-restart', (event) => {
  try {
    stopUdpReceiver()
    startUdpReceiver({
      port:        store.get('udp.port', 20777) as number,
      bindAddress: store.get('udp.bindAddress', '0.0.0.0') as string,
    })
    event.reply('udp-restart-result', { ok: true })
  } catch (err) {
    event.reply('udp-restart-result', { ok: false, error: String(err) })
  }
})

ipcMain.handle('protocol-get-config', () => {
  return getProtocolConfig()
})

ipcMain.on('protocol-set-override', (_event, value: ProtocolOverride) => {
  setOverride(value)
})

function getWindowsTaskbarThemeSync(): 'light' | 'dark' {
  if (process.platform !== 'win32') return 'dark'
  try {
    const stdout = execSync(
      'reg query HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize /v SystemUsesLightTheme',
      { encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] }
    )
    const match = /SystemUsesLightTheme\s+REG_DWORD\s+(0x\d+)/.exec(stdout)
    if (match) {
      const value = parseInt(match[1], 16)
      return value === 1 ? 'light' : 'dark'
    }
  } catch (e) {
    // Ignore error, fallback to dark
  }
  return 'dark'
}

function createWindow(): void {
  const taskbarTheme = getWindowsTaskbarThemeSync()
  const iconPath = taskbarTheme === 'light' ? iconTransparentLight : iconTransparent

  const win = new BrowserWindow({
    width: 1600,
    height: 900,
    minWidth: 1200,
    minHeight: 700,
    backgroundColor: '#0f172a',
    frame: false,
    icon: iconPath,
    webPreferences: {
      preload: join(__dirname, '../preload/index.js'),
      sandbox: false,
    },
  })

  win.on('ready-to-show', () => win.show())

  ipcMain.on('window-minimize', () => {
    if (win.isFullScreen()) {
      win.once('leave-full-screen', () => win.minimize())
      win.setFullScreen(false)
    } else {
      win.minimize()
    }
  })
  ipcMain.on('window-maximize', () => {
    if (win.isFullScreen()) {
      win.setFullScreen(false)
    } else if (win.isMaximized()) {
      win.unmaximize()
    } else {
      win.maximize()
    }
  })
  ipcMain.on('window-close',       () => win.close())
  ipcMain.on('window-fullscreen',  () => win.setFullScreen(!win.isFullScreen()))

  win.on('maximize',          () => win.webContents.send('window-maximized', true))
  win.on('unmaximize',        () => win.webContents.send('window-maximized', false))
  win.on('enter-full-screen', () => win.webContents.send('window-fullscreen-changed', true))
  win.on('leave-full-screen', () => win.webContents.send('window-fullscreen-changed', false))

  win.webContents.setWindowOpenHandler(({ url }) => {
    shell.openExternal(url)
    return { action: 'deny' }
  })

  setOnPlayerStateChange((state) => {
    if (!win.isDestroyed()) {
      win.webContents.send('playback_state', state)
    }
  })

  win.webContents.on('did-finish-load', () => {
    const fileToOpen = startupFilePath || macStartupFilePath
    if (fileToOpen) {
      openTelemetryFile(fileToOpen)
      startupFilePath = null
      macStartupFilePath = null
    }
  })

  // In dev, electron-vite sets ELECTRON_RENDERER_URL
  if (process.env['ELECTRON_RENDERER_URL']) {
    win.loadURL(process.env['ELECTRON_RENDERER_URL'])
  } else {
    win.loadFile(join(__dirname, '../renderer/index.html'))
  }
}

app.whenReady().then(() => {
  if (process.platform === 'win32') {
    app.setAppUserModelId(app.isPackaged ? 'com.tracknrace' : process.execPath)
  }

  initSessionRecorder()

  Menu.setApplicationMenu(null)
  startUdpReceiver({
    port:        store.get('udp.port', 20777) as number,
    bindAddress: store.get('udp.bindAddress', '0.0.0.0') as string,
  })
  createWindow()

  // Track the current icon path to prevent redundant win.setIcon calls
  let lastIconPath = ''

  function updateTaskbarIconIfNeeded(): void {
    const taskbarTheme = getWindowsTaskbarThemeSync()
    const currentIconPath = taskbarTheme === 'light' ? iconTransparentLight : iconTransparent
    
    if (currentIconPath !== lastIconPath) {
      lastIconPath = currentIconPath
      for (const win of BrowserWindow.getAllWindows()) {
        if (!win.isDestroyed()) {
          win.setIcon(currentIconPath)
        }
      }
    }
  }

  // Initialize with startup icon theme
  const initialTheme = getWindowsTaskbarThemeSync()
  lastIconPath = initialTheme === 'light' ? iconTransparentLight : iconTransparent

  // Update on nativeTheme change
  nativeTheme.on('updated', () => {
    updateTaskbarIconIfNeeded()
  })

  // Poll fallback (1.5s interval) to guarantee detection of custom taskbar theme changes
  const pollInterval = setInterval(() => {
    updateTaskbarIconIfNeeded()
  }, 1500)

  app.on('will-quit', () => {
    closePlayer()
    clearInterval(pollInterval)
  })


  if (process.env['ELECTRON_RENDERER_URL']) {
    globalShortcut.register('Ctrl+Shift+I', () => {
      const focusedWindow = BrowserWindow.getFocusedWindow()

      if (focusedWindow) {
        if (focusedWindow.webContents.isDevToolsOpened()) {
          focusedWindow.webContents.closeDevTools()
        } else {
          focusedWindow.webContents.openDevTools()
        }
      }
    })
  }

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) createWindow()
  })
})

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') app.quit()
})
