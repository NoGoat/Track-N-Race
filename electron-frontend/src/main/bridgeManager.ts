import { app, BrowserWindow } from 'electron'
import * as path from 'path'
import { chartHistoryRecords } from './binaryRows'
import { configStore as store } from './configStore'

type ProtocolOverride = 'auto' | 'f1_24' | 'f1_25' | 'f1_26'
interface UdpForwardTarget { address: string; port: number }

function storedForwardTargets(): UdpForwardTarget[] {
  if (!(store.get('udp.forwardingEnabled', false) as boolean)) return []
  const value = store.get('udp.forwardTargets', [])
  if (!Array.isArray(value)) return []
  return value.slice(0, 15).flatMap((item): UdpForwardTarget[] => {
    if (!item || typeof item !== 'object') return []
    const candidate = item as Record<string, unknown>
    const address = typeof candidate.address === 'string' ? candidate.address.trim() : ''
    const port = Number(candidate.port)
    return address && Number.isInteger(port) && port >= 1 && port <= 65535
      ? [{ address, port }]
      : []
  })
}

let lastStatus: { override: ProtocolOverride; detected: number | null; active: number | null } = {
  override: (store.get('udp.protocol', 'auto') as ProtocolOverride),
  detected: null,
  active: null,
}

// The full last protocol_status row (labels/cardColors/format/aero_mode), cached
// so a renderer that missed the one-shot emission can pull it on demand (e.g. on
// mount, or when it notices it's running on default/fallback labels). See
// requestStatus() below.
let lastStatusRow: Record<string, unknown> | null = null

// Whether the renderer is currently visible (driven by the renderer's
// document.visibilityState via the page-visibility IPC). When it's
// hidden/minimized/occluded, Chromium background-throttles it, so continuing to
// push 60Hz telemetry just buffers in the IPC channel and janks hard on refocus.
// We pause forwarding the hot channels while hidden; the engine/recording keep
// running unaffected.
let rendererVisible = true

interface TimedBinary { at: number; data: Buffer }
interface TimedJson { at: number; data: string }
let resumeWindowMs = 30_000
let hiddenBinary: TimedBinary[] = []
let hiddenJson: TimedJson[] = []
let hiddenBinaryStart = 0
let hiddenJsonStart = 0

function clearResumeCache(): void {
  hiddenBinary = []
  hiddenJson = []
  hiddenBinaryStart = 0
  hiddenJsonStart = 0
}

function trimResumeCache(now: number): void {
  const cutoff = now - resumeWindowMs
  while (hiddenBinaryStart < hiddenBinary.length && hiddenBinary[hiddenBinaryStart].at < cutoff) hiddenBinaryStart++
  while (hiddenJsonStart < hiddenJson.length && hiddenJson[hiddenJsonStart].at < cutoff) hiddenJsonStart++
  // Compact in chunks rather than slicing a long window on every 60 Hz tick.
  if (hiddenBinaryStart >= 4096) {
    hiddenBinary = hiddenBinary.slice(hiddenBinaryStart)
    hiddenBinaryStart = 0
  }
  if (hiddenJsonStart >= 512) {
    hiddenJson = hiddenJson.slice(hiddenJsonStart)
    hiddenJsonStart = 0
  }
}

function cacheResumeJson(batch: string, now: number): void {
  let start = 0
  while (start < batch.length) {
    let end = batch.indexOf('\n', start)
    if (end === -1) end = batch.length
    if (end > start) {
      const row = batch.slice(start, end)
      // Only cold chart histories need backfilling. Other panels receive their
      // next current-state row normally, without replaying stale banners/events.
      if (row.includes('"type":"status"') || row.includes('"type":"damage"')) {
        hiddenJson.push({ at: now, data: row })
      }
    }
    start = end + 1
  }
  trimResumeCache(now)
}

function sendResumeCache(): void {
  if (hiddenBinaryStart === hiddenBinary.length && hiddenJsonStart === hiddenJson.length) return
  const binary = hiddenBinaryStart === hiddenBinary.length
    ? Buffer.alloc(0)
    : Buffer.concat(hiddenBinary.slice(hiddenBinaryStart).map(entry => entry.data))
  const coldJson = hiddenJson.slice(hiddenJsonStart).map(entry => entry.data).join('\n')
  for (const win of BrowserWindow.getAllWindows()) {
    if (!win.isDestroyed()) win.webContents.send('telemetry-resume', { binary, coldJson })
  }
}

