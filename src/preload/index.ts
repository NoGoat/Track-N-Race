import { contextBridge, ipcRenderer } from 'electron'

const storeAPI = {
  get: (key: string, defaultValue: unknown): unknown =>
    ipcRenderer.sendSync('store-get', key, defaultValue),
  set: (key: string, value: unknown): void =>
    ipcRenderer.send('store-set', key, value),
}

const telemetryBridge = {
  on: (callback: (row: unknown) => void): (() => void) => {
    const listener = (_event: Electron.IpcRendererEvent, row: unknown) => callback(row)
    ipcRenderer.on('telemetry', listener)
    return () => ipcRenderer.removeListener('telemetry', listener)
  },
}

const windowControls = {
  minimize:   (): void => ipcRenderer.send('window-minimize'),
  maximize:   (): void => ipcRenderer.send('window-maximize'),
  close:      (): void => ipcRenderer.send('window-close'),
  fullscreen: (): void => ipcRenderer.send('window-fullscreen'),
  onMaximizeChange: (callback: (isMaximized: boolean) => void): (() => void) => {
    const listener = (_event: Electron.IpcRendererEvent, value: boolean) => callback(value)
    ipcRenderer.on('window-maximized', listener)
    return () => ipcRenderer.removeListener('window-maximized', listener)
  },
  onFullscreenChange: (callback: (isFullscreen: boolean) => void): (() => void) => {
    const listener = (_event: Electron.IpcRendererEvent, value: boolean) => callback(value)
    ipcRenderer.on('window-fullscreen-changed', listener)
    return () => ipcRenderer.removeListener('window-fullscreen-changed', listener)
  },
}

const udpBridge = {
  restart: (): Promise<{ ok: boolean; error?: string }> =>
    new Promise((resolve) => {
      ipcRenderer.once('udp-restart-result', (_event, result) => resolve(result))
      ipcRenderer.send('udp-restart')
    }),
}

const protocolBridge = {
  getConfig: (): Promise<{ override: string; detected: number | null; lastDetected: number | null; active: number | null }> =>
    ipcRenderer.invoke('protocol-get-config'),
  setOverride: (value: 'auto' | 'f1_24' | 'f1_25'): void =>
    ipcRenderer.send('protocol-set-override', value),
}

contextBridge.exposeInMainWorld('electronStore', storeAPI)
contextBridge.exposeInMainWorld('telemetryBridge', telemetryBridge)
contextBridge.exposeInMainWorld('windowControls', windowControls)
contextBridge.exposeInMainWorld('udpBridge', udpBridge)
contextBridge.exposeInMainWorld('protocolBridge', protocolBridge)

