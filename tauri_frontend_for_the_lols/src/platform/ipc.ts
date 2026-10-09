import { Channel, invoke as tauriInvoke } from '@tauri-apps/api/core'
import { listen } from '@tauri-apps/api/event'
import { getCurrentWindow } from '@tauri-apps/api/window'
import { open, save } from '@tauri-apps/plugin-dialog'
import { openPath, openUrl } from '@tauri-apps/plugin-opener'

type Listener = (event: unknown, ...args: any[]) => void
type Row = Record<string, any>
export interface PlaybackState {
  isPlaying: boolean; speed: number; progressPct: number; currentTime: number
  totalTime: number; filename: string | null; isScanning: boolean
}
const listeners = new Map<string, Set<Listener>>()
const cached = new Map<string, any[]>()
function emit(channel: string, ...args: any[]): void {
  if (['playback_state', 'udp-status', 'pairing:state', 'window-maximized', 'window-fullscreen-changed'].includes(channel))
    cached.set(channel, args)
  for (const listener of listeners.get(channel) ?? []) {
    try { listener(undefined, ...args) } catch (error) { console.error(channel, error) }
  }
}
let settings: Row = {}
export let hostPlatform = 'win32'
let logDir = ''
let settingsWrites = Promise.resolve()
let engineReadyResolve: () => void
const engineReady = new Promise<void>((resolve) => {
  engineReadyResolve = resolve
})
let commands: Promise<unknown> = Promise.resolve()
function native(op: string, fields: Row = {}): Promise<any> {
  const task = commands.catch(() => {}).then(async () => {
    await engineReady
    if (op === 'seek' && fields.requestId !== latestSeek) return null
    return tauriInvoke('engine_command', { command: { op, ...fields } })
  })
  commands = task
  return task
}
function get(key: string, fallback?: unknown): any {
  let value: any = settings
  for (const part of key.split('.')) value = value?.[part]
  return value ?? fallback
}
function set(key: string, value: unknown): void {
  const parts = key.split('.')
  if (parts.some(p => !p || ['__proto__', 'constructor', 'prototype'].includes(p))) throw new Error('Invalid settings key')
  let cursor = settings
  for (const part of parts.slice(0, -1)) cursor = cursor[part] ??= {}
  cursor[parts[parts.length - 1]] = value
  settingsWrites = settingsWrites.catch(() => {}).then(() => tauriInvoke<void>('settings_set', { key, value }))
  void settingsWrites.catch(error => reportError('Save settings', error))
  if (key.startsWith('logging.')) {
    void native('logging', { enabled: get('logging.enabled', false), outputDirectory: get('logging.directory', '') })
      .catch(error => reportError('Recording settings', error))
  }
}
function debugSettings() {
  return Object.fromEntries(['additionalLogging', 'reactScan', 'webglMonitor', 'memoryLog', 'nodeApiExceptions']
    .map(key => [key, get('debug.' + key, false)]))
}
let protocolRow: Row | null = null
let filename: string | null = null
let activePath: string | null = null
let visible = true
let visibilitySequence = 0
let playbackRequest = 0
let latestSeek = 0
let requirementRequest = 0
let phase: 'idle' | 'waiting-flush' | 'waiting-renderer' = 'idle'
let waitingId = 0
let buffered: Array<{ channel: string; value: string | Uint8Array }> = []
let bufferedBytes = 0
let lastRequirements: Row | null = null
let playback: PlaybackState = { isPlaying: false, speed: 1, progressPct: 0, currentTime: 0, totalTime: 0, filename: null, isScanning: false }
const decoder = new TextDecoder()
const encoder = new TextEncoder()
function resetSeek() { phase = 'idle'; waitingId = 0; buffered = []; bufferedBytes = 0 }
function state(patch: Partial<PlaybackState>) {
  playback = { ...playback, filename, ...patch }
  emit('playback_state', playback)
}
function buffer(channel: string, value: string | Uint8Array) {
  buffered.push({ channel, value })
  bufferedBytes += typeof value === 'string' ? encoder.encode(value).byteLength : value.byteLength
  if (bufferedBytes > 64 * 1024 * 1024) {
    resetSeek()
    reportError('Playback', 'History installation exceeded its buffer limit; restoring the current timeline.')
    restore()
  }
}
function reportError(operation: string, error: unknown) {
  console.error(operation, error)
  emit('recording-error', { operation, message: String(error), path: activePath ?? '' })
}
function restore() {
  const hiddenSequence = ++visibilitySequence
  const shownSequence = ++visibilitySequence
  void native('visibility', { visible: false, sequence: hiddenSequence })
    .then(() => native('visibility', { visible, sequence: shownSequence }))
    .catch(error => reportError('Restore telemetry', error))
}
function receiveJson(batch: string) {
  const independent = /"type":"playback_(lap_blocks|lap_data|loaded|close)"/.test(batch)
  if (phase === 'waiting-flush') {
    const safe = batch.split('\n').filter(row => /"type":"playback_(lap_data|lap_blocks)"/.test(row)).join('\n')
    if (safe) emit('telemetry-batch', safe + '\n')
  } else if (phase === 'waiting-renderer' && !independent) buffer('telemetry-batch', batch)
  else if (visible || independent) emit('telemetry-batch', batch)
  if (!/"type":"(protocol_status|playback_state|playback_close|recording_error)"/.test(batch)) return
  for (const line of batch.split('\n')) {
    if (!line) continue
    const row = JSON.parse(line) as Row
    if (row.type === 'protocol_status') {
      protocolRow = row
      if (row.override === 'auto' && row.detected_format != null) set('udp.lastDetectedProtocol', row.detected_format)
      emit('telemetry', row)
    } else if (row.type === 'playback_state') {
      state({ isPlaying: !!row.playing, speed: row.speed ?? 1, progressPct: row.total_time > 0 ? row.current_time / row.total_time : 0,
        currentTime: (row.start_time ?? 0) + (row.current_time ?? 0), totalTime: row.total_time ?? 0 })
    } else if (row.type === 'playback_close') {
      activePath = filename = null
      state({ isPlaying: false, speed: 1, progressPct: 0, currentTime: 0, totalTime: 0, isScanning: false })
    } else if (row.type === 'recording_error') emit('recording-error', row)
  }
}
function receive(header: Row, bytes: Uint8Array) {
  const payload = header.payload
  switch (header.kind) {
    case 'json': receiveJson(payload); break
    case 'binary':
      if (phase === 'waiting-flush') return
      if (phase === 'waiting-renderer') buffer('telemetry-binary', bytes)
      else if (visible) emit('telemetry-binary', bytes)
      break
    case 'flush':
      if (payload.authoritativeSeek) {
        if (payload.requestId !== 0 && payload.requestId !== latestSeek) return
        if (payload.requestId !== 0 && payload.requestId === waitingId) phase = 'waiting-renderer'
      } else if (payload.requestId !== 0 && payload.requestId <= latestSeek) return
      emit('telemetry', { ...payload, binary: bytes.length ? bytes : null })
      break
    case 'live-lap': emit('live-lap-data', payload, bytes); break
    case 'pair': emit('pairing:state', payload); break
    case 'export': emit('player:export-progress', payload.pct, payload.stage); break
    case 'gap': restore(); break
    case 'diagnostic': if (get('debug.additionalLogging', false)) console.info('[native]', payload); break
  }
}
async function windowState() {
  const window = getCurrentWindow()
  emit('window-maximized', await window.isMaximized())
  emit('window-fullscreen-changed', await window.isFullscreen())
}
async function selectFile(directory: boolean): Promise<string | null> {
  const path = await open({
    directory, multiple: false, defaultPath: get('dialogs.lastDirectory', undefined),
    ...(directory ? {} : { filters: [{ name: 'Track N Race Data', extensions: ['tnrd', 'trnd'] }] }),
  })
  if (typeof path !== 'string') return null
  set('dialogs.lastDirectory', directory ? path : path.replace(/[\\/][^\\/]+$/, ''))
  return path
}
let loadInProgress = false
async function loadFile(path: string) {
  if (loadInProgress) return { ok: false, error: 'A recording is already loading' }
  loadInProgress = true
  latestSeek = ++playbackRequest
  resetSeek()
  try {
    if (activePath) await native('close')
    // Close/load events share the binary channel. Wait for the close event
    // before installing the new filename; native command completion alone
    // does not imply the webview has consumed its stream.
    if (activePath) await waitForClose()
    activePath = path; filename = path.split(/[\\/]/).pop() ?? path
    state({ isScanning: true })
    await native('load', { path })
    state({ isScanning: false })
    return { ok: true }
  } catch (error) {
    activePath = filename = null
    state({ isScanning: false, isPlaying: false })
    emit('player:load-failed', String(error))
    return { ok: false, error: String(error) }
  } finally { loadInProgress = false }
}
function waitForClose(): Promise<void> {
  if (!activePath) return Promise.resolve()
  return new Promise((resolve, reject) => {
    const listener: Listener = (_event, value: PlaybackState) => {
      if (!value.filename) { clearTimeout(timeout); ipcRenderer.removeListener('playback_state', listener); resolve() }
    }
    const timeout = window.setTimeout(() => {
      ipcRenderer.removeListener('playback_state', listener)
      reject(new Error('Timed out waiting for the previous recording to close'))
    }, 10000)
    ipcRenderer.on('playback_state', listener)
  })
}
async function dispatch(channel: string, args: any[]): Promise<any> {
  const [a, b, c, d, e, f, g] = args
  const window = getCurrentWindow()
  switch (channel) {
    case 'store-set': set(a, b); return
    case 'debug-settings-get': return debugSettings()
    case 'debug-settings-set':
      set('debug.' + a, b); emit('debug-settings-changed', debugSettings())
      if (a === 'additionalLogging') await native('diagnostics', { enabled: b })
      return
    case 'diagnostics:telemetry-retention': return tauriInvoke('diagnostics', { snapshot: a })
    case 'diagnostics:open-folder': return openPath(logDir)
    case 'dialog:showOpenDialog': return selectFile(true)
    case 'dialog:showOpenDialogTNRD': return selectFile(false)
    case 'window-minimize':
      if (await window.isFullscreen()) await window.setFullscreen(false)
      return window.minimize()
    case 'window-maximize':
      if (await window.isFullscreen()) await window.setFullscreen(false)
      else await window.toggleMaximize()
      return windowState()
    case 'window-fullscreen': await window.setFullscreen(!await window.isFullscreen()); return windowState()
    case 'window-close': return window.close()
    case 'window-minimize-to-tray':
      visible = false
      await native('visibility', { visible: false, sequence: ++visibilitySequence })
      return window.hide()
    case 'page-visibility':
      if (visible !== a) { visible = a; await native('visibility', { visible, sequence: ++visibilitySequence }) }
      if (visible && protocolRow) emit('telemetry', protocolRow)
      return
    case 'udp-get-status': return cached.get('udp-status')?.[0] ?? { ok: true }
    case 'udp-restart': {
      // Wait for recording-setting writes and prior native calls first.
      await settingsWrites; await commands.catch(() => {})
      if (activePath) { await native('close'); if (activePath) await waitForClose() }
      resetSeek(); latestSeek = ++playbackRequest
      const status = await tauriInvoke('engine_start')
      if (lastRequirements) await native('requirements', { ...lastRequirements, requestId: ++requirementRequest })
      await native('visibility', { visible, sequence: ++visibilitySequence })
      emit('udp-status', status); emit('udp-restart-result', status); return status
    }
    case 'protocol-get-config':
      return { override: protocolRow?.override ?? get('udp.protocol', 'auto'),
        detected: protocolRow?.detected_format ?? null, active: protocolRow?.active_format ?? null,
        lastDetected: get('udp.lastDetectedProtocol', null) }
    case 'protocol-set-override': set('udp.protocol', a); return native('protocol', { format: a })
    case 'protocol-get-team-colors': return { catalog: await native('team-catalog'), overrides: get('teamColorOverrides', {}) }
    case 'protocol-set-team-colors': set('teamColorOverrides', a); return native('team-colors', { teamColorOverrides: a })
    case 'protocol-request-status': if (protocolRow) emit('telemetry', protocolRow); return
    case 'strategy-set-minimum-stops': return native('strategy', { strategyMinimumStops: Math.min(8, Math.max(0, Math.trunc(Number(a) || 0))) })
    case 'pairing:get-state': return native('pair-state')
    case 'pairing:set-enabled': return native(a ? 'pair-start' : 'pair-stop')
    case 'pairing:open-window': return native('pair-open')
    case 'pairing:close-window': return native('pair-close')
    case 'pairing:remove-device': return native('pair-remove', { id: a })
    case 'updates:check-on-startup': return tauriInvoke('update_check', { version: __APP_VERSION__ })
    case 'updates:skip-version': set('updates.skippedVersion', a); return
    case 'updates:open-download-page': return openUrl('https://github.com/NoGoat/Track-N-Race/releases/latest')
    case 'player:load': return loadFile(a)
    case 'player:play': return native('play')
    case 'player:pause': return native('pause')
    case 'player:close': latestSeek = ++playbackRequest; resetSeek(); return native('close')
    case 'player:setSpeed': return native('speed', { value: a })
    case 'player:setDriver': return native('driver', { driverIndex: a, useRecordedRows: b })
    case 'player:setFocusDriver': return native('focus-driver', { driverIndex: a })
    case 'player:seek': {
      const requestId = ++playbackRequest; latestSeek = requestId; resetSeek()
      phase = 'waiting-flush'; waitingId = requestId
      try { await native('seek', { value: a, allHistory: b, rowTypeMask: c, windowSeconds: d, requestId }) }
      catch (error) {
        emit('telemetry', { type: 'playback_seek_flush_failed', requestId })
        if (waitingId === requestId) resetSeek()
        throw error
      }
      return
    }
    case 'player:seek-installed':
      if (phase === 'waiting-renderer' && waitingId === a) {
        const pending = buffered; resetSeek()
        if (visible) for (const item of pending) emit(item.channel, item.value)
      }
      return
    case 'player:setDataRequirements':
      lastRequirements = { streamMask: a, historyMask: b, windowSeconds: c, v6Types: d, v6HistoryTypes: e }
      return native('requirements', { ...lastRequirements, requestId: ++requirementRequest })
    case 'player:getLapData': return native('lap', { lapNum: a, rowTypeMask: b ?? 0xFFFFFFFF })
    case 'player:getAllLapsData': return native('all-laps', { rowTypeMask: a ?? 0xFFFFFFFF, requestId: ++playbackRequest })
    case 'player:getWindowData': return native('window', { windowSeconds: a, rowTypeMask: b ?? 0xFFFFFFFF, requestId: ++playbackRequest })
    case 'live:getFastestLap': return native('live-fastest', { requestId: a })
    case 'live:getLap': return native('live-lap', { requestId: a, lapNum: b })
    case 'engine:lap-history-car': return native('lap-history-car', { driverIndex: a })
    case 'analysis:load-file':
      try { return { ok: true, ...await native('analysis-load', { path: a }) } }
      catch (error) { return { ok: false, error: String(error) } }
    case 'analysis:get-lap-data': return native('analysis-lap', { lapNum: a, rowTypeMask: b, secondary: c === 'file2', driverIndex: d })
    case 'analysis:compare-laps': return native('analysis-compare', {
      lapNum: a, secondary: b === 'file2', driverIndex: c,
      comparisonLap: d, comparisonSecondary: e === 'file2', comparisonDriver: f, sectorDelta: g,
    })
    case 'analysis:close-file': return native('analysis-close')
    case 'player:export-xlsx': {
      const path = activePath
      if (!path) return { ok: false, error: 'No session loaded' }
      const destination = await save({ title: 'Export Session to Excel', defaultPath: path.replace(/\.(tnrd|trnd)$/i, '.xlsx'),
        filters: [{ name: 'Excel Workbook', extensions: ['xlsx'] }] })
      if (!destination) return { ok: false, error: 'cancelled' }
      try { await native('export', { path, destination }); return { ok: true } }
      catch (error) { return { ok: false, error: String(error) } }
    }
    default: throw new Error('Unsupported host operation: ' + channel)
  }
}
export const ipcRenderer = {
  on(channel: string, listener: Listener) {
    let entries = listeners.get(channel)
    if (!entries) listeners.set(channel, entries = new Set())
    entries.add(listener)
    const previous = cached.get(channel)
    if (previous) queueMicrotask(() => { if (entries.has(listener)) listener(undefined, ...previous) })
  },
  once(channel: string, listener: Listener) {
    const wrapped: Listener = (...args) => { ipcRenderer.removeListener(channel, wrapped); listener(...args) }
    ipcRenderer.on(channel, wrapped)
  },
  removeListener(channel: string, listener: Listener) { listeners.get(channel)?.delete(listener) },
  sendSync(channel: string, ...args: any[]): any {
    if (channel !== 'store-get') throw new Error('Only cached settings support synchronous access')
    return get(args[0], args[1])
  },
  send(channel: string, ...args: any[]): void {
    void dispatch(channel, args).catch(error => {
      if (channel === 'udp-restart') {
        const status = { ok: false, error: String(error) }
        emit('udp-status', status); emit('udp-restart-result', status)
      }
      reportError(channel, error)
    })
  },
  invoke(channel: string, ...args: any[]): Promise<any> { return dispatch(channel, args) },
}
// Keeps the renderer's existing API contract; this does not expose Node APIs.
export const contextBridge = {
  exposeInMainWorld(key: string, value: unknown) { Object.defineProperty(window, key, { value, configurable: false }) },
}