let engine: any = null
let nextDataRequirementsRequestId = 0
let unsubLogging: Array<() => void> = []

// ── Playback (driven by the C++ engine's player, see tnrp::Engine) ──────────
// The native engine owns the clock/index/seek logic and streams rows through
// the same JSON/binary channels as live telemetry; this layer only tracks the
// loaded file, adapts playback_state to the renderer's shape, and forwards the
// binary seek flush.
export interface PlaybackState {
  isPlaying: boolean
  speed: number
  progressPct: number
  currentTime: number   // absolute session_time (start_time + relative cursor)
  totalTime: number
  filename: string | null
  isScanning: boolean
}

let activeFilePath: string | null = null
let onPlaybackState: ((state: PlaybackState) => void) | null = null
let nextPlaybackRequestId = 0
let latestSeekRequestId = 0

function activeFilename(): string | null {
  return activeFilePath ? path.basename(activeFilePath) : null
}

function emitPlaybackState(state: Partial<PlaybackState>): void {
  if (!onPlaybackState) return
  onPlaybackState({
    isPlaying: false,
    speed: 1,
    progressPct: 0,
    currentTime: 0,
    totalTime: 0,
    filename: activeFilename(),
    isScanning: false,
    ...state,
  })
}

// The addon already coalesces hot rows with at most one native-to-JS flush in
// flight. Forward each real batch unchanged so display data keeps the engine's
// session_time values and no synthetic samples can cross lap boundaries.
function forwardBinary(batch: Uint8Array): void {
  if (rendererVisible) {
    for (const win of BrowserWindow.getAllWindows()) {
      if (!win.isDestroyed()) win.webContents.send('telemetry-binary', batch)
    }
  } else {
    const history = chartHistoryRecords(Buffer.from(batch))
    if (history.length > 0) hiddenBinary.push({ at: performance.now(), data: history })
    trimResumeCache(performance.now())
  }
}

function broadcast(row: Record<string, unknown>): void {
  for (const win of BrowserWindow.getAllWindows()) {
    if (!win.isDestroyed()) win.webContents.send('telemetry', row)
  }
}

function handleRow(row: Record<string, unknown>): void {
  const type = row.type as string

  if (type === 'protocol_status') {
    lastStatus = {
      override: (row.override as ProtocolOverride) ?? 'auto',
      detected: (row.detected_format as number) ?? null,
      active: (row.active_format as number) ?? null,
    }
    lastStatusRow = row
    if (lastStatus.override === 'auto' && lastStatus.detected != null) {
      store.set('udp.lastDetectedProtocol', lastStatus.detected)
    }
    broadcast(row)
    return
  }

  broadcast(row)
}

// Playback control rows intercepted from the engine's JSON batch (they also
// flow through to the renderer inside the batch, which ignores the ones it
// doesn't know).
function handlePlaybackRow(row: Record<string, unknown>): void {
  const type = row.type as string
  if (type === 'playback_state') {
    const total = (row.total_time as number) ?? 0
    const current = (row.current_time as number) ?? 0
    emitPlaybackState({
      isPlaying: !!row.playing,
      speed: (row.speed as number) ?? 1,
      progressPct: total > 0 ? current / total : 0,
      currentTime: ((row.start_time as number) ?? 0) + current,
      totalTime: total,
    })
  } else if (type === 'playback_close') {
    activeFilePath = null
    clearResumeCache()
    emitPlaybackState({})   // paused, no file
  }
}

interface RecordingError {
  operation: string
  message: string
  path: string
}

function handleRecordingError(row: Record<string, unknown>): void {
  const error: RecordingError = {
    operation: typeof row.operation === 'string' ? row.operation : 'unknown operation',
    message: typeof row.message === 'string' ? row.message : 'Unknown recording error',
    path: typeof row.path === 'string' ? row.path : '',
  }
  // initializeDiagnostics() captures console.error and writes it to the
  // per-launch main.log before forwarding it to the original console.
  console.error('[recording] Native writer error:', error)
  for (const win of BrowserWindow.getAllWindows()) {
    if (!win.isDestroyed()) win.webContents.send('recording-error', error)
  }
}

