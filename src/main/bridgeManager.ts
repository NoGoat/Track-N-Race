import { app, BrowserWindow } from 'electron'
import Store from 'electron-store'
import * as path from 'path'
import { HotRowSmoother } from './binaryForwardFill'

const store = new Store()

type ProtocolOverride = 'auto' | 'f1_24' | 'f1_25'

let lastStatus: { override: ProtocolOverride; detected: number | null; active: number | null } = {
  override: (store.get('udp.protocol', 'auto') as ProtocolOverride),
  detected: null,
  active: null,
}

let engine: any = null
let unsubLogging: Array<() => void> = []

// The game streams 3 hot packet types at 60 Hz (motion, car_tel, motion_ex), so
// the addon can hand us ~180 binary batches/sec. Forwarding each as its own IPC
// message makes the renderer do up to 180 decode+render passes/sec and fall behind
// (visible as bursty "every few seconds" updates). Coalesce to one frame-aligned
// IPC message per ~16 ms so the renderer does at most ~60 updates/sec.
const TICK_MS = 16
let binPending: Uint8Array[] = []
let binFlushTimer: NodeJS.Timeout | null = null
const smoother = new HotRowSmoother(TICK_MS)

function flushBinary(): void {
  const batch = smoother.tick(binPending)
  binPending = []
  if (!batch) return
  for (const win of BrowserWindow.getAllWindows()) {
    if (!win.isDestroyed()) win.webContents.send('telemetry-binary', batch)
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
    if (lastStatus.override === 'auto' && lastStatus.detected != null) {
      store.set('udp.lastDetectedProtocol', lastStatus.detected)
    }
    broadcast(row)
    return
  }

  broadcast(row)
}

function loadAddon(): any {
  // Try to load the N-API module
  // Electron's require correctly handles ASAR unpacking for .node files automatically.
  let p = path.join(app.getAppPath(), 'node_addon', 'build', 'Release', 'protocol_parser.node')
  return require(p)
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
      for (const win of BrowserWindow.getAllWindows()) {
        if (!win.isDestroyed()) win.webContents.send('telemetry-batch', batch)
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
    if (!binFlushTimer) binFlushTimer = setInterval(flushBinary, 16)
    
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
  if (binFlushTimer) { clearInterval(binFlushTimer); binFlushTimer = null }
  binPending = []
  smoother.reset()
  if (engine) {
    engine.playerClose()
    engine.destroy()
    engine = null
  }
}

export function setOverride(value: ProtocolOverride): void {
  store.set('udp.protocol', value)
  if (engine) engine.setOverride(value)
}

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
