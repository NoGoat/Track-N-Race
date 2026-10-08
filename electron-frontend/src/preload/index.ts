import { contextBridge, ipcRenderer } from 'electron'
import type { PlaybackState } from '../main/bridgeManager'

const storeAPI = {
  get: (key: string, defaultValue: unknown): unknown =>
    ipcRenderer.sendSync('store-get', key, defaultValue),
  set: (key: string, value: unknown): void =>
    ipcRenderer.send('store-set', key, value),
}

interface DebugSettings {
  additionalLogging: boolean
  reactScan: boolean
  webglMonitor: boolean
  memoryLog: boolean
  nodeApiExceptions: boolean
}

let additionalLoggingEnabled = storeAPI.get('debug.additionalLogging', false) === true
ipcRenderer.on('debug-settings-changed', (_event, settings: DebugSettings) => {
  additionalLoggingEnabled = settings.additionalLogging
})

function payloadBytes(value: unknown): number | null {
  if (typeof value === 'string') return new TextEncoder().encode(value).byteLength
  if (value instanceof Uint8Array) return value.byteLength
  if (value instanceof ArrayBuffer) return value.byteLength
  return null
}

function logSubscription(channel: string): void {
  if (!additionalLoggingEnabled) return
  console.info(`[telemetry-diagnostics][preload] subscribed to ${channel}`)
}

function logFirstDelivery(channel: string, payload: unknown): void {
  if (!additionalLoggingEnabled) return
  console.info(`[telemetry-diagnostics][preload] first ${channel} delivery`, {
    bytes: payloadBytes(payload),
    valueType: Object.prototype.toString.call(payload),
  })
}

const telemetryBridge = {
  on: (callback: (row: unknown) => void): (() => void) => {
    logSubscription('telemetry')
    let first = true
    const listener = (_event: Electron.IpcRendererEvent, row: unknown) => {
      if (first && additionalLoggingEnabled) { first = false; logFirstDelivery('telemetry', row) }
      callback(row)
    }
    ipcRenderer.on('telemetry', listener)
    return () => ipcRenderer.removeListener('telemetry', listener)
  },
  onBatch: (callback: (batch: string) => void): (() => void) => {
    logSubscription('telemetry-batch')
    let first = true
    const listener = (_event: Electron.IpcRendererEvent, batch: string) => {
      if (first && additionalLoggingEnabled) { first = false; logFirstDelivery('telemetry-batch', batch) }
      callback(batch)
    }
    ipcRenderer.on('telemetry-batch', listener)
    return () => ipcRenderer.removeListener('telemetry-batch', listener)
  },
  onBinary: (callback: (batch: Uint8Array) => void): (() => void) => {
    logSubscription('telemetry-binary')
    let first = true
    const listener = (_event: Electron.IpcRendererEvent, batch: Uint8Array) => {
      if (first && additionalLoggingEnabled) { first = false; logFirstDelivery('telemetry-binary', batch) }
      callback(batch)
    }
    ipcRenderer.on('telemetry-binary', listener)
    return () => ipcRenderer.removeListener('telemetry-binary', listener)
  },
  // A live lap answer: its JSON header and the lap's V6H1 payload.
  onLiveLapData: (callback: (header: string, history: Uint8Array) => void): (() => void) => {
    const listener = (_event: Electron.IpcRendererEvent, header: string, history: Uint8Array) => callback(header, history)
    ipcRenderer.on('live-lap-data', listener)
    return () => ipcRenderer.removeListener('live-lap-data', listener)
  },
  reportRetention: (snapshot: unknown): void => {
    const runtime = sampleRendererRuntimeMemory()
    ipcRenderer.send('diagnostics:telemetry-retention',
      snapshot && typeof snapshot === 'object' ? { ...snapshot, runtime_memory: runtime } : snapshot)
  },
}

// Renderer V8/Blink figures for the RAM log, attached to each retention report
// (once a second while the memory log is on). V8 exposes no allocation
// counter, so the heap is polled between reports: growth between polls is a
// lower bound on what was allocated (collections inside one poll interval hide
// some), and drops are what collections reclaimed. The poll stops by itself
// once reports stop.
const HEAP_POLL_MS = 100
const HEAP_POLL_IDLE_MS = 5000
let heapPollTimer: ReturnType<typeof setInterval> | null = null
let heapPollLastUsed = 0
let heapPollWindowStart = 0
let heapGrownBytes = 0
let heapReclaimedBytes = 0
let heapDrops = 0
let lastRuntimeReport = 0