let addonModule: any = null
function loadAddon(): any {
  // Try to load the N-API module
  // Electron's require correctly handles ASAR unpacking for .node files automatically.
  if (addonModule) return addonModule
  if (process.env.TNR_SIMULATE_BRIDGE_FAILURE === '1') {
    throw new Error(
      'Simulated native telemetry addon load failure (TNR_SIMULATE_BRIDGE_FAILURE=1)'
    )
  }
  let p = path.join(app.getAppPath(), 'node_addon', 'build', 'Release', 'protocol_parser.node')
  addonModule = require(p)
  return addonModule
}

function pushLogging(): void {
  if (engine) {
    const enabled = store.get('logging.enabled', false) as boolean
    const dir = store.get('logging.directory', '') as string
    engine.setLogging(enabled, dir)
  }
}

export function startBridge(): string | null {
  if (engine) return null

  try {
    const addon = loadAddon()
    const config = {
      format: store.get('udp.protocol', 'auto'),
      port: store.get('udp.port', 20777),
      bindAddress: store.get('udp.bindAddress', '0.0.0.0'),
      forwardTargets: storedForwardTargets(),
      // Playback fast path: hot playback rows arrive on the binary channel
      // unchanged, with seeks delivered via the dedicated flush callback.
      binaryPlayback: true
    }

    engine = new addon.Engine(config, (batch: string) => {
      // Skip forwarding to a hidden renderer; playback delivers its cold rows
      // through this channel too, so it's a high-volume path worth gating —
      // except one-shot playback control rows, which must never be dropped.
      const forwardWhileHidden =
        batch.includes('"type":"playback_lap_blocks"') ||
        batch.includes('"type":"playback_loaded"') ||
        batch.includes('"type":"playback_close"')
      if (rendererVisible || forwardWhileHidden) {
        for (const win of BrowserWindow.getAllWindows()) {
          if (!win.isDestroyed()) win.webContents.send('telemetry-batch', batch)
        }
      } else {
        cacheResumeJson(batch, performance.now())
      }

      // Control-row interception (always runs, visible or not): protocol_status
      // feeds the label cache; playback_state/playback_close drive the
      // playback-state channel and the local playback bookkeeping.
      if (batch.includes('"type":"protocol_status"') ||
          batch.includes('"type":"playback_state"') ||
          batch.includes('"type":"playback_close"') ||
          batch.includes('"type":"recording_error"')) {
        let start = 0
        while (start < batch.length) {
          let end = batch.indexOf('\n', start)
          if (end === -1) end = batch.length
          if (end > start) {
            const rowStr = batch.slice(start, end)
            if (rowStr.includes('"type":"protocol_status"')) {
              handleRow(JSON.parse(rowStr))
            } else if (rowStr.includes('"type":"recording_error"')) {
              try { handleRecordingError(JSON.parse(rowStr)) } catch (e) {
                console.error('[recording] Failed to parse native writer error:', e, rowStr)
              }
            } else if (rowStr.includes('"type":"playback_state"') ||
                       rowStr.includes('"type":"playback_close"')) {
              try { handlePlaybackRow(JSON.parse(rowStr)) } catch (e) {}
            }
          }
          start = end + 1
        }
      }
    }, (binBatch: Uint8Array) => {
      forwardBinary(binBatch)
    }, (binary: Buffer, coldJson: string, currentLapStart: number, lapNum: number, allHistory: boolean, requestId: number, authoritativeSeek: boolean, rowTypeMask: number) => {
      // Superseded scrubs are discarded before the large payload crosses IPC
      // or is decoded into renderer objects.
      // Authoritative scrubs supersede one another. History-family requests are
      // additive and may complete out of order; accept all of them unless a
      // newer authoritative seek/load invalidated their timeline.
      if (authoritativeSeek) {
        if (requestId !== 0 && requestId !== latestSeekRequestId) return
      } else if (requestId !== 0 && requestId <= latestSeekRequestId) return
      try {
        clearResumeCache()
        broadcast({ type: 'playback_seek_flush_bin', binary, coldJson, currentLapStart, lapNum, allHistory, requestId, authoritativeSeek, rowTypeMask })
      } catch (error) {
        console.error('[bridge] Failed to forward playback seek history:', error)
        // Never leave the renderer's AL publication gate closed if IPC rejects
        // a payload. It can resume from the post-seek stream and retry later.
        broadcast({ type: 'playback_seek_flush_failed', requestId })
      }
    })

    if (!engine.startUdp()) {
      const error = engine.udpLastError?.() || 'Failed to start the UDP listener.'
      engine.destroy()
      engine = null
      return error
    }
    pushLogging()
    
    // Listen for logging changes
    unsubLogging = [
      store.onDidChange('logging.enabled', () => pushLogging()),
      store.onDidChange('logging.directory', () => pushLogging()),
    ]

    return null
  } catch (err) {
    console.error('[bridge] Failed to load N-API addon:', err)
    if (err instanceof Error) {
      return err.stack || `${err.name}: ${err.message}`
    }
    return String(err)
  }
}

