import { app, BrowserWindow } from 'electron'
import * as fs from 'fs'
import * as path from 'path'
import { chartHistoryRecords, inspectBinaryBatch } from './binaryRows'
import { configStore as store } from './configStore'
import { setTelemetryRetentionProvider } from './diagnostics'
import {
  configurePairService,
  pairEngineConfig,
  receivePairServiceState,
} from './pairHostAdapter'

type ProtocolOverride = 'auto' | 'f1_24' | 'f1_25' | 'f1_26'
interface UdpForwardTarget { address: string; port: number }
type TeamColorOverrides = Record<string, Record<string, string>>

function storedTeamColorOverrides(): TeamColorOverrides {
  const value = store.get('teamColorOverrides', {})
  if (!value || typeof value !== 'object' || Array.isArray(value)) return {}
  const result: TeamColorOverrides = {}
  for (const format of ['2024', '2025', '2026']) {
    const teams = (value as Record<string, unknown>)[format]
    if (!teams || typeof teams !== 'object' || Array.isArray(teams)) continue
    for (const [id, color] of Object.entries(teams as Record<string, unknown>)) {
      if (/^\d+$/.test(id) && typeof color === 'string' && /^#[0-9a-f]{6}$/i.test(color)) {
        const formatOverrides = result[format] ?? {}
        formatOverrides[id] = color.toUpperCase()
        result[format] = formatOverrides
      }
    }
  }
  return result
}

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
let additionalLoggingEnabled = store.get('debug.additionalLogging', false) === true

const DIAGNOSTIC_INTERVAL_MS = 10_000
interface BridgeDiagnostics {
  startedAt: number
  jsonCallbacks: number
  jsonBytes: number
  jsonRows: number
  jsonTypes: Record<string, number>
  lastJsonAt: number | null
  binaryCallbacks: number
  binaryBytes: number
  binaryRecords: number
  binaryTypes: Record<string, number>
  invalidBinaryBatches: number
  lastBinaryAt: number | null
  jsonForwardedBatches: number
  jsonHiddenBatches: number
  binaryForwardedBatches: number
  binaryHiddenBatches: number
  ipcSendTargets: number
}

function freshBridgeDiagnostics(): BridgeDiagnostics {
  return {
    startedAt: Date.now(),
    jsonCallbacks: 0, jsonBytes: 0, jsonRows: 0, jsonTypes: {}, lastJsonAt: null,
    binaryCallbacks: 0, binaryBytes: 0, binaryRecords: 0, binaryTypes: {},
    invalidBinaryBatches: 0, lastBinaryAt: null,
    jsonForwardedBatches: 0, jsonHiddenBatches: 0,
    binaryForwardedBatches: 0, binaryHiddenBatches: 0, ipcSendTargets: 0,
  }
}

let bridgeDiagnostics = freshBridgeDiagnostics()
let diagnosticTimer: ReturnType<typeof setInterval> | null = null
let activeUdpConfig: Record<string, unknown> | null = null
let warnedNoDatagrams = false
let warnedNoTelemetryOutput = false
let warnedZeroConsumerMask = false
let warnedMissingNativeDiagnostics = false

function increment(map: Record<string, number>, key: string, count = 1): void {
  map[key] = (map[key] ?? 0) + count
}

function observeJsonBatch(batch: string): { rows: number; types: Record<string, number> } {
  const types: Record<string, number> = {}
  let rows = 0
  let start = 0
  while (start < batch.length) {
    let end = batch.indexOf('\n', start)
    if (end === -1) end = batch.length
    if (end > start) {
      rows++
      const match = /"type"\s*:\s*"([^"]+)"/.exec(batch.slice(start, end))
      increment(types, match?.[1] ?? '<missing-type>')
    }
    start = end + 1
  }
  bridgeDiagnostics.jsonCallbacks++
  bridgeDiagnostics.jsonBytes += Buffer.byteLength(batch)
  bridgeDiagnostics.jsonRows += rows
  bridgeDiagnostics.lastJsonAt = Date.now()
  for (const [type, count] of Object.entries(types)) increment(bridgeDiagnostics.jsonTypes, type, count)
  if (bridgeDiagnostics.jsonCallbacks === 1) {
    console.info('[telemetry-diagnostics][main] first native JSON callback:', { bytes: Buffer.byteLength(batch), rows, types })
  }
  return { rows, types }
}

