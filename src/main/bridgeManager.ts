import { app, BrowserWindow } from 'electron'
import Store from 'electron-store'
import * as path from 'path'
import { HotRowSmoother } from './binaryForwardFill'

const store = new Store()

type ProtocolOverride = 'auto' | 'f1_24' | 'f1_25' | 'f1_26'

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

let engine: any = null
let unsubLogging: Array<() => void> = []

// The game streams 3 hot packet types per frame (motion, car_tel, motion_ex), so
// the addon can hand us ~3x the frame rate in binary batches/sec. Forwarding each as
// its own IPC message makes the renderer fall behind (bursty "every few seconds"
// updates). Coalesce + forward-fill via the smoother, flushing once per measured
// frame. The flush re-reads the smoother's detected period each tick so the cadence
// tracks the actual send rate (20/40/60 Hz, or an fps-capped value).
const BOOTSTRAP_TICK_MS = 16
let binPending: Uint8Array[] = []
let binFlushTimer: NodeJS.Timeout | null = null
const smoother = new HotRowSmoother()

function flushBinary(): void {
  // Always drain the smoother (so binPending can't grow unbounded while hidden),
  // but only forward to a visible renderer.
  const batch = smoother.tick(binPending)
  binPending = []
  if (batch && rendererVisible) {
    for (const win of BrowserWindow.getAllWindows()) {
      if (!win.isDestroyed()) win.webContents.send('telemetry-binary', batch)
    }
  }
  // Re-schedule at the measured frame period. setTimeout delay is integer-ms, so the
  // wall-clock poll rounds to 16/17 ms etc.; the sub-ms precision lives in the fill's
  // session_time advance, not the timer.
  const delayMs = Math.max(1, Math.round(smoother.getPeriodMs()))
  binFlushTimer = setTimeout(flushBinary, delayMs)
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

let addonModule: any = null
function loadAddon(): any {
  // Try to load the N-API module
  // Electron's require correctly handles ASAR unpacking for .node files automatically.
  if (addonModule) return addonModule
  let p = path.join(app.getAppPath(), 'node_addon', 'build', 'Release', 'protocol_parser.node')
  addonModule = require(p)
  return addonModule
}

// The library-owned i18n catalog for a packet format (2024/2025/2026), used by
// the TS playback path to label a recorded clip without an Engine instance.
export function labelsForFormat(format: number): Record<string, string> {
  try {
    return JSON.parse(loadAddon().labelsJson(format))
  } catch {
    return {}
  }
}

// The library-owned declarative card-colour spec (format-independent).
export function cardColorSpecs(): Record<string, unknown> {
  try {
    return JSON.parse(loadAddon().cardColorsJson())
  } catch {
    return {}
  }
}

// The format the live engine is currently routing with (for restoring labels
// after playback closes). Falls back to the last detected/stored format.
export function getLiveFormat(): number {
  return lastStatus.active
      ?? lastStatus.detected
      ?? (store.get('udp.lastDetectedProtocol', 2025) as number)
}

function pushLogging(): void {
  if (engine) {
    const enabled = store.get('logging.enabled', false) as boolean
    const dir = store.get('logging.directory', '') as string
    engine.setLogging(enabled, dir)
  }
}

export function startBridge(): void {
  if (engine) return

  try {
    const addon = loadAddon()
    const config = {
      format: store.get('udp.protocol', 'auto'),
      port: store.get('udp.port', 20777),
      bindAddress: store.get('udp.bindAddress', '0.0.0.0')
    }
    
    engine = new addon.Engine(config, (batch: string) => {
      // Skip forwarding to a hidden renderer; playback delivers hot rows through
      // this channel too, so it's a high-volume path worth gating. The
      // protocol_status handling below still runs so the cache stays current and
      // is re-pushed on refocus (see setRendererVisible).
      if (rendererVisible) {
        for (const win of BrowserWindow.getAllWindows()) {
          if (!win.isDestroyed()) win.webContents.send('telemetry-batch', batch)
        }
      }

      if (batch.includes('"type":"protocol_status"')) {
        let start = 0
        while (start < batch.length) {
          let end = batch.indexOf('\n', start)
          if (end === -1) end = batch.length
          if (end > start) {
            const rowStr = batch.slice(start, end)
            if (rowStr.includes('"type":"protocol_status"')) {
              handleRow(JSON.parse(rowStr))
            }
          }
          start = end + 1
        }
      }
    }, (binBatch: Uint8Array) => {
      // Hot rows: accumulate and flush once per frame (see flushBinary).
      binPending.push(binBatch)
    })

    engine.startUdp()
    pushLogging()
    if (!binFlushTimer) binFlushTimer = setTimeout(flushBinary, BOOTSTRAP_TICK_MS)
    
    // Listen for logging changes
    unsubLogging = [
      store.onDidChange('logging.enabled', () => pushLogging()),
      store.onDidChange('logging.directory', () => pushLogging()),
    ]
    
  } catch (err) {
    console.error('[bridge] Failed to load N-API addon:', err)
  }
}

export function stopBridge(): void {
  for (const unsub of unsubLogging) unsub()
  unsubLogging = []
  if (binFlushTimer) { clearTimeout(binFlushTimer); binFlushTimer = null }
  binPending = []
  smoother.reset()
  if (engine) {
    engine.playerClose()
    engine.destroy()
    engine = null
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

// Renderer visibility gate: pause the hot telemetry channels while the window is
// hidden/minimized/occluded so a background-throttled renderer doesn't buffer a
// backlog that janks on refocus. On becoming visible again, re-push the cached
// protocol_status so labels/colours are current even if the format changed while
// paused (the hot rows resume on their own from the next packet).
export function setRendererVisible(visible: boolean): void {
  const wasVisible = rendererVisible
  rendererVisible = visible
  if (visible && !wasVisible && lastStatusRow) broadcast(lastStatusRow)
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

export function restartUdp(): void {
  // To restart UDP on port changes, we stop and recreate the engine
  stopBridge()
  startBridge()
}