export function stopBridge(): void {
  for (const unsub of unsubLogging) unsub()
  unsubLogging = []
  clearResumeCache()
  activeFilePath = null
  if (engine) {
    // Synchronous native barrier: preserve queued rows and the rolling buffer
    // before teardown, even though Engine destruction also finalizes the stream.
    engine.flushRecording()
    engine.playerClose()
    engine.destroy()
    engine = null
  }
}

let recordingFlushInProgress = false
export function flushRecording(): boolean {
  if (!engine || recordingFlushInProgress) return false
  recordingFlushInProgress = true
  try {
    engine.flushRecording()
    return true
  } finally {
    recordingFlushInProgress = false
  }
}

// ── Player API (thin wrappers over the C++ engine's player) ─────────────────

export function setOnPlaybackState(cb: (state: PlaybackState) => void): void {
  onPlaybackState = cb
}

export interface PlayerLoadResult { ok: boolean; error?: string }

export async function playerLoad(filePath: string): Promise<PlayerLoadResult> {
  // Invalidate workers belonging to the previously loaded timeline.
  latestSeekRequestId = ++nextPlaybackRequestId
  if (!engine) return { ok: false, error: 'The playback engine is not available.' }
  // Loading over an already-open clip: close it first so the renderer clears
  // its playback buffers (playback_close) before the new clip's rows arrive.
  if (activeFilePath) engine.playerClose()
  activeFilePath = filePath
  emitPlaybackState({ isScanning: true })
  let result: PlayerLoadResult = { ok: false, error: 'The recording could not be opened.' }
  try {
    result = await engine.playerLoad(filePath)   // async: decompress+index off-thread
  } catch (err) {
    console.error('[bridge] playerLoad failed:', err)
    result = { ok: false, error: err instanceof Error ? err.message : String(err) }
  }
  if (!result.ok) {
    activeFilePath = null
    emitPlaybackState({})
    return result
  }
  // The engine's own playback_state row (intercepted above) follows with the
  // real duration; this one just clears the scanning flag deterministically.
  emitPlaybackState({ isScanning: false })
  return result
}

export function playerPlay(): void { engine?.playerPlay() }
export function playerPause(): void { engine?.playerPause() }
export function playerSeek(pct: number, allHistory = false, rowTypeMask = 0xFFFFFFFF, windowSeconds = 0): void {
  const requestId = ++nextPlaybackRequestId
  latestSeekRequestId = requestId
  if (!app.isPackaged && process.env['ELECTRON_RENDERER_URL']) {
    console.info(`[playback-debug] ${new Date().toISOString()} main-player-seek ${JSON.stringify({ progress: pct, allHistory, windowSeconds, requestId, engineReady: Boolean(engine) })}`)
  }
  engine?.playerSeek(pct, allHistory, requestId, rowTypeMask >>> 0, Math.max(0, windowSeconds))
}
export function playerSetSpeed(mult: number): void { engine?.playerSetSpeed(mult) }
export function playerGetLapData(lapNum: number, rowTypeMask = 0xFFFFFFFF): void {
  engine?.playerGetLapData(lapNum, rowTypeMask >>> 0)
}
export function playerGetAllLapsData(rowTypeMask = 0xFFFFFFFF): void {
  const requestId = ++nextPlaybackRequestId
  engine?.playerGetAllLapsData(requestId, rowTypeMask >>> 0)
}
export function playerGetWindowData(windowSeconds: number, rowTypeMask = 0xFFFFFFFF): void {
  const requestId = ++nextPlaybackRequestId
  engine?.playerGetWindowData(Math.max(0, windowSeconds), requestId, rowTypeMask >>> 0)
}
export function playerSetDataRequirements(streamMask = 0xFFFFFFFF, historyMask = 0,
                                          windowSeconds = 0): void {
  const requestId = ++nextDataRequirementsRequestId
  engine?.setDataRequirements(streamMask >>> 0, historyMask >>> 0,
    Math.max(-1, windowSeconds), requestId)
}
export function playerClose(): void {
  latestSeekRequestId = ++nextPlaybackRequestId
  engine?.playerClose()
}

