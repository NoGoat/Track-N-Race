import { app, BrowserWindow, shell, ipcMain, Menu, Tray, nativeTheme, nativeImage, dialog, screen } from 'electron'
import * as path from 'path'
import * as fs from 'fs'
import { join } from 'path'
import { execSync, spawn } from 'child_process'
import Store from 'electron-store'
import {
  setOverride,
  restartUdp,
  startBridge,
  stopBridge,
  getProtocolConfig,
  requestStatus,
  setRendererVisible,
  exportSessionXlsx,
  playerLoad,
  playerPlay,
  playerPause,
  playerSeek,
  playerSetSpeed,
  playerGetLapData,
  playerClose,
  setOnPlaybackState,
  getActiveFilePath,
  sweepTempFiles
} from './bridgeManager'

type ProtocolOverride = 'auto' | 'f1_24' | 'f1_25' | 'f1_26'

import iconTransparent from '../../build/icon_transparent.ico?asset'
import iconTransparentLight from '../../build/icon_transparent_light.ico?asset'
import iconTransparentPng from '../../build/icon_transparent.png?asset'
import iconTransparentLightPng from '../../build/icon_transparent_light.png?asset'

console.log('[main] module loading, pid=', process.pid)

process.on('uncaughtException', (err) => {
  console.error('[main] uncaughtException:', err)
})
process.on('unhandledRejection', (reason) => {
  console.error('[main] unhandledRejection:', reason)
})

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

// Mirrors createWindow()'s local `win` so the tray's click/menu handlers can
// reach the window outside that function's closure.
let mainWindow: BrowserWindow | null = null
let tray: Tray | null = null

function showWindow(): void {
  if (!mainWindow) return
  mainWindow.show()
  mainWindow.focus()
}

async function openTelemetryFile(filePath: string): Promise<boolean> {
  // The bridge switches to playback mode itself (it ignores live UDP while a clip
  // is loaded), so there's no separate live-suspend step here.
  return playerLoad(filePath)
}

