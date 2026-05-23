import { app, BrowserWindow, shell, ipcMain, Menu, globalShortcut, nativeTheme } from 'electron'
import { join } from 'path'
import { execSync } from 'child_process'
import Store from 'electron-store'
import { startUdpReceiver, stopUdpReceiver } from './udpReceiver'
import { setOverride, getProtocolConfig } from './protocolDispatcher'
import type { ProtocolOverride } from './protocolDispatcher'

const store = new Store()

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
  const iconName = taskbarTheme === 'light' ? 'icon_transparent_light.ico' : 'icon_transparent.ico'

  const win = new BrowserWindow({
    width: 1600,
    height: 900,
    minWidth: 1200,
    minHeight: 700,
    backgroundColor: '#0f172a',
    frame: false,
    icon: join(__dirname, `../../build/${iconName}`),
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

  Menu.setApplicationMenu(null)
  startUdpReceiver({
    port:        store.get('udp.port', 20777) as number,
    bindAddress: store.get('udp.bindAddress', '0.0.0.0') as string,
  })
  createWindow()

  // Track the current icon name to prevent redundant win.setIcon calls
  let lastIconName = ''

  function updateTaskbarIconIfNeeded(): void {
    const taskbarTheme = getWindowsTaskbarThemeSync()
    const iconName = taskbarTheme === 'light' ? 'icon_transparent_light.ico' : 'icon_transparent.ico'
    
    if (iconName !== lastIconName) {
      lastIconName = iconName
      const iconPath = join(__dirname, `../../build/${iconName}`)
      for (const win of BrowserWindow.getAllWindows()) {
        if (!win.isDestroyed()) {
          win.setIcon(iconPath)
        }
      }
    }
  }

  // Initialize with startup icon theme
  const initialTheme = getWindowsTaskbarThemeSync()
  lastIconName = initialTheme === 'light' ? 'icon_transparent_light.ico' : 'icon_transparent.ico'

  // Update on nativeTheme change
  nativeTheme.on('updated', () => {
    updateTaskbarIconIfNeeded()
  })

  // Poll fallback (1.5s interval) to guarantee detection of custom taskbar theme changes
  const pollInterval = setInterval(() => {
    updateTaskbarIconIfNeeded()
  }, 1500)

  app.on('will-quit', () => {
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