function pollRendererHeap(): void {
  if (Date.now() - lastRuntimeReport > HEAP_POLL_IDLE_MS) {
    if (heapPollTimer !== null) clearInterval(heapPollTimer)
    heapPollTimer = null
    return
  }
  const used = process.getHeapStatistics().usedHeapSize * 1024
  const delta = used - heapPollLastUsed
  if (delta >= 0) heapGrownBytes += delta
  else { heapReclaimedBytes -= delta; heapDrops++ }
  heapPollLastUsed = used
}

function sampleRendererRuntimeMemory(): Record<string, unknown> | null {
  try {
    const now = Date.now()
    lastRuntimeReport = now
    const heap = process.getHeapStatistics()
    const blink = process.getBlinkMemoryInfo()
    const windowMs = heapPollTimer === null ? 0 : now - heapPollWindowStart
    const churn = {
      window_ms: windowMs,
      heap_grown_bytes: heapGrownBytes,
      heap_reclaimed_bytes: heapReclaimedBytes,
      heap_drops: heapDrops,
      allocated_lower_bound_bytes_per_s: windowMs > 0 ? Math.round(heapGrownBytes * 1000 / windowMs) : null,
      reclaimed_bytes_per_s: windowMs > 0 ? Math.round(heapReclaimedBytes * 1000 / windowMs) : null,
    }
    heapGrownBytes = heapReclaimedBytes = heapDrops = 0
    heapPollWindowStart = now
    heapPollLastUsed = heap.usedHeapSize * 1024
    if (heapPollTimer === null) heapPollTimer = setInterval(pollRendererHeap, HEAP_POLL_MS)
    return {
      pid: process.pid,
      // Electron reports these in kilobytes.
      v8: {
        total_heap_size_bytes: heap.totalHeapSize * 1024,
        total_heap_size_executable_bytes: heap.totalHeapSizeExecutable * 1024,
        total_physical_size_bytes: heap.totalPhysicalSize * 1024,
        total_available_size_bytes: heap.totalAvailableSize * 1024,
        used_heap_size_bytes: heap.usedHeapSize * 1024,
        heap_size_limit_bytes: heap.heapSizeLimit * 1024,
        malloced_memory_bytes: heap.mallocedMemory * 1024,
        peak_malloced_memory_bytes: heap.peakMallocedMemory * 1024,
      },
      blink: {
        allocated_bytes: blink.allocated * 1024,
        total_bytes: blink.total * 1024,
      },
      churn,
    }
  } catch {
    // Diagnostics must never affect the renderer.
    return null
  }
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
  getStatus: (): Promise<{ ok: boolean; error?: string }> =>
    ipcRenderer.invoke('udp-get-status'),
  restart: (): Promise<{ ok: boolean; error?: string }> =>
    new Promise((resolve) => {
      ipcRenderer.once('udp-restart-result', (_event, result) => resolve(result))
      ipcRenderer.send('udp-restart')
    }),
  onStatusChange: (callback: (status: { ok: boolean; error?: string }) => void): (() => void) => {
    const listener = (_event: Electron.IpcRendererEvent, status: { ok: boolean; error?: string }) => callback(status)
    ipcRenderer.on('udp-status', listener)
    return () => ipcRenderer.removeListener('udp-status', listener)
  },
}

const debugBridge = {
  get: (): Promise<DebugSettings> => ipcRenderer.invoke('debug-settings-get'),
  setAdditionalLogging: (enabled: boolean): void =>
    ipcRenderer.send('debug-settings-set', 'additionalLogging', enabled),
  setReactScan: (enabled: boolean): void =>
    ipcRenderer.send('debug-settings-set', 'reactScan', enabled),
  setWebglMonitor: (enabled: boolean): void =>
    ipcRenderer.send('debug-settings-set', 'webglMonitor', enabled),
  setMemoryLog: (enabled: boolean): void =>
    ipcRenderer.send('debug-settings-set', 'memoryLog', enabled),
  setNodeApiExceptions: (enabled: boolean): void =>
    ipcRenderer.send('debug-settings-set', 'nodeApiExceptions', enabled),
  onChange: (callback: (settings: DebugSettings) => void): (() => void) => {
    const listener = (_event: Electron.IpcRendererEvent, settings: DebugSettings) => callback(settings)
    ipcRenderer.on('debug-settings-changed', listener)
    return () => ipcRenderer.removeListener('debug-settings-changed', listener)
  },
}