export async function analysisLoadFile(filePath: string): Promise<{ ok: boolean; error?: string; data?: unknown; trackId?: number; trackName?: string }> {
  if (!engine) return { ok: false, error: 'The telemetry engine is not available.' }
  try {
    const result = await engine.analysisLoadFile(filePath)
    if (!result.ok) return result
    return { ok: true, data: JSON.parse(result.blocksJson), trackId: result.trackId, trackName: result.trackName }
  } catch (err) {
    return { ok: false, error: err instanceof Error ? err.message : String(err) }
  }
}

export function analysisGetLapData(lapNum: number, rowTypeMask = 0xFFFFFFFF): unknown | null {
  if (!engine) return null
  const json = engine.analysisGetLapData(lapNum, rowTypeMask >>> 0)
  if (!json) return null
  try {
    return JSON.parse(json)
  } catch {
    return null
  }
}

export function analysisCloseFile(): void {
  engine?.analysisCloseFile()
}

export function getActiveFilePath(): string | null {
  return activeFilePath
}

// Reclaim stale decompression temps from prior runs (either app). Called once
// at startup after the single-instance lock is held.
export function sweepTempFiles(): void {
  try {
    loadAddon().sweepTempFiles()
  } catch (err) {
    console.error('[bridge] temp sweep failed:', err)
  }
}

export function exportSessionXlsx(
  srcPath: string,
  destPath: string,
  onProgress?: (pct: number, stage: string) => void
): Promise<{ ok: boolean; error?: string }> {
  if (!engine) return Promise.resolve({ ok: false, error: 'engine not started' })
  return engine.playerExportXlsx(srcPath, destPath, onProgress ?? (() => {}))
}

export function setOverride(value: ProtocolOverride): void {
  store.set('udp.protocol', value)
  if (engine) engine.setOverride(value)
}

// Renderer-initiated pull: re-broadcast the last known full protocol_status so a
// window that missed the one-shot emission (or fell back to default labels) can
// recover the catalog. No-op until the engine has emitted at least one status.
export function requestStatus(): void {
  if (lastStatusRow) broadcast(lastStatusRow)
}

// Renderer visibility gate: pause IPC while the window is hidden/minimized/
// occluded. Main retains one bounded chart window (not an IPC queue), sends it
// as a single resume payload on return, and refreshes the protocol catalog.
export function setRendererVisible(visible: boolean): void {
  const wasVisible = rendererVisible
  if (!visible && wasVisible) {
    const selectedSeconds = Number(store.get('timeWindow', 30))
    resumeWindowMs = Math.min(600, Math.max(15, Number.isFinite(selectedSeconds) ? selectedSeconds : 30)) * 1000
    clearResumeCache()
  }
  rendererVisible = visible
  if (visible && !wasVisible) {
    trimResumeCache(performance.now())
    // Send one bounded catch-up before normal live forwarding can resume. The
    // renderer applies it as a single store publication, avoiding an IPC burst.
    sendResumeCache()
    clearResumeCache()
    if (lastStatusRow) broadcast(lastStatusRow)
  }
}

export function isRendererVisible(): boolean { return rendererVisible }

export function getProtocolConfig(): {
  override: ProtocolOverride; detected: number | null; lastDetected: number | null; active: number | null
} {
  return {
    override: lastStatus.override,
    detected: lastStatus.detected,
    lastDetected: (store.get('udp.lastDetectedProtocol', null) as number | null),
    active: lastStatus.active,
  }
}

export function restartUdp(): string | null {
  // To restart UDP on port changes, we stop and recreate the engine
  stopBridge()
  return startBridge()
}
