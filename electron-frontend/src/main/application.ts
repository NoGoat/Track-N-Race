import { app, BrowserWindow, shell, ipcMain, Menu, Tray, nativeTheme, nativeImage, dialog, screen, clipboard } from 'electron'
import * as path from 'path'
import * as fs from 'fs'
import { join } from 'path'
import { execFile, spawn } from 'child_process'
import { configStore as store } from './configStore'
import { setFatalFlushHandler } from './diagnostics'
import { checkForUpdateOnStartup, RELEASE_PAGE_URL, skipUpdateVersion, type AvailableUpdate } from './updateChecker'
import {
  setOverride,
  restartUdp,
  startBridge,
  stopBridge,
  flushRecording,
  getProtocolConfig,
  requestStatus,
  setRendererVisible,
  exportSessionXlsx,
  playerLoad,
  playerPlay,
  playerPause,
  playerSeek,
  playerSeekInstalled,
  playerSetSpeed,
  playerGetLapData,
  playerGetAllLapsData,
  playerGetWindowData,
  playerSetDataRequirements,
  playerClose,
  analysisLoadFile,
  analysisGetLapData,
  analysisCloseFile,
  setOnPlaybackState,
  getActiveFilePath,
  sweepTempFiles,
  type PlayerLoadResult
} from './bridgeManager'

type ProtocolOverride = 'auto' | 'f1_24' | 'f1_25' | 'f1_26'

import iconTransparent from '../../build/icon_transparent.ico?asset'
import iconTransparentLight from '../../build/icon_transparent_light.ico?asset'
import iconTransparentPng from '../../build/icon_transparent.png?asset'
import iconTransparentLightPng from '../../build/icon_transparent_light.png?asset'

declare const __APP_VERSION__: string

console.log('[main] application module loading, pid=', process.pid)
setFatalFlushHandler(flushRecording)

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
const LAST_DIALOG_DIRECTORY_KEY = 'dialogs.lastDirectory'
let startupUpdateCheck: Promise<AvailableUpdate | null> | null = null

function lastDialogDirectory(): string | undefined {
  const value = store.get(LAST_DIALOG_DIRECTORY_KEY)
  return typeof value === 'string' && value.length > 0 && fs.existsSync(value)
    ? value
    : undefined
}

function rememberDialogDirectory(selectedPath: string, selectedDirectory = false): void {
  store.set(LAST_DIALOG_DIRECTORY_KEY, selectedDirectory ? selectedPath : path.dirname(selectedPath))
}

function showWindow(): void {
  if (!mainWindow) return
  mainWindow.show()
  mainWindow.focus()
}

function expandWindowsShortPath(filePath: string): string {
  if (process.platform !== 'win32') return filePath
  try {
    return fs.realpathSync.native(filePath)
  } catch {
    return filePath
  }
}

async function openTelemetryFile(filePath: string): Promise<PlayerLoadResult> {
  // The bridge switches to playback mode itself (it ignores live UDP while a clip
  // is loaded), so there's no separate live-suspend step here.
  const result = await playerLoad(expandWindowsShortPath(filePath))
  if (!result.ok) {
    for (const win of BrowserWindow.getAllWindows()) {
      if (!win.isDestroyed()) {
        win.webContents.send(
          'player:load-failed',
          result.error || 'The file could not be read.'
        )
      }
    }
  }
  return result
}

// The bootstrap acquired the single-instance lock before loading this module,
// which lets diagnostics start before any native/application imports run.
app.on('second-instance', (_event, commandLine) => {
  console.log('[main] second-instance request:', commandLine)
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
  if (key === 'theme') updateWindowsTitleBarSymbolColor(value)
})

ipcMain.handle('updates:check-on-startup', () => {
  startupUpdateCheck ??= checkForUpdateOnStartup(__APP_VERSION__)
  return startupUpdateCheck
})

ipcMain.on('updates:skip-version', (_event, version: string) => {
  skipUpdateVersion(version)
})