const protocolBridge = {
  getConfig: (): Promise<{ override: string; detected: number | null; lastDetected: number | null; active: number | null }> =>
    ipcRenderer.invoke('protocol-get-config'),
  setOverride: (value: 'auto' | 'f1_24' | 'f1_25' | 'f1_26'): void =>
    ipcRenderer.send('protocol-set-override', value),
  getTeamColors: (): Promise<{
    catalog: Record<string, Array<{ id: number; name: string; color: string; group: string }>>
    overrides: Record<string, Record<string, string>>
  }> => ipcRenderer.invoke('protocol-get-team-colors'),
  setTeamColors: (value: Record<string, Record<string, string>>): void =>
    ipcRenderer.send('protocol-set-team-colors', value),
  requestStatus: (): void =>
    ipcRenderer.send('protocol-request-status'),
}

const strategyBridge = {
  setMinimumStops: (value: number): void =>
    ipcRenderer.send('strategy-set-minimum-stops', value),
}

const fsBridge = {
  openLaunchDiagnostics: (): Promise<void> =>
    ipcRenderer.invoke('diagnostics:open-folder'),
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

const updateBridge = {
  checkOnStartup: (): Promise<unknown> => ipcRenderer.invoke('updates:check-on-startup'),
  skipVersion: (version: string): void => ipcRenderer.send('updates:skip-version', version),
  openDownloadPage: (): Promise<void> => ipcRenderer.invoke('updates:open-download-page'),
}

const pairingBridge = {
  getState: (): Promise<unknown> => ipcRenderer.invoke('pairing:get-state'),
  setEnabled: (enabled: boolean): Promise<unknown> => ipcRenderer.invoke('pairing:set-enabled', enabled),
  openWindow: (): Promise<unknown> => ipcRenderer.invoke('pairing:open-window'),
  closeWindow: (): Promise<unknown> => ipcRenderer.invoke('pairing:close-window'),
  removeDevice: (id: string): Promise<unknown> => ipcRenderer.invoke('pairing:remove-device', id),
  onState: (callback: (state: unknown) => void): (() => void) => {
    const listener = (_event: Electron.IpcRendererEvent, state: unknown) => callback(state)
    ipcRenderer.on('pairing:state', listener)
    return () => ipcRenderer.removeListener('pairing:state', listener)
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
  setDataRequirements: (streamMask: number, historyMask: number, windowSeconds: number,
                        v6Types: number[] = [], v6HistoryTypes: number[] = []) => {
    ipcRenderer.send('player:setDataRequirements', streamMask >>> 0, historyMask >>> 0,
      Number.isFinite(windowSeconds) ? Math.max(-1, windowSeconds) : 0, v6Types, v6HistoryTypes)
  },
  onSeekStart: (callback: (allHistory: boolean) => void) => {
    seekStartListeners.add(callback)
    return () => { seekStartListeners.delete(callback) }
  },
  seekInstalled: (requestId: number) => ipcRenderer.send('player:seek-installed', requestId),
  setSpeed: (mult: number) => ipcRenderer.send('player:setSpeed', mult),
  setDriver: (driverIndex: number, useRecordedRows = false) =>
    ipcRenderer.send('player:setDriver', driverIndex, useRecordedRows),
  setFocusDriver: (driverIndex: number) => ipcRenderer.send('player:setFocusDriver', driverIndex),
  getLiveFastestLap: (requestId: number) => ipcRenderer.send('live:getFastestLap', requestId),
  getLiveLap: (requestId: number, lapNum: number) => ipcRenderer.send('live:getLap', requestId, lapNum),
  setLapHistoryCar: (carIdx: number) => ipcRenderer.send('engine:lap-history-car', carIdx),
  getLapData: (lapNum: number, rowTypeMask = 0xFFFFFFFF) =>
    ipcRenderer.send('player:getLapData', lapNum, rowTypeMask >>> 0),
  getAllLapsData: (rowTypeMask?: number) => ipcRenderer.send('player:getAllLapsData', rowTypeMask),
  getWindowData: (windowSeconds: number, rowTypeMask?: number) => ipcRenderer.send('player:getWindowData', windowSeconds, rowTypeMask),
  close: () => ipcRenderer.send('player:close'),
  exportXlsx: (): Promise<{ ok: boolean; error?: string }> => ipcRenderer.invoke('player:export-xlsx'),
  onExportProgress: (cb: (pct: number, stage: string) => void) => {
    const handler = (_e: Electron.IpcRendererEvent, pct: number, stage: string) => cb(pct, stage)
    ipcRenderer.on('player:export-progress', handler)
    return () => { ipcRenderer.removeListener('player:export-progress', handler) }
  },
  onStateChange: (cb: (state: PlaybackState) => void) => {
    const handler = (_e: Electron.IpcRendererEvent, state: PlaybackState) => cb(state)
    ipcRenderer.on('playback_state', handler)
    return () => { ipcRenderer.removeListener('playback_state', handler) }
  },
  onRequestOpenConfirm: (cb: (filePath: string) => void) => {
    const handler = (_e: Electron.IpcRendererEvent, filePath: string) => cb(filePath)
    ipcRenderer.on('player:request-open-confirm', handler)
    return () => { ipcRenderer.removeListener('player:request-open-confirm', handler) }
  },
  onLoadFailed: (cb: (reason: string) => void) => {
    const handler = (_e: Electron.IpcRendererEvent, reason: string) => cb(reason)
    ipcRenderer.on('player:load-failed', handler)
    return () => { ipcRenderer.removeListener('player:load-failed', handler) }
  }
}

const analysisBridge = {
  loadFile: (filePath: string): Promise<{ ok: boolean; error?: string; data?: unknown; trackId?: number; trackName?: string }> =>
    ipcRenderer.invoke('analysis:load-file', filePath),
  getLapData: (
    lapNum: number, rowTypeMask = 0xFFFFFFFF,
    source: 'file1' | 'file2' = 'file2', driverIndex = -1,
  ): Promise<unknown | null> =>
    ipcRenderer.invoke('analysis:get-lap-data', lapNum, rowTypeMask >>> 0, source, driverIndex),
  compareLaps: (
    currentLapNum: number,
    currentSource: 'file1' | 'file2',
    currentDriverIndex: number,
    comparisonLapNum: number,
    comparisonSource: 'file1' | 'file2',
    comparisonDriverIndex: number,
    sectorDelta: boolean,
  ): Promise<unknown | null> => ipcRenderer.invoke(
    'analysis:compare-laps', currentLapNum, currentSource, currentDriverIndex,
    comparisonLapNum, comparisonSource, comparisonDriverIndex, sectorDelta,
  ),
  closeFile: (): void => ipcRenderer.send('analysis:close-file'),
}

contextBridge.exposeInMainWorld('electronStore', storeAPI)
contextBridge.exposeInMainWorld('platform', process.platform)
contextBridge.exposeInMainWorld('telemetryBridge', telemetryBridge)
contextBridge.exposeInMainWorld('windowControls', windowControls)
contextBridge.exposeInMainWorld('udpBridge', udpBridge)
contextBridge.exposeInMainWorld('debugBridge', debugBridge)
contextBridge.exposeInMainWorld('protocolBridge', protocolBridge)
contextBridge.exposeInMainWorld('strategyBridge', strategyBridge)
contextBridge.exposeInMainWorld('fsBridge', fsBridge)
contextBridge.exposeInMainWorld('recordingBridge', recordingBridge)
contextBridge.exposeInMainWorld('updateBridge', updateBridge)
contextBridge.exposeInMainWorld('pairingBridge', pairingBridge)
contextBridge.exposeInMainWorld('playerBridge', playerBridge)
contextBridge.exposeInMainWorld('analysisBridge', analysisBridge)
