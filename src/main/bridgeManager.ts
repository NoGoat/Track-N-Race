/**
 * bridgeManager.ts
 *
 * Owns the native `protocol_parser` bridge process. The bridge binds UDP 20777,
 * parses F1 telemetry via libtnrp, records .tnrd files, and streams parsed rows
 * back to us over a named pipe (Windows) / unix domain socket (Linux). We:
 *   - create the pipe server and spawn the bridge as a child,
 *   - forward each row to the renderer over the existing 'telemetry' /
 *     'playback_state' IPC channels (so preload + React are unchanged),
 *   - translate UI commands (override / logging / udp restart / player*) into
 *     newline-JSON commands on the bridge's stdin,
 *   - persist the auto-detected protocol and keep a config snapshot for the UI.
 *
 * The bridge is started on app ready and dies when we close its stdin on quit;
 * if it crashes mid-session we respawn it.
 */

import { app, BrowserWindow } from 'electron'
import { spawn, ChildProcess } from 'child_process'
import * as net from 'net'
import * as os from 'os'
import * as path from 'path'
import * as fs from 'fs'
import Store from 'electron-store'

const store = new Store()

type ProtocolOverride = 'auto' | 'f1_24' | 'f1_25'

let child: ChildProcess | null = null
let server: net.Server | null = null
let pipePath = ''
let lineBuf = ''
let shuttingDown = false
let onPlaybackState: ((state: unknown) => void) | null = null

// Resolves the next player_load result (the bridge replies with playback_loaded).
let pendingLoad: ((ok: boolean) => void) | null = null
let currentFilename: string | null = null

// Snapshot from the latest protocol_status row, for protocol-get-config.
let lastStatus: { override: ProtocolOverride; detected: number | null; active: number | null } = {
  override: (store.get('udp.protocol', 'auto') as ProtocolOverride),
  detected: null,
  active: null,
}

function broadcast(row: Record<string, unknown>): void {
  for (const win of BrowserWindow.getAllWindows()) {
    if (!win.isDestroyed()) win.webContents.send('telemetry', row)
  }
}

function resolveBinaryPath(): string {
  const exe = process.platform === 'win32' ? 'protocol_parser.exe' : 'protocol_parser'
  if (app.isPackaged) return path.join(path.dirname(process.execPath), exe)
  const devDir = process.platform === 'win32' ? 'Release' : '.'
  return path.join(app.getAppPath(), 'protocol_parser/build', devDir, exe)
}

function makePipePath(): string {
  const id = `tnrp-${process.pid}-${Date.now()}`
  if (process.platform === 'win32') return `\\\\.\\pipe\\${id}`
  return path.join(os.tmpdir(), `${id}.sock`)
}

function sendCommand(cmd: Record<string, unknown>): void {
  if (child && child.stdin && child.stdin.writable) {
    child.stdin.write(JSON.stringify(cmd) + '\n')
  }
}

// --- Row routing -------------------------------------------------------------

function handleRow(row: Record<string, unknown>): void {
  const type = row.type as string

  if (type === 'protocol_status') {
    lastStatus = {
      override: (row.override as ProtocolOverride) ?? 'auto',
      detected: (row.detected_format as number) ?? null,
      active: (row.active_format as number) ?? null,
    }
    // Persist the auto-detected format so the UI is correct before packets arrive.
    if (lastStatus.override === 'auto' && lastStatus.detected != null) {
      store.set('udp.lastDetectedProtocol', lastStatus.detected)
    }
    broadcast(row)
    return
  }

  if (type === 'playback_loaded') {
    const ok = row.ok === true
    if (pendingLoad) { pendingLoad(ok); pendingLoad = null }
    return
  }

  if (type === 'playback_state') {
    const total = (row.total_time as number) || 0
    const cur = (row.current_time as number) || 0
    const state = {
      isPlaying: row.playing === true,
      speed: (row.speed as number) ?? 1,
      progressPct: total > 0 ? cur / total : 0,
      currentTime: cur,
      totalTime: total,
      filename: currentFilename,
      isScanning: false,
    }
    if (onPlaybackState) onPlaybackState(state)
    return
  }

  if (type === 'playback_finished') return  // state already reflects the end

  // Everything else (telemetry/status/.../protocol_warning/playback_close) goes
  // to the renderer unchanged on the 'telemetry' channel.
  broadcast(row)
}