// Single Instance Lock
const gotSingleInstanceLock = app.requestSingleInstanceLock()
console.log('[main] gotSingleInstanceLock =', gotSingleInstanceLock)
if (!gotSingleInstanceLock) {
  console.log('[main] another instance holds the lock — quitting')
  app.quit()
  process.exit(0) // Instantly kill the secondary process to prevent the second window flash!
} else {
  app.on('second-instance', (_event, commandLine) => {
    const win = BrowserWindow.getAllWindows()[0]
    if (win) {
      if (win.isMinimized()) win.restore()
      if (!win.isVisible()) win.show()
      win.focus()

      const filePath = getFilePathFromArgs(commandLine)
      if (filePath) {
        win.webContents.send('player:request-open-confirm', filePath)
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
      if (win.isMinimized()) win.restore()
      win.focus()
      win.webContents.send('player:request-open-confirm', filePath)
    } else {
      openTelemetryFile(filePath)
    }
  } else {
    macStartupFilePath = filePath
  }
})

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

// Player IPC — all forwarded to the bridge over stdin commands.
ipcMain.handle('player:load', async (_event, filePath: string) => {
  return openTelemetryFile(filePath)
})

// Pause forwarding the hot telemetry channels while the renderer is
// hidden/minimized/occluded (playback/recording keep running); resume on
// refocus. Prevents a background-throttled renderer from buffering an IPC
// backlog that janks badly when brought back to focus.
ipcMain.on('page-visibility', (_e, visible: boolean) => setRendererVisible(visible))
ipcMain.on('player:play', () => playerPlay())
ipcMain.on('player:pause', () => playerPause())
ipcMain.on('player:seek', (_event, pct: number) => playerSeek(pct))
ipcMain.on('player:setSpeed', (_event, mult: number) => playerSetSpeed(mult))
ipcMain.on('player:getLapData', (_event, lapNum: number) => playerGetLapData(lapNum))
ipcMain.on('player:close', () => playerClose())

ipcMain.handle('player:export-xlsx', async (event) => {
  const srcPath = getActiveFilePath()
  if (!srcPath) return { ok: false, error: 'No session loaded' }

  const base = path.basename(srcPath).replace(/\.(tnrd|trnd)$/i, '')
  const { canceled, filePath } = await dialog.showSaveDialog({
    title: 'Export Session to Excel',
    defaultPath: path.join(path.dirname(srcPath), `${base}.xlsx`),
    filters: [{ name: 'Excel Workbook', extensions: ['xlsx'] }]
  })
  if (canceled || !filePath) return { ok: false, error: 'cancelled' }

  const sender = event.sender
  return exportSessionXlsx(srcPath, filePath, (pct, stage) => {
    if (!sender.isDestroyed()) sender.send('player:export-progress', pct, stage)
  })
})

ipcMain.on('udp-restart', (event) => {
  try {
    restartUdp()
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

ipcMain.on('protocol-request-status', () => {
  requestStatus()
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
  console.log('[main] createWindow() start')
  const taskbarTheme = getWindowsTaskbarThemeSync()
  const iconPath = taskbarTheme === 'light' ? iconTransparentLight : iconTransparent
  const useNativeTitlebar = store.get('nativeTitlebar', false) as boolean

  // Size the window to 60% of the primary display's work area. workAreaSize is
  // in DIPs, so this already accounts for the display's scale factor.
  const { workAreaSize } = screen.getPrimaryDisplay()
  const width = Math.max(1200, Math.round(workAreaSize.width * 0.6))
  const height = Math.max(700, Math.round(workAreaSize.height * 0.6))

  const win = new BrowserWindow({
    width,
    height,
    minWidth: 1200,
    minHeight: 700,
    backgroundColor: '#0f172a',
    frame: useNativeTitlebar,
    icon: iconPath,
    webPreferences: {
      preload: join(__dirname, '../preload/index.js'),
      sandbox: false,
    },
  })
  mainWindow = win

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
  ipcMain.on('window-minimize-to-tray', () => win.hide())

  win.on('maximize',          () => win.webContents.send('window-maximized', true))
  win.on('unmaximize',        () => win.webContents.send('window-maximized', false))
  win.on('enter-full-screen', () => win.webContents.send('window-fullscreen-changed', true))
  win.on('leave-full-screen', () => win.webContents.send('window-fullscreen-changed', false))

  win.webContents.setWindowOpenHandler(({ url }) => {
    shell.openExternal(url)
    return { action: 'deny' }
  })

  win.webContents.on('render-process-gone', (_event, details) => {
    console.error('[main] render-process-gone:', details)
  })
  win.webContents.on('did-fail-load', (_event, errorCode, errorDescription, validatedURL) => {
    console.error('[main] did-fail-load:', errorCode, errorDescription, validatedURL)
  })
  win.on('unresponsive', () => {
    console.error('[main] window unresponsive')
  })
  win.on('closed', () => {
    console.log('[main] window closed')
  })

  setOnPlaybackState((state) => {
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

  if (process.env['ELECTRON_RENDERER_URL']) {
    win.webContents.on('before-input-event', (event, input) => {
      if (input.type === 'keyDown' && input.control && input.shift && input.key === 'I') {
        if (win.webContents.isDevToolsOpened()) {
          win.webContents.closeDevTools()
        } else {
          win.webContents.openDevTools()
        }
        event.preventDefault()
      }
    })
  }

  // In dev, electron-vite sets ELECTRON_RENDERER_URL
  if (process.env['ELECTRON_RENDERER_URL']) {
    console.log('[main] loadURL', process.env['ELECTRON_RENDERER_URL'])
    win.loadURL(process.env['ELECTRON_RENDERER_URL'])
  } else {
    win.loadFile(join(__dirname, '../renderer/index.html'))
  }
  console.log('[main] createWindow() end')
}

console.log('[main] awaiting app.whenReady()')
app.whenReady().then(() => {
  console.log('[main] app.whenReady() resolved')
  if (process.platform === 'win32') {
    app.setAppUserModelId(app.isPackaged ? 'com.tracknrace' : process.execPath)
  }

  Menu.setApplicationMenu(null)

  // Reclaim decompression temp files leaked by prior runs (or the native app) that
  // exited abnormally. Safe here: secondary instances already exited above, so we
  // hold the single-instance lock and no other session's temp is at risk.
  sweepTempFiles()

  console.log('[main] calling startBridge()')
  startBridge()   // starts the in-process libtnrp addon; owns UDP + recording
  console.log('[main] calling createWindow()')
  createWindow()

  // Tray icons are decoded by a different, more restrictive loader than
  // BrowserWindow's `icon` option (no .ico support on Linux), so use a PNG here.
  function trayPngForTheme(theme: 'light' | 'dark'): Electron.NativeImage {
    const path = theme === 'light' ? iconTransparentLightPng : iconTransparentPng
    return nativeImage.createFromPath(path).resize({ width: 32, height: 32 })
  }

  tray = new Tray(trayPngForTheme(getWindowsTaskbarThemeSync()))
  tray.setToolTip('Track N Race')
  tray.setContextMenu(Menu.buildFromTemplate([
    { label: 'Show', click: showWindow },
    { label: 'Quit', click: () => app.quit() }
  ]))
  tray.on('click', showWindow)

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
      tray?.setImage(trayPngForTheme(taskbarTheme))
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
    console.log('[main] will-quit')
    stopBridge()   // closes the player too (temp file cleanup happens in-engine)
    clearInterval(pollInterval)
  })


  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) createWindow()
  })
}).catch((err) => {
  console.error('[main] app.whenReady() rejected:', err)
})

app.on('window-all-closed', () => {
  console.log('[main] window-all-closed')
  if (process.platform !== 'darwin') {
    app.quit()
  }
})

app.on('quit', (_event, exitCode) => {
  console.log('[main] app quit, exitCode =', exitCode)
})

app.on('render-process-gone', (_event, _webContents, details) => {
  console.error('[main] app-level render-process-gone:', details)
})

app.on('child-process-gone', (_event, details) => {
  console.error('[main] child-process-gone:', details)
})