function windowDiagnostics(): Array<Record<string, unknown>> {
  return BrowserWindow.getAllWindows().map(win => ({
    id: win.id,
    destroyed: win.isDestroyed(),
    visible: win.isVisible(),
    minimized: win.isMinimized(),
    focused: win.isFocused(),
    loading: win.webContents.isLoading(),
    crashed: win.webContents.isCrashed(),
    url: win.webContents.getURL(),
  }))
}

function logBridgeHealth(reason: string): void {
  if (!additionalLoggingEnabled) return
  const now = Date.now()
  let native: Record<string, unknown> | null = null
  let nativeError: string | null = null
  if (engine?.liveDiagnostics) {
    try {
      native = engine.liveDiagnostics() as Record<string, unknown>
      nativeError = String(engine.udpLastError?.() ?? '') || null
    } catch (error) {
      nativeError = `diagnostic snapshot failed: ${String(error)}`
    }
  } else if (engine && !warnedMissingNativeDiagnostics) {
    warnedMissingNativeDiagnostics = true
    console.warn('[telemetry-diagnostics][main] Native addon has no liveDiagnostics() method; the packaged addon may be stale.')
  }

  console.info('[telemetry-diagnostics][main] health:', {
    reason,
    elapsedMs: now - bridgeDiagnostics.startedAt,
    engineReady: Boolean(engine),
    udpConfig: activeUdpConfig,
    rendererVisible,
    activePlaybackFile: activeFilename(),
    protocol: lastStatus,
    native,
    nativeError,
    bridge: {
      ...bridgeDiagnostics,
      lastJsonAgeMs: bridgeDiagnostics.lastJsonAt == null ? null : now - bridgeDiagnostics.lastJsonAt,
      lastBinaryAgeMs: bridgeDiagnostics.lastBinaryAt == null ? null : now - bridgeDiagnostics.lastBinaryAt,
    },
    resumeCache: {
      binaryEntries: hiddenBinary.length - hiddenBinaryStart,
      jsonEntries: hiddenJson.length - hiddenJsonStart,
      windowMs: resumeWindowMs,
    },
    windows: windowDiagnostics(),
  })

  const datagrams = Number(native?.datagrams ?? -1)
  const produced = Number(native?.rowsProduced ?? 0) + Number(native?.binaryBytesProduced ?? 0)
  if (!warnedNoDatagrams && datagrams === 0 && now - bridgeDiagnostics.startedAt >= DIAGNOSTIC_INTERVAL_MS) {
    warnedNoDatagrams = true
    console.warn('[telemetry-diagnostics][main] Listener is bound but the native socket has received zero UDP datagrams. Check the game UDP destination IP/port, network interface, firewall, and whether UDP telemetry is enabled.')
  } else if (!warnedNoTelemetryOutput && datagrams > 0 && produced === 0) {
    warnedNoTelemetryOutput = true
    console.warn('[telemetry-diagnostics][main] UDP datagrams are reaching the native socket but the parser has produced no telemetry output. Inspect formats, packetIds, tooShort, unsupportedFormat, parserDropped, noOutput, and consumerRowMask in this snapshot.')
  }
  if (!warnedZeroConsumerMask && now - bridgeDiagnostics.startedAt >= DIAGNOSTIC_INTERVAL_MS && Number(native?.consumerRowMask ?? -1) === 0) {
    warnedZeroConsumerMask = true
    console.warn('[telemetry-diagnostics][main] Native consumerRowMask is still zero. The renderer has not declared any visible telemetry row requirements, so parsed output will be filtered before the N-API callbacks.')
  }
}