function onPipeData(chunk: Buffer): void {
  lineBuf += chunk.toString('utf8')
  let nl: number
  while ((nl = lineBuf.indexOf('\n')) >= 0) {
    const line = lineBuf.slice(0, nl)
    lineBuf = lineBuf.slice(nl + 1)
    if (!line) continue
    try {
      handleRow(JSON.parse(line))
    } catch {
      // ignore malformed line
    }
  }
}

// --- Lifecycle ---------------------------------------------------------------

export function startBridge(): void {
  if (child) return
  shuttingDown = false

  pipePath = makePipePath()
  // Clean up any stale socket file (POSIX).
  if (process.platform !== 'win32') {
    try { fs.unlinkSync(pipePath) } catch { /* not present */ }
  }

  server = net.createServer((socket) => {
    socket.on('data', onPipeData)
    socket.on('error', () => { /* bridge gone; respawn handled on child exit */ })
  })
  server.on('error', (err) => console.error('[bridge] pipe server error:', err))

  server.listen(pipePath, () => {
    const bin = resolveBinaryPath()
    if (!fs.existsSync(bin)) {
      console.error(`[bridge] binary not found at ${bin} — build protocol_parser first`)
      return
    }
    const args = [
      '--pipe', pipePath,
      '--port', String(store.get('udp.port', 20777)),
      '--bind', String(store.get('udp.bindAddress', '0.0.0.0')),
      '--protocol', String(store.get('udp.protocol', 'auto')),
      '--log-dir', String(store.get('logging.directory', '')),
    ]
    if (store.get('logging.enabled', false)) args.push('--log-enabled')

    child = spawn(bin, args, { stdio: ['pipe', 'ignore', 'pipe'] })
    child.stderr?.on('data', (d) => process.stderr.write(`[bridge] ${d}`))
    child.on('exit', (code) => {
      child = null
      if (!shuttingDown) {
        console.error(`[bridge] exited (code ${code}); respawning in 1s`)
        setTimeout(() => { if (!shuttingDown) restart() }, 1000)
      }
    })
  })

  // Push logging settings changes through without a respawn.
  store.onDidChange('logging.enabled', () => pushLogging())
  store.onDidChange('logging.directory', () => pushLogging())
}

function pushLogging(): void {
  sendCommand({
    cmd: 'set_logging',
    enabled: store.get('logging.enabled', false),
    dir: store.get('logging.directory', ''),
  })
}

function teardownServer(): void {
  if (server) { server.close(); server = null }
  if (process.platform !== 'win32' && pipePath) {
    try { fs.unlinkSync(pipePath) } catch { /* ignore */ }
  }
  lineBuf = ''
}

function restart(): void {
  teardownServer()
  startBridge()
}

export function stopBridge(): void {
  shuttingDown = true
  if (child) {
    child.stdin?.end()         // EOF -> bridge shuts itself down cleanly
    child.kill()
    child = null
  }
  teardownServer()
}

// --- Commands used by index.ts IPC handlers ----------------------------------

export function setOnPlaybackState(cb: (state: unknown) => void): void {
  onPlaybackState = cb
}

export function setOverride(value: ProtocolOverride): void {
  store.set('udp.protocol', value)
  sendCommand({ cmd: 'set_override', value })
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
  sendCommand({
    cmd: 'restart_udp',
    port: store.get('udp.port', 20777),
    bind: store.get('udp.bindAddress', '0.0.0.0'),
  })
}

export function playerLoad(filePath: string): Promise<boolean> {
  currentFilename = filePath.split(/[\\/]/).pop() ?? null
  return new Promise<boolean>((resolve) => {
    pendingLoad = resolve
    sendCommand({ cmd: 'player_load', path: filePath })
    // Safety timeout so a missing reply never hangs the renderer invoke.
    setTimeout(() => { if (pendingLoad === resolve) { pendingLoad = null; resolve(false) } }, 15000)
  })
}

export function playerPlay():  void { sendCommand({ cmd: 'player_play' }) }
export function playerPause(): void { sendCommand({ cmd: 'player_pause' }) }
export function playerSeek(pct: number):   void { sendCommand({ cmd: 'player_seek', pct }) }
export function playerSetSpeed(mult: number): void { sendCommand({ cmd: 'player_set_speed', mult }) }
export function playerGetLapData(lapNum: number): void { sendCommand({ cmd: 'player_get_lap_data', lap_num: lapNum }) }
export function playerClose(): void {
  currentFilename = null
  sendCommand({ cmd: 'player_close' })
}
