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
  switchToRecorder: (): void => ipcRenderer.send('switch-to-recorder'),
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

const fsBridge = {
  selectDirectory: (): Promise<string | null> =>
    ipcRenderer.invoke('dialog:showOpenDialog'),
  selectTNRDFile: (): Promise<string | null> =>
    ipcRenderer.invoke('dialog:showOpenDialogTNRD'),
}

const debugBridge = {
  write: (msg: string): void => ipcRenderer.send('debug:write', msg)
}

const playerBridge = {
  load: (filePath: string): Promise<boolean> => ipcRenderer.invoke('player:load', filePath),
  play: () => ipcRenderer.send('player:play'),
  pause: () => ipcRenderer.send('player:pause'),
  seek: (pct: number) => ipcRenderer.send('player:seek', pct),
  setSpeed: (mult: number) => ipcRenderer.send('player:setSpeed', mult),
  close: () => ipcRenderer.send('player:close'),
  onStateChange: (cb: (state: any) => void) => {
    const handler = (_e: any, state: any) => cb(state)
    ipcRenderer.on('playback_state', handler)
    return () => { ipcRenderer.removeListener('playback_state', handler) }
  },
  onRequestOpenConfirm: (cb: (filePath: string) => void) => {
    const handler = (_e: any, filePath: string) => cb(filePath)
    ipcRenderer.on('player:request-open-confirm', handler)
    return () => { ipcRenderer.removeListener('player:request-open-confirm', handler) }
  }
}

contextBridge.exposeInMainWorld('electronStore', storeAPI)
contextBridge.exposeInMainWorld('telemetryBridge', telemetryBridge)
contextBridge.exposeInMainWorld('windowControls', windowControls)
contextBridge.exposeInMainWorld('udpBridge', udpBridge)
contextBridge.exposeInMainWorld('protocolBridge', protocolBridge)
contextBridge.exposeInMainWorld('fsBridge', fsBridge)
contextBridge.exposeInMainWorld('playerBridge', playerBridge)
contextBridge.exposeInMainWorld('debugBridge', debugBridge)