function startDiagnosticTimer(): void {
  if (diagnosticTimer) clearInterval(diagnosticTimer)
  if (!additionalLoggingEnabled) return
  bridgeDiagnostics = freshBridgeDiagnostics()
  warnedNoDatagrams = false
  warnedNoTelemetryOutput = false
  warnedZeroConsumerMask = false
  warnedMissingNativeDiagnostics = false
  diagnosticTimer = setInterval(() => logBridgeHealth('periodic'), DIAGNOSTIC_INTERVAL_MS)
  diagnosticTimer.unref?.()
}

function stopDiagnosticTimer(): void {
  if (!diagnosticTimer) return
  clearInterval(diagnosticTimer)
  diagnosticTimer = null
}

function configureAdditionalLogging(enabled: boolean): void {
  if (additionalLoggingEnabled === enabled) return
  additionalLoggingEnabled = enabled
  engine?.setDiagnosticsEnabled?.(enabled)
  if (enabled) {
    startDiagnosticTimer()
    if (engine) logBridgeHealth('additional-logging-enabled')
  } else {
    stopDiagnosticTimer()
  }
}

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
    if (!win.isDestroyed()) {
      win.webContents.send('telemetry-resume', { binary, coldJson })
      if (additionalLoggingEnabled) bridgeDiagnostics.ipcSendTargets++
    }
  }
  if (additionalLoggingEnabled) {
    console.info('[telemetry-diagnostics][main] sent renderer resume cache:', { binaryBytes: binary.length, coldJsonBytes: Buffer.byteLength(coldJson) })
  }
}

let engine: any = null
let nextDataRequirementsRequestId = 0
let rendererStreamMask = 0xFFFFFFFF
let rendererHistoryMask = 0
let rendererHistoryWindow = 0
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
type SeekForwardPhase = 'idle' | 'waiting-flush' | 'waiting-renderer'
let seekForwardPhase: SeekForwardPhase = 'idle'
let seekForwardRequestId = 0
let seekBufferedBinary: Buffer[] = []
let seekBufferedJson: string[] = []
let seekBufferedBytes = 0
const MAX_SEEK_FORWARD_BYTES = 64 * 1024 * 1024

function resetSeekForwarding(): void {
  seekForwardPhase = 'idle'
  seekForwardRequestId = 0
  seekBufferedBinary = []
  seekBufferedJson = []
  seekBufferedBytes = 0
}

function trimSeekForwardBuffer(): void {
  while (seekBufferedBytes > MAX_SEEK_FORWARD_BYTES && seekBufferedBinary.length > 0) {
    seekBufferedBytes -= seekBufferedBinary[0].byteLength
    seekBufferedBinary.shift()
  }
  while (seekBufferedBytes > MAX_SEEK_FORWARD_BYTES && seekBufferedJson.length > 0) {
    seekBufferedBytes -= Buffer.byteLength(seekBufferedJson[0])
    seekBufferedJson.shift()
  }
}

function bufferSeekJson(batch: string): void {
  seekBufferedJson.push(batch)
  seekBufferedBytes += Buffer.byteLength(batch)
  trimSeekForwardBuffer()
}

function bufferSeekBinary(batch: Uint8Array): void {
  const retained = Buffer.from(batch)
  seekBufferedBinary.push(retained)
  seekBufferedBytes += retained.byteLength
  trimSeekForwardBuffer()
}

function timedBinaryRetention(entries: TimedBinary[], activeStart: number): {
  entries: number
  activeEntries: number
  payloadBytes: number
  activePayloadBytes: number
} {
  let payloadBytes = 0
  let activePayloadBytes = 0
  for (let index = 0; index < entries.length; index++) {
    const bytes = entries[index].data.byteLength
    payloadBytes += bytes
    if (index >= activeStart) activePayloadBytes += bytes
  }
  return {
    entries: entries.length,
    activeEntries: Math.max(0, entries.length - activeStart),
    payloadBytes,
    activePayloadBytes,
  }
}

function timedJsonRetention(entries: TimedJson[], activeStart: number): {
  entries: number
  activeEntries: number
  payloadBytes: number
  activePayloadBytes: number
} {
  let payloadBytes = 0
  let activePayloadBytes = 0
  for (let index = 0; index < entries.length; index++) {
    const bytes = Buffer.byteLength(entries[index].data)
    payloadBytes += bytes
    if (index >= activeStart) activePayloadBytes += bytes
  }
  return {
    entries: entries.length,
    activeEntries: Math.max(0, entries.length - activeStart),
    payloadBytes,
    activePayloadBytes,
  }
}