ipcMain.handle('updates:open-download-page', () => shell.openExternal(RELEASE_PAGE_URL))

ipcMain.handle('dialog:showOpenDialog', async () => {
  const { canceled, filePaths } = await dialog.showOpenDialog({
    defaultPath: lastDialogDirectory(),
    properties: ['openDirectory']
  })
  if (canceled) {
    return null
  } else {
    rememberDialogDirectory(filePaths[0], true)
    return filePaths[0]
  }
})

ipcMain.handle('dialog:showOpenDialogTNRD', async () => {
  const { canceled, filePaths } = await dialog.showOpenDialog({
    defaultPath: lastDialogDirectory(),
    filters: [{ name: 'Track N Race Data', extensions: ['tnrd', 'trnd'] }],
    properties: ['openFile']
  })
  if (canceled) return null
  rememberDialogDirectory(filePaths[0])
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
ipcMain.on('player:seek', (_event, pct: number, allHistory: boolean, rowTypeMask?: number, windowSeconds?: number) =>
  playerSeek(pct, allHistory === true, rowTypeMask, windowSeconds))
ipcMain.on('player:seek-installed', (_event, requestId: number) =>
  playerSeekInstalled(requestId))
ipcMain.on('player:setSpeed', (_event, mult: number) => playerSetSpeed(mult))
ipcMain.on('player:getLapData', (_event, lapNum: number, rowTypeMask?: number) => playerGetLapData(lapNum, rowTypeMask))
ipcMain.on('player:getAllLapsData', (_event, rowTypeMask?: number) => playerGetAllLapsData(rowTypeMask))
ipcMain.on('player:getWindowData', (_event, windowSeconds: number, rowTypeMask?: number) => playerGetWindowData(windowSeconds, rowTypeMask))
ipcMain.on('player:setDataRequirements', (_event, streamMask: number, historyMask: number, windowSeconds: number) =>
  playerSetDataRequirements(streamMask, historyMask, windowSeconds))
ipcMain.on('player:close', () => playerClose())
ipcMain.handle('analysis:load-file', (_event, filePath: string) => analysisLoadFile(filePath))
ipcMain.handle('analysis:get-lap-data', (_event, lapNum: number, rowTypeMask?: number) => analysisGetLapData(lapNum, rowTypeMask))
ipcMain.on('analysis:close-file', () => analysisCloseFile())

ipcMain.handle('player:export-xlsx', async (event) => {
  const srcPath = getActiveFilePath()
  if (!srcPath) return { ok: false, error: 'No session loaded' }

  const base = path.basename(srcPath).replace(/\.(tnrd|trnd)$/i, '')
  const exportDirectory = lastDialogDirectory() ?? path.dirname(srcPath)
  const { canceled, filePath } = await dialog.showSaveDialog({
    title: 'Export Session to Excel',
    defaultPath: path.join(exportDirectory, `${base}.xlsx`),
    filters: [{ name: 'Excel Workbook', extensions: ['xlsx'] }]
  })
  if (canceled || !filePath) return { ok: false, error: 'cancelled' }
  rememberDialogDirectory(filePath)

  const sender = event.sender
  return exportSessionXlsx(srcPath, filePath, (pct, stage) => {
    if (!sender.isDestroyed()) sender.send('player:export-progress', pct, stage)
  })
})

ipcMain.on('udp-restart', (event) => {
  try {
    const error = restartUdp()
    event.reply('udp-restart-result', error ? { ok: false, error } : { ok: true })
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



type TaskbarTheme = 'light' | 'dark'

function nativeTaskbarTheme(): TaskbarTheme {
  return nativeTheme.shouldUseDarkColors ? 'dark' : 'light'
}

function getWindowsTaskbarTheme(): Promise<TaskbarTheme> {
  if (process.platform !== 'win32') return Promise.resolve(nativeTaskbarTheme())

  return new Promise((resolve) => {
    execFile(
      'reg.exe',
      [
        'query',
        'HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize',
        '/v',
        'SystemUsesLightTheme'
      ],
      { encoding: 'utf8', windowsHide: true, timeout: 1000 },
      (error, stdout) => {
        if (!error) {
          const match = /SystemUsesLightTheme\s+REG_DWORD\s+(0x\d+)/.exec(stdout)
          if (match) {
            resolve(parseInt(match[1], 16) === 1 ? 'light' : 'dark')
            return
          }
        }

        resolve(nativeTaskbarTheme())
      }
    )
  })
}

function windowsTitleBarSymbolColor(theme: unknown): string {
  return theme === 'light' ? '#000000' : '#ffffff'
}

function updateWindowsTitleBarSymbolColor(theme: unknown): void {
  if (process.platform !== 'win32' || !mainWindow || mainWindow.isDestroyed()) return
  if (store.get('nativeTitlebar', false) as boolean) return

  mainWindow.setTitleBarOverlay({
    color: '#00000000',
    symbolColor: windowsTitleBarSymbolColor(theme),
    height: 40,
  })
}

function createWindow(): void {
  console.log('[main] createWindow() start')
  const taskbarTheme = nativeTaskbarTheme()
  const iconPath = taskbarTheme === 'light' ? iconTransparentLight : iconTransparent
  const useNativeTitlebar = store.get('nativeTitlebar', false) as boolean
  const isMacOS = process.platform === 'darwin'
  const useTitleBarOverlay = (process.platform === 'win32' || process.platform === 'linux') && !useNativeTitlebar

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
    frame: useNativeTitlebar || isMacOS || useTitleBarOverlay,
    ...(isMacOS && !useNativeTitlebar ? {
      titleBarStyle: 'hidden' as const,
      trafficLightPosition: { x: 14, y: 14 },
    } : {}),
    ...(useTitleBarOverlay ? {
      titleBarStyle: 'hidden' as const,
      titleBarOverlay: {
        color: '#00000000',
        ...(process.platform === 'win32' ? {
          symbolColor: windowsTitleBarSymbolColor(store.get('theme', 'dark')),
        } : {}),
        height: 40,
      },
    } : {}),
    icon: iconPath,
    webPreferences: {
      preload: join(__dirname, '../preload/index.js'),
      sandbox: false,
    },
  })
  mainWindow = win

  // Electron disables Chromium's visual pinch zoom by default. Enable the
  // non-layout, trackpad-driven magnification in development for close UI
  // inspection, while leaving packaged application behavior unchanged.
  if (process.env['ELECTRON_RENDERER_URL']) {
    void win.webContents.setVisualZoomLevelLimits(1, 5).catch(error => {
      console.error('[main] failed to enable development pinch zoom:', error)
    })
  }

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
  win.webContents.on('console-message', (details) => {
    const location = details.sourceId ? ` (${details.sourceId}:${details.lineNumber})` : ''
    const text = `[renderer] ${details.message}${location}`
    if (details.level === 'error') console.error(text)
    else if (details.level === 'warning') console.warn(text)
    else if (details.level === 'debug') console.debug(text)
    else console.info(text)
  })
  win.webContents.on('preload-error', (_event, preloadPath, error) => {
    console.error('[main] preload-error:', preloadPath, error)
  })
  win.webContents.on('did-start-loading', () => {
    console.log('[main] renderer did-start-loading')
  })
  win.webContents.on('did-finish-load', () => {
    console.log('[main] renderer did-finish-load')
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

  // if (process.env['ELECTRON_RENDERER_URL']) {
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
  // }

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
  const bridgeStartupError = startBridge()
  console.log('[main] calling createWindow()')
  createWindow()
  if (bridgeStartupError) {
    const bridgeErrorReport = [
      'Track N Race native telemetry bridge startup failure',
      `App version: ${app.isPackaged ? __APP_VERSION__ : 'Next'}`,
      `Electron: ${process.versions.electron}`,
      `Chrome: ${process.versions.chrome}`,
      `Node: ${process.versions.node}`,
      `Platform: ${process.platform} ${process.arch}`,
      `OS version: ${process.getSystemVersion()}`,
      '',
      bridgeStartupError
    ].join('\n')

    setImmediate(() => {
      clipboard.writeText(bridgeErrorReport)
      dialog.showErrorBox(
        'Telemetry Bridge Failed to Load',
        'Track N Race will continue without the native telemetry bridge. However, pretty much nothing will work.\n\n' +
        'The full diagnostic report and stack trace have been copied to your clipboard. ' +
        'Paste them into a message to the developer.\n\n' +
        bridgeErrorReport
      )
    })
  }

  // Tray icons are decoded by a different, more restrictive loader than
  // BrowserWindow's `icon` option (no .ico support on Linux), so use a PNG here.
  function trayPngForTheme(theme: TaskbarTheme): Electron.NativeImage {
    const path = theme === 'light' ? iconTransparentLightPng : iconTransparentPng
    const size = process.platform === 'darwin' ? 20 : 32
    return nativeImage.createFromPath(path).resize({ width: size, height: size })
  }

  const initialTheme = nativeTaskbarTheme()
  tray = new Tray(trayPngForTheme(initialTheme))
  tray.setToolTip('Track N Race')
  tray.setContextMenu(Menu.buildFromTemplate([
    { label: 'Show', click: showWindow },
    { label: 'Quit', click: () => app.quit() }
  ]))
  tray.on('click', showWindow)

  // Track the current icon path to prevent redundant win.setIcon calls
  let lastIconPath = initialTheme === 'light' ? iconTransparentLight : iconTransparent
  let taskbarThemeQueryInFlight = false

  async function updateTaskbarIconIfNeeded(): Promise<void> {
    if (taskbarThemeQueryInFlight) return
    taskbarThemeQueryInFlight = true

    try {
      const taskbarTheme = await getWindowsTaskbarTheme()
      const currentIconPath = taskbarTheme === 'light' ? iconTransparentLight : iconTransparent

      if (currentIconPath !== lastIconPath) {
        lastIconPath = currentIconPath
        for (const win of BrowserWindow.getAllWindows()) {
          if (!win.isDestroyed()) {
            win.setIcon(currentIconPath)
          }
        }
        if (tray && !tray.isDestroyed()) {
          tray.setImage(trayPngForTheme(taskbarTheme))
        }
      }
    } finally {
      taskbarThemeQueryInFlight = false
    }
  }

  // Update on nativeTheme change
  nativeTheme.on('updated', () => {
    void updateTaskbarIconIfNeeded()
  })

  // Reconcile the immediate nativeTheme fallback with the Windows taskbar setting.
  void updateTaskbarIconIfNeeded()

  // Poll fallback (1.5s interval) to guarantee detection of custom taskbar theme changes
  const pollInterval = setInterval(() => {
    void updateTaskbarIconIfNeeded()
  }, 1500)

  app.on('will-quit', () => {
    console.log('[main] will-quit')
    stopBridge()   // closes the player too (temp file cleanup happens in-engine)
    clearInterval(pollInterval)
  })


  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) {
      createWindow()
    } else {
      showWindow()
    }
  })
}).catch((err) => {
  console.error('[main] app.whenReady() rejected:', err)
})

app.on('window-all-closed', () => {
  console.log('[main] window-all-closed')
  app.quit()
})

app.on('quit', (_event, exitCode) => {
  console.log('[main] app quit, exitCode =', exitCode)
})

app.on('browser-window-created', (_event, window) => {
  console.log('[main] browser-window-created, id=', window.id)
})

app.on('web-contents-created', (_event, contents) => {
  console.log('[main] web-contents-created, id=', contents.id, 'type=', contents.getType())
})

app.on('render-process-gone', (_event, _webContents, details) => {
  console.error('[main] app-level render-process-gone:', details)
  flushRecording()
})

app.on('child-process-gone', (_event, details) => {
  console.error('[main] child-process-gone:', details)
  flushRecording()
})