export async function initializeHost() {
  const boot = await tauriInvoke<{ settings: Row; platform: string; logDir: string }>('bootstrap')
  settings = boot.settings
  settings.nativeTitlebar ??= true
  hostPlatform = boot.platform
  logDir = boot.logDir
  const channel = new Channel<ArrayBuffer>()
  channel.onmessage = buffer => {
    let header: Row | undefined
    try {
      const bytes = new Uint8Array(buffer)
      if (bytes.length < 4) throw new Error('Truncated native envelope')
      const size = new DataView(buffer).getUint32(0, true)
      if (size > bytes.length - 4) throw new Error('Invalid native envelope length')
      header = JSON.parse(decoder.decode(bytes.subarray(4, 4 + size)))
      receive(header!, bytes.subarray(4 + size))
    } catch (error) { reportError('Native stream', error) }
    finally {
      if (header) void tauriInvoke('stream_ack', { generation: header.generation, sequence: header.sequence })
        .catch(error => console.error('Stream acknowledgement', error))
    }
  }
  await tauriInvoke('stream_start', { channel })
  await listen<string>('open-recording', event => emit('player:request-open-confirm', event.payload))
  document.addEventListener('click', event => {
    const anchor = event.target instanceof Element ? event.target.closest<HTMLAnchorElement>('a[href]') : null
    if (!anchor) return
    const url = new URL(anchor.href)
    if (url.protocol === 'https:' || url.protocol === 'http:') {
      event.preventDefault()
      void openUrl(url.href).catch(error => reportError('Open link', error))
    }
  })
  await getCurrentWindow().onResized(() => { void windowState() })
  await windowState()
}
export async function startEngine() {
  try {
    const status = await tauriInvoke('engine_start')
    emit('udp-status', status)
    engineReadyResolve()
  } catch (error) {
    engineReadyResolve()
    emit('udp-status', { ok: false, error: String(error) })
    reportError('Start telemetry engine', error)
  }
  const files = await tauriInvoke<string[]>('renderer_ready')
  if (files.length) await loadFile(files[0])
}
declare const __APP_VERSION__: string