function mainTelemetryRetentionDiagnostics(): Record<string, unknown> {
  const resumeBinary = timedBinaryRetention(hiddenBinary, hiddenBinaryStart)
  const resumeJson = timedJsonRetention(hiddenJson, hiddenJsonStart)
  const seekBinaryBytes = seekBufferedBinary.reduce((total, batch) => total + batch.byteLength, 0)
  const seekJsonBytes = seekBufferedJson.reduce((total, batch) => total + Buffer.byteLength(batch), 0)
  let nativeTransit: Record<string, unknown> | null = null
  try {
    const snapshot = engine?.telemetryRetention?.()
    if (snapshot && typeof snapshot === 'object' && !Array.isArray(snapshot)) nativeTransit = snapshot
  } catch {
    // Native diagnostics are best-effort and must not interfere with forwarding.
  }
  const nativeTransitBytes = typeof nativeTransit?.retained_bytes === 'number' &&
    Number.isFinite(nativeTransit.retained_bytes)
    ? nativeTransit.retained_bytes
    : 0
  const retainedBytes = resumeBinary.payloadBytes + resumeJson.payloadBytes +
    seekBinaryBytes + seekJsonBytes + nativeTransitBytes

  return {
    sampled_at: new Date().toISOString(),
    mode: activeFilePath ? 'playback' : 'realtime',
    retained_bytes: retainedBytes,
    byte_basis: 'exact retained Buffer/string payload bytes and reserved native transit payload bytes; container overhead excluded',
    renderer_visible: rendererVisible,
    resume_window_ms: resumeWindowMs,
    hidden_resume: {
      binary: resumeBinary,
      json: resumeJson,
      retained_bytes: resumeBinary.payloadBytes + resumeJson.payloadBytes,
    },
    seek_forward: {
      phase: seekForwardPhase,
      request_id: seekForwardRequestId,
      binary_batches: seekBufferedBinary.length,
      json_batches: seekBufferedJson.length,
      binary_bytes: seekBinaryBytes,
      json_bytes: seekJsonBytes,
      retained_bytes: seekBinaryBytes + seekJsonBytes,
      tracked_bytes: seekBufferedBytes,
      limit_bytes: MAX_SEEK_FORWARD_BYTES,
    },
    native_transit: nativeTransit,
  }
}

setTelemetryRetentionProvider(mainTelemetryRetentionDiagnostics)

