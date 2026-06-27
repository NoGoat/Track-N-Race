import { app, BrowserWindow } from 'electron'
import Store from 'electron-store'
import * as path from 'path'

const store = new Store()

type ProtocolOverride = 'auto' | 'f1_24' | 'f1_25'

let lastStatus: { override: ProtocolOverride; detected: number | null; active: number | null } = {
  override: (store.get('udp.protocol', 'auto') as ProtocolOverride),
  detected: null,
  active: null,
}

let engine: any = null
let unsubLogging: Array<() => void> = []

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
      // The addon coalesces rows into a newline-delimited JSON blob.
      let start = 0
      while (start < batch.length) {
        let end = batch.indexOf('\n', start)
        if (end === -1) end = batch.length
        if (end > start) handleRow(JSON.parse(batch.slice(start, end)))
        start = end + 1
      }
    })
    
    engine.startUdp()
    pushLogging()
    
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
