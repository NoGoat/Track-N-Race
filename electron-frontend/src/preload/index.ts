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
  onBatch: (callback: (batch: string) => void): (() => void) => {
    const listener = (_event: Electron.IpcRendererEvent, batch: string) => callback(batch)
    ipcRenderer.on('telemetry-batch', listener)
    return () => ipcRenderer.removeListener('telemetry-batch', listener)
  },
  onBinary: (callback: (batch: Uint8Array) => void): (() => void) => {
    const listener = (_event: Electron.IpcRendererEvent, batch: Uint8Array) => callback(batch)
    ipcRenderer.on('telemetry-binary', listener)
    return () => ipcRenderer.removeListener('telemetry-binary', listener)
  },
  onResume: (callback: (payload: { binary: Uint8Array; coldJson: string }) => void): (() => void) => {
    const listener = (_event: Electron.IpcRendererEvent, payload: { binary: Uint8Array; coldJson: string }) => callback(payload)
    ipcRenderer.on('telemetry-resume', listener)
    return () => ipcRenderer.removeListener('telemetry-resume', listener)
  },
}

const windowControls = {
  minimize:   (): void => ipcRenderer.send('window-minimize'),
  maximize:   (): void => ipcRenderer.send('window-maximize'),
  close:      (): void => ipcRenderer.send('window-close'),
  fullscreen: (): void => ipcRenderer.send('window-fullscreen'),
  minimizeToTray: (): void => ipcRenderer.send('window-minimize-to-tray'),
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
  setOverride: (value: 'auto' | 'f1_24' | 'f1_25' | 'f1_26'): void =>
    ipcRenderer.send('protocol-set-override', value),
  requestStatus: (): void =>
    ipcRenderer.send('protocol-request-status'),
}

const fsBridge = {
  selectDirectory: (): Promise<string | null> =>
    ipcRenderer.invoke('dialog:showOpenDialog'),
  selectTNRDFile: (): Promise<string | null> =>
    ipcRenderer.invoke('dialog:showOpenDialogTNRD'),
}

const recordingBridge = {
  onError: (cb: (error: { operation: string; message: string; path: string }) => void): (() => void) => {
    const handler = (_event: Electron.IpcRendererEvent, error: { operation: string; message: string; path: string }) => cb(error)
    ipcRenderer.on('recording-error', handler)
    return () => ipcRenderer.removeListener('recording-error', handler)
  },
}


let playerAllLapsMode = false
let playerAllLapsRowMask = 0xFFFFFFFF
let playerWindowSeconds = 0
const seekStartListeners = new Set<(allHistory: boolean) => void>()
const playerBridge = {
  setPageVisible: (visible: boolean): void => ipcRenderer.send('page-visibility', visible),
  load: (filePath: string): Promise<{ ok: boolean; error?: string }> => ipcRenderer.invoke('player:load', filePath),
  play: () => ipcRenderer.send('player:play'),
  pause: () => ipcRenderer.send('player:pause'),
  seek: (pct: number) => {
    for (const listener of seekStartListeners) listener(playerAllLapsMode)
    ipcRenderer.send('player:seek', pct, playerAllLapsMode, playerAllLapsRowMask, playerWindowSeconds)
  },
  setAllLapsMode: (enabled: boolean, rowTypeMask = 0xFFFFFFFF, windowSeconds = 0) => {
    playerAllLapsMode = enabled
    playerAllLapsRowMask = rowTypeMask >>> 0
    playerWindowSeconds = Number.isFinite(windowSeconds) ? Math.max(0, windowSeconds) : 0
  },
  onSeekStart: (callback: (allHistory: boolean) => void) => {
    seekStartListeners.add(callback)
    return () => { seekStartListeners.delete(callback) }
  },
  setSpeed: (mult: number) => ipcRenderer.send('player:setSpeed', mult),
  getLapData: (lapNum: number) => ipcRenderer.send('player:getLapData', lapNum),
  getAllLapsData: (rowTypeMask?: number) => ipcRenderer.send('player:getAllLapsData', rowTypeMask),
  getWindowData: (windowSeconds: number, rowTypeMask?: number) => ipcRenderer.send('player:getWindowData', windowSeconds, rowTypeMask),
  close: () => ipcRenderer.send('player:close'),
  exportXlsx: (): Promise<{ ok: boolean; error?: string }> => ipcRenderer.invoke('player:export-xlsx'),
  onExportProgress: (cb: (pct: number, stage: string) => void) => {
    const handler = (_e: any, pct: number, stage: string) => cb(pct, stage)
    ipcRenderer.on('player:export-progress', handler)
    return () => { ipcRenderer.removeListener('player:export-progress', handler) }
  },
  onStateChange: (cb: (state: any) => void) => {
    const handler = (_e: any, state: any) => cb(state)
    ipcRenderer.on('playback_state', handler)
    return () => { ipcRenderer.removeListener('playback_state', handler) }
  },
  onRequestOpenConfirm: (cb: (filePath: string) => void) => {
    const handler = (_e: any, filePath: string) => cb(filePath)
    ipcRenderer.on('player:request-open-confirm', handler)
    return () => { ipcRenderer.removeListener('player:request-open-confirm', handler) }
  },
  onLoadFailed: (cb: (reason: string) => void) => {
    const handler = (_e: any, reason: string) => cb(reason)
    ipcRenderer.on('player:load-failed', handler)
    return () => { ipcRenderer.removeListener('player:load-failed', handler) }
  }
}

const analysisBridge = {
  loadFile: (filePath: string): Promise<{ ok: boolean; error?: string; data?: unknown; trackId?: number; trackName?: string }> =>
    ipcRenderer.invoke('analysis:load-file', filePath),
  getLapData: (lapNum: number): Promise<unknown | null> =>
    ipcRenderer.invoke('analysis:get-lap-data', lapNum),
  closeFile: (): void => ipcRenderer.send('analysis:close-file'),
}

contextBridge.exposeInMainWorld('electronStore', storeAPI)
contextBridge.exposeInMainWorld('platform', process.platform)
contextBridge.exposeInMainWorld('telemetryBridge', telemetryBridge)
contextBridge.exposeInMainWorld('windowControls', windowControls)
contextBridge.exposeInMainWorld('udpBridge', udpBridge)
contextBridge.exposeInMainWorld('protocolBridge', protocolBridge)
contextBridge.exposeInMainWorld('fsBridge', fsBridge)
contextBridge.exposeInMainWorld('recordingBridge', recordingBridge)
contextBridge.exposeInMainWorld('playerBridge', playerBridge)
contextBridge.exposeInMainWorld('analysisBridge', analysisBridge)