function releaseSeekForwarding(requestId: number): void {
  if (seekForwardPhase !== 'waiting-renderer' || requestId !== seekForwardRequestId) return
  const binary = seekBufferedBinary.length > 0 ? Buffer.concat(seekBufferedBinary) : null
  const json = seekBufferedJson.join('')
  resetSeekForwarding()
  if (!rendererVisible) {
    const now = performance.now()
    if (binary?.length) {
      const history = chartHistoryRecords(binary)
      if (history.length > 0) hiddenBinary.push({ at: now, data: history })
    }
    if (json) cacheResumeJson(json, now)
    trimResumeCache(now)
    return
  }
  for (const win of BrowserWindow.getAllWindows()) {
    if (win.isDestroyed()) continue
    if (binary?.length) win.webContents.send('telemetry-binary', binary)
    if (json) win.webContents.send('telemetry-batch', json)
  }
}

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
  const buffer = Buffer.from(batch)
  if (additionalLoggingEnabled) {
    const inspection = inspectBinaryBatch(buffer)
    bridgeDiagnostics.binaryCallbacks++
    bridgeDiagnostics.binaryBytes += buffer.length
    bridgeDiagnostics.binaryRecords += inspection.records
    bridgeDiagnostics.lastBinaryAt = Date.now()
    increment(bridgeDiagnostics.binaryTypes, 'telemetry', inspection.telemetry)
    increment(bridgeDiagnostics.binaryTypes, 'motion', inspection.motion)
    increment(bridgeDiagnostics.binaryTypes, 'positions', inspection.positions)
    increment(bridgeDiagnostics.binaryTypes, 'motion_ex', inspection.motionEx)
    if (!inspection.valid) {
      bridgeDiagnostics.invalidBinaryBatches++
      console.error('[telemetry-diagnostics][main] invalid packed binary batch:', inspection)
    }
    if (bridgeDiagnostics.binaryCallbacks === 1) {
      console.info('[telemetry-diagnostics][main] first native binary callback:', inspection)
    }
  }
  if (seekForwardPhase === 'waiting-flush') return
  if (seekForwardPhase === 'waiting-renderer') {
    bufferSeekBinary(batch)
    return
  }
  if (rendererVisible) {
    for (const win of BrowserWindow.getAllWindows()) {
      if (!win.isDestroyed()) {
        win.webContents.send('telemetry-binary', batch)
        if (additionalLoggingEnabled) bridgeDiagnostics.ipcSendTargets++
      }
    }
    if (additionalLoggingEnabled) bridgeDiagnostics.binaryForwardedBatches++
  } else {
    if (additionalLoggingEnabled) bridgeDiagnostics.binaryHiddenBatches++
    const history = chartHistoryRecords(buffer)
    if (history.length > 0) hiddenBinary.push({ at: performance.now(), data: history })
    trimResumeCache(performance.now())
  }
}

function broadcast(row: Record<string, unknown>): void {
  for (const win of BrowserWindow.getAllWindows()) {
    if (!win.isDestroyed()) {
      win.webContents.send('telemetry', row)
      if (additionalLoggingEnabled) bridgeDiagnostics.ipcSendTargets++
    }
  }
}

// Per-lap comparison payloads are immutable indexed reads, not rows from the
// playback cursor. A request can race an in-progress seek; ordinary JSON is
// deliberately discarded while that seek waits for its authoritative flush,
// but discarding playback_lap_data leaves the renderer with no dependency
// change that would necessarily request it again. Forward only these safe rows
// in an isolated batch and keep the old-cursor rows behind the barrier.
function forwardIndexedLapDataDuringSeek(batch: string): void {
  if (!batch.includes('"type":"playback_lap_data"')) return
  let indexedRows = ''
  let start = 0
  while (start < batch.length) {
    let end = batch.indexOf('\n', start)
    if (end === -1) end = batch.length
    if (end > start) {
      const rowStr = batch.slice(start, end)
      if (rowStr.includes('"type":"playback_lap_data"')) {
        indexedRows += rowStr
        indexedRows += '\n'
      }
    }
    start = end + 1
  }
  if (!indexedRows) return
  for (const win of BrowserWindow.getAllWindows()) {
    if (!win.isDestroyed()) win.webContents.send('telemetry-batch', indexedRows)
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
    if (additionalLoggingEnabled) {
      console.info('[telemetry-diagnostics][main] protocol status:', {
        override: lastStatus.override,
        detected: lastStatus.detected,
        active: lastStatus.active,
        aeroMode: row.aero_mode ?? null,
        gameYear: (row.capabilities as Record<string, unknown> | undefined)?.gameYear ?? null,
      })
    }
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
  const p = path.join(app.getAppPath(), 'node_addon', 'build', 'Release', 'protocol_parser.node')
  if (additionalLoggingEnabled) {
    let addonFile: Record<string, unknown> = { path: p, exists: false }
    try {
      const stat = fs.statSync(p)
      addonFile = { path: p, exists: true, bytes: stat.size, modifiedAt: stat.mtime.toISOString() }
    } catch (error) {
      addonFile = { ...addonFile, statError: String(error) }
    }
    console.info('[telemetry-diagnostics][main] loading native addon:', addonFile)
  }
  addonModule = require(p)
  if (additionalLoggingEnabled) {
    console.info('[telemetry-diagnostics][main] native addon exports:', Object.keys(addonModule).sort())
  }
  return addonModule
}

function pushLogging(): void {
  if (engine) {
    const enabled = store.get('logging.enabled', false) as boolean
    const dir = store.get('logging.directory', '') as string
    if (additionalLoggingEnabled) {
      console.info('[telemetry-diagnostics][main] applying recording settings:', { enabled, directory: dir || '<default>' })
    }
    engine.setLogging(enabled, dir)
  }
}

export function startBridge(): string | null {
  if (engine) return null
  additionalLoggingEnabled = store.get('debug.additionalLogging', false) === true

  try {
    const addon = loadAddon()
    const storedPort = Number(store.get('udp.port', 20777))
    if (!Number.isInteger(storedPort) || storedPort < 1 || storedPort > 65535) {
      const error = `Invalid saved UDP port: ${String(store.get('udp.port', 20777))}. Expected an integer from 1 to 65535.`
      console.error('[udp]', error)
      return error
    }
    const config = {
      format: store.get('udp.protocol', 'auto'),
      port: storedPort,
      bindAddress: store.get('udp.bindAddress', '0.0.0.0'),
      forwardTargets: storedForwardTargets(),
      strategyMinimumStops: 1,
      teamColorOverrides: storedTeamColorOverrides(),
      // Playback fast path: hot playback rows arrive on the binary channel
      // unchanged, with seeks delivered via the dedicated flush callback.
      binaryPlayback: true,
      ...pairEngineConfig(),
    }

    activeUdpConfig = { ...config }
    if (additionalLoggingEnabled) {
      console.info('[telemetry-diagnostics][main] constructing native engine:', {
        config,
        storedLastDetectedProtocol: store.get('udp.lastDetectedProtocol', null),
        forwardingEnabled: store.get('udp.forwardingEnabled', false),
      })
      console.info(`[udp] Starting listener on ${String(config.bindAddress || '0.0.0.0')}:${config.port}`)
    }

    const pairDiagnosticCallback = (message: string): void => {
      if (additionalLoggingEnabled) console.info('[pair-native]', message)
    }
    engine = new addon.Engine(config, (batch: string) => {
      if (additionalLoggingEnabled) observeJsonBatch(batch)
      // Skip forwarding to a hidden renderer; playback delivers its cold rows
      // through this channel too, so it's a high-volume path worth gating —
      // except one-shot playback control rows, which must never be dropped.
      const forwardWhileHidden =
        batch.includes('"type":"playback_lap_blocks"') ||
        // Indexed Analysis lap payloads are one-shot request responses. Dropping
        // one during the minimize/restore visibility race permanently leaves
        // that lap marked as requested in the renderer, so comparisons and
        // sector metadata never recover. They are immutable and safe to send
        // while hidden, just like the load metadata above.
        batch.includes('"type":"playback_lap_data"') ||
        batch.includes('"type":"playback_loaded"') ||
        batch.includes('"type":"playback_close"')
      if (seekForwardPhase === 'waiting-flush') {
        forwardIndexedLapDataDuringSeek(batch)
      }
      if (seekForwardPhase === 'waiting-renderer' && !forwardWhileHidden) {
        bufferSeekJson(batch)
      } else if (seekForwardPhase !== 'waiting-flush' && (rendererVisible || forwardWhileHidden)) {
        let targets = 0
        for (const win of BrowserWindow.getAllWindows()) {
          if (!win.isDestroyed()) {
            win.webContents.send('telemetry-batch', batch)
            if (additionalLoggingEnabled) targets++
          }
        }
        if (additionalLoggingEnabled) {
          bridgeDiagnostics.jsonForwardedBatches++
          bridgeDiagnostics.ipcSendTargets += targets
        }
      } else {
        if (additionalLoggingEnabled) bridgeDiagnostics.jsonHiddenBatches++
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
    }, (binary: Buffer, coldJson: string, currentLapStart: number, lapNum: number, allHistory: boolean, requestId: number, authoritativeSeek: boolean, rowTypeMask: number, historyStart: number) => {
      // Superseded scrubs are discarded before the large payload crosses IPC
      // or is decoded into renderer objects.
      // Authoritative scrubs supersede one another. History-family requests are
      // additive and may complete out of order; accept all of them unless a
      // newer authoritative seek/load invalidated their timeline.
      if (authoritativeSeek) {
        if (requestId !== 0 && requestId !== latestSeekRequestId) return
        if (requestId !== 0 && requestId === seekForwardRequestId)
          seekForwardPhase = 'waiting-renderer'
      } else if (requestId !== 0 && requestId <= latestSeekRequestId) return
      try {
        clearResumeCache()
        broadcast({ type: 'playback_seek_flush_bin', binary, coldJson, currentLapStart, lapNum, allHistory, requestId, authoritativeSeek, rowTypeMask, historyStart })
      } catch (error) {
        console.error('[bridge] Failed to forward playback seek history:', error)
        // Never leave the renderer's AL publication gate closed if IPC rejects
        // a payload. It can resume from the post-seek stream and retry later.
        broadcast({ type: 'playback_seek_flush_failed', requestId })
        if (requestId === seekForwardRequestId) resetSeekForwarding()
      }
    }, (publicJson: string, persistedJson: string) => {
      receivePairServiceState(publicJson, persistedJson)
    }, pairDiagnosticCallback)

    engine.setDiagnosticsEnabled?.(additionalLoggingEnabled)
    if (!engine.startUdp()) {
      const error = engine.udpLastError?.() || 'Failed to start the UDP listener.'
      console.error(`[udp] Listener failed on ${String(config.bindAddress || '0.0.0.0')}:${config.port}: ${error}`)
      engine.destroy()
      engine = null
      return error
    }
    configurePairService(engine)
    if (additionalLoggingEnabled) {
      console.info(`[udp] Listener bound on ${String(config.bindAddress || '0.0.0.0')}:${config.port}`)
      startDiagnosticTimer()
      logBridgeHealth('listener-started')
    }
    pushLogging()
    
    // Listen for logging changes
    unsubLogging = [
      store.onDidChange('logging.enabled', () => pushLogging()),
      store.onDidChange('logging.directory', () => pushLogging()),
      store.onDidChange('debug.additionalLogging', value => configureAdditionalLogging(value === true)),
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

export function stopBridge(forceProcessExit = false): void {
  if (engine && additionalLoggingEnabled) logBridgeHealth('bridge-stopping')
  stopDiagnosticTimer()
  for (const unsub of unsubLogging) unsub()
  unsubLogging = []
  clearResumeCache()
  resetSeekForwarding()
  activeFilePath = null
  activeUdpConfig = null
  if (engine) {
    // With recording disabled, do not enter the native engine at all during
    // process shutdown. Any active playback/load/history read may own the
    // engine mutex, so even flushRecording() (a no-op at the writer level)
    // could wait behind that read indefinitely.
    if (forceProcessExit) return
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
  resetSeekForwarding()
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
  if (!engine) {
    resetSeekForwarding()
    return
  }
  seekForwardPhase = 'waiting-flush'
  seekForwardRequestId = requestId
  seekBufferedBinary = []
  seekBufferedJson = []
  seekBufferedBytes = 0
  if (additionalLoggingEnabled) {
    console.info(`[playback-debug] ${new Date().toISOString()} main-player-seek ${JSON.stringify({ progress: pct, allHistory, windowSeconds, requestId, engineReady: Boolean(engine) })}`)
  }
  engine.playerSeek(pct, allHistory, requestId, rowTypeMask >>> 0, Math.max(0, windowSeconds))
}
export function playerSeekInstalled(requestId: number): void {
  releaseSeekForwarding(requestId)
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
  rendererStreamMask = streamMask >>> 0
  rendererHistoryMask = historyMask >>> 0
  rendererHistoryWindow = Math.max(-1, windowSeconds)
  applyAggregateDataRequirements()
}

function applyAggregateDataRequirements(): void {
  const requestId = ++nextDataRequirementsRequestId
  if (additionalLoggingEnabled) {
    console.info('[telemetry-diagnostics][main] renderer data requirements:', {
      requestId,
      streamMask: rendererStreamMask,
      streamMaskHex: `0x${rendererStreamMask.toString(16).padStart(8, '0')}`,
      historyMask: rendererHistoryMask,
      historyMaskHex: `0x${rendererHistoryMask.toString(16).padStart(8, '0')}`,
      windowSeconds: rendererHistoryWindow,
      engineReady: Boolean(engine),
    })
  }
  engine?.setDataRequirements(rendererStreamMask,
    rendererHistoryMask, rendererHistoryWindow, requestId)
}
export function playerClose(): void {
  console.log(`[close-trace] ${new Date().toISOString()} bridge playerClose entry`)
  latestSeekRequestId = ++nextPlaybackRequestId
  resetSeekForwarding()
  console.log(`[close-trace] ${new Date().toISOString()} bridge calling native playerClose`)
  engine?.playerClose()
  console.log(`[close-trace] ${new Date().toISOString()} bridge native playerClose returned`)
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

export async function analysisCompareLaps(
  currentLapNum: number,
  currentSource: 'file1' | 'file2',
  comparisonLapNum: number,
  comparisonSource: 'file1' | 'file2',
  sectorDelta: boolean,
): Promise<unknown | null> {
  if (!engine) return null
  const json = await engine.analysisCompareLaps(
    currentLapNum,
    currentSource === 'file2',
    comparisonLapNum,
    comparisonSource === 'file2',
    sectorDelta,
  )
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
  if (additionalLoggingEnabled) {
    console.info('[telemetry-diagnostics][main] protocol override changed:', { previous: lastStatus.override, next: value, engineReady: Boolean(engine) })
  }
  store.set('udp.protocol', value)
  if (engine) engine.setOverride(value)
}

export function getTeamColorConfig(): {
  catalog: Record<string, Array<{ id: number; name: string; color: string; group: string }>>
  overrides: TeamColorOverrides
} {
  let catalog = {}
  if (engine) {
    try { catalog = JSON.parse(engine.teamColorCatalog()) } catch {}
  }
  return { catalog, overrides: storedTeamColorOverrides() }
}

export function setTeamColorOverrides(value: unknown): void {
  const source = value && typeof value === 'object' && !Array.isArray(value)
    ? value as Record<string, unknown>
    : {}
  const normalized: TeamColorOverrides = {}
  for (const format of ['2024', '2025', '2026']) {
    const teams = source[format]
    if (!teams || typeof teams !== 'object' || Array.isArray(teams)) continue
    for (const [id, color] of Object.entries(teams as Record<string, unknown>)) {
      if (/^\d+$/.test(id) && typeof color === 'string' && /^#[0-9a-f]{6}$/i.test(color)) {
        const formatOverrides = normalized[format] ?? {}
        formatOverrides[id] = color.toUpperCase()
        normalized[format] = formatOverrides
      }
    }
  }
  store.set('teamColorOverrides', normalized)
  if (engine) engine.setTeamColorOverrides(normalized)
}

export function setStrategyMinimumStops(value: number): void {
  const stops = Math.min(8, Math.max(0, Math.trunc(Number(value) || 0)))
  if (engine) engine.setStrategyMinimumStops(stops)
}

// Renderer-initiated pull: re-broadcast the last known full protocol_status so a
// window that missed the one-shot emission (or fell back to default labels) can
// recover the catalog. No-op until the engine has emitted at least one status.
export function requestStatus(): void {
  if (additionalLoggingEnabled) {
    console.info('[telemetry-diagnostics][main] renderer requested protocol status:', { cached: Boolean(lastStatusRow), protocol: lastStatus })
  }
  if (lastStatusRow) broadcast(lastStatusRow)
}

// Renderer visibility gate: pause IPC while the window is hidden/minimized/
// occluded. Main retains one bounded chart window (not an IPC queue), sends it
// as a single resume payload on return, and refreshes the protocol catalog.
export function setRendererVisible(visible: boolean): void {
  const wasVisible = rendererVisible
  if (additionalLoggingEnabled && visible !== wasVisible) {
    console.info('[telemetry-diagnostics][main] renderer visibility changed:', { previous: wasVisible, next: visible })
  }
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
