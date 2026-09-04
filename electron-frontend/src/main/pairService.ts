import { randomBytes, randomInt, timingSafeEqual } from 'crypto'
import { networkInterfaces, hostname } from 'os'
import { WebSocket, WebSocketServer, type RawData } from 'ws'
import { configStore as store } from './configStore'
import { filterBinaryRows } from './binaryRows'

const DEFAULT_PORT = 20779
const PAIR_PROTOCOL_VERSION = 1
const BINARY_ROWS_VERSION = 2
const PAIR_WINDOW_MS = 2 * 60_000
const MAX_CLIENT_BUFFERED_BYTES = 8 * 1024 * 1024
const RACE_DASHBOARD_MASK = (1 << 1) | (1 << 2) | (1 << 4) | (1 << 5)
const ROW_TYPES: Record<string, number> = {
  telemetry: 1,
  status: 2,
  lap: 4,
  session: 5,
  damage: 3,
  participants: 6,
  tyre_sets: 7,
  tyre_stints: 8,
  classification: 9,
  event: 10,
  motion: 11,
  motion_ex: 12,
  positions: 13,
}
// Control rows have no data-family bit. Keep this list deliberately narrow:
// renderer-only payloads such as playback_lap_data and playback_lap_blocks can
// contain megabytes of chart history and must never reach a paired dashboard.
const PAIR_CONTROL_TYPES = new Set([
  'protocol_status',
  'playback_state',
  'timeline_reset',
])

export interface PairDevice {
  id: string
  name: string
  token: string
  pairedAt: number
  lastSeenAt: number
}

export interface PairServiceState {
  enabled: boolean
  serverId: string
  port: number
  pairingOpen: boolean
  pairingExpiresAt: number
  matchingCode: string | null
  qrPayload: string | null
  devices: Array<Omit<PairDevice, 'token'> & { connected: boolean }>
  error: string | null
}

type NativeDiscovery = {
  startPairDiscovery(serverId: string, name: string, port: number, pairing: boolean): string | null
  updatePairDiscovery(serverId: string, name: string, port: number, pairing: boolean): void
  stopPairDiscovery(): void
}

type Client = {
  socket: WebSocket
  authenticated: boolean
  deviceId: string
  streamMask: number
}

let server: WebSocketServer | null = null
let discovery: NativeDiscovery | null = null
let pairingSecret: string | null = null
let matchingCode: string | null = null
let pairingExpiresAt = 0
let lastError: string | null = null
let latestProtocolStatusRow: string | null = null
let latestProtocolYear: number | null = null
let latestFormula: number | null = null
const clients = new Set<Client>()
const listeners = new Set<(state: PairServiceState) => void>()
let requirementsChanged: ((streamMask: number, historyMask: number, windowSeconds: number) => void) | null = null

function serverId(): string {
  let id = store.get('pairing.serverId', '') as string
  if (!id) {
    id = randomBytes(16).toString('hex')
    store.set('pairing.serverId', id)
  }
  return id
}

function devices(): PairDevice[] {
  const value = store.get('pairing.devices', [])
  return Array.isArray(value) ? value.filter(item => item && typeof item === 'object') as PairDevice[] : []
}

function saveDevices(value: PairDevice[]): void { store.set('pairing.devices', value) }

function localAddress(): string {
  for (const entries of Object.values(networkInterfaces())) {
    for (const item of entries ?? []) {
      if (item.family === 'IPv4' && !item.internal) return item.address
    }
  }
  return '127.0.0.1'
}

function safeEqual(a: string, b: string): boolean {
  const aa = Buffer.from(a)
  const bb = Buffer.from(b)
  return aa.length === bb.length && timingSafeEqual(aa, bb)
}

function publicState(): PairServiceState {
  const connected = new Set([...clients].filter(c => c.authenticated).map(c => c.deviceId))
  const isOpen = pairingExpiresAt > Date.now()
  return {
    enabled: server !== null,
    serverId: serverId(),
    port: DEFAULT_PORT,
    pairingOpen: isOpen,
    pairingExpiresAt: isOpen ? pairingExpiresAt : 0,
    matchingCode: isOpen ? matchingCode : null,
    qrPayload: isOpen && pairingSecret
      ? `tnrpair://v1/${serverId()}?h=${encodeURIComponent(localAddress())}&p=${DEFAULT_PORT}&s=${encodeURIComponent(pairingSecret)}&e=${pairingExpiresAt}`
      : null,
    devices: devices().map(({ token: _token, ...device }) => ({ ...device, connected: connected.has(device.id) })),
    error: lastError,
  }
}

function emitState(): void {
  const state = publicState()
  for (const listener of listeners) listener(state)
}

function updateDiscovery(): void {
  discovery?.updatePairDiscovery(serverId(), hostname() || 'Track N Race', DEFAULT_PORT,
    pairingExpiresAt > Date.now())
}

function aggregateRequirements(): void {
  let mask = 0
  for (const client of clients) if (client.authenticated) mask |= client.streamMask
  requirementsChanged?.(mask >>> 0, 0, 0)
}

function sendJson(socket: WebSocket, value: unknown): void {
  if (socket.readyState === WebSocket.OPEN) socket.send(JSON.stringify(value))
}

function authenticate(client: Client, message: Record<string, unknown>): void {
  if (Number(message.pairProtocol) !== PAIR_PROTOCOL_VERSION) {
    sendJson(client.socket, { type: 'error', code: 'unsupported_pair_protocol' })
    return
  }
  if (Number(message.binaryRowsVersion) !== BINARY_ROWS_VERSION) {
    sendJson(client.socket, { type: 'error', code: 'unsupported_binary_rows' })
    return
  }
  const deviceId = typeof message.deviceId === 'string' ? message.deviceId.slice(0, 128) : ''
  const name = typeof message.name === 'string' ? message.name.slice(0, 96) : 'Android device'
  if (!deviceId) return sendJson(client.socket, { type: 'error', code: 'invalid_device' })

  let known = devices()
  const saved = known.find(device => device.id === deviceId)
  const token = typeof message.token === 'string' ? message.token : ''
  const secret = typeof message.secret === 'string' ? message.secret : ''
  const code = typeof message.code === 'string' ? message.code : ''
  const resume = !!saved && !!token && safeEqual(saved.token, token)
  const inWindow = pairingExpiresAt > Date.now()
  const initialPair = inWindow && ((pairingSecret && safeEqual(pairingSecret, secret)) ||
    (matchingCode && safeEqual(matchingCode, code)))
  if (!resume && !initialPair) {
    sendJson(client.socket, { type: 'error', code: inWindow ? 'invalid_pairing_code' : 'pairing_closed' })
    return
  }

  const credential = resume ? saved!.token : randomBytes(32).toString('base64url')
  const now = Date.now()
  const next: PairDevice = { id: deviceId, name, token: credential, pairedAt: saved?.pairedAt ?? now, lastSeenAt: now }
  known = [...known.filter(device => device.id !== deviceId), next]
  saveDevices(known)
  client.authenticated = true
  client.deviceId = deviceId
  client.streamMask = RACE_DASHBOARD_MASK
  if (initialPair) closePairingWindow()
  sendJson(client.socket, {
    type: 'welcome', pairProtocol: PAIR_PROTOCOL_VERSION,
    binaryRowsVersion: BINARY_ROWS_VERSION, serverId: serverId(),
    token: credential, source: 'desktop',
    protocolYear: latestProtocolYear, formula: latestFormula,
    capabilities: ['subscribe', 'latest-state', 'playback-state']
  })
  if (latestProtocolStatusRow) {
    sendJson(client.socket, { type: 'rows', rows: [latestProtocolStatusRow] })
  }
  aggregateRequirements()
  emitState()
}

function handleMessage(client: Client, raw: RawData): void {
  if (typeof raw !== 'string' && !Buffer.isBuffer(raw)) return
  let message: Record<string, unknown>
  try { message = JSON.parse(raw.toString()) as Record<string, unknown> } catch { return }
  if (!client.authenticated) {
    if (message.type === 'pair' || message.type === 'resume') authenticate(client, message)
    return
  }
  if (message.type === 'subscribe') {
    // The only Android page today is the race dashboard. It has no history
    // dependency and cannot broaden its subscription beyond its exact mask.
    client.streamMask = (Number(message.streamMask) >>> 0) & RACE_DASHBOARD_MASK
    aggregateRequirements()
    sendJson(client.socket, { type: 'subscribed', streamMask: client.streamMask, historyMask: 0, backfill: 'none' })
  } else if (message.type === 'ping') {
    sendJson(client.socket, { type: 'pong', at: Date.now() })
  }
}

export function configurePairService(nativeDiscovery: NativeDiscovery,
  onRequirementsChanged: (streamMask: number, historyMask: number, windowSeconds: number) => void): void {
  discovery = nativeDiscovery
  requirementsChanged = onRequirementsChanged
  if (store.get('pairing.enabled', false) as boolean) startPairService()
}

export function startPairService(): PairServiceState {
  if (server) return publicState()
  lastError = null
  try {
    server = new WebSocketServer({ host: '0.0.0.0', port: DEFAULT_PORT, maxPayload: 1024 * 1024 })
    server.on('connection', socket => {
      const client: Client = { socket, authenticated: false, deviceId: '', streamMask: 0 }
      clients.add(client)
      socket.on('message', data => handleMessage(client, data))
      socket.on('close', () => { clients.delete(client); aggregateRequirements(); emitState() })
      socket.on('error', () => {})
    })
    server.on('error', error => { lastError = error.message; emitState() })
    const error = discovery?.startPairDiscovery(serverId(), hostname() || 'Track N Race', DEFAULT_PORT, false)
    if (error) lastError = error
    store.set('pairing.enabled', true)
  } catch (error) {
    lastError = error instanceof Error ? error.message : String(error)
    server = null
  }
  emitState()
  return publicState()
}

export function stopPairService(persistDisabled = true): PairServiceState {
  closePairingWindow()
  for (const client of clients) client.socket.close(1001, 'Paired mode disabled')
  clients.clear()
  server?.close()
  server = null
  discovery?.stopPairDiscovery()
  if (persistDisabled) store.set('pairing.enabled', false)
  aggregateRequirements()
  emitState()
  return publicState()
}

export function openPairingWindow(): PairServiceState {
  if (!server) startPairService()
  pairingSecret = randomBytes(24).toString('base64url')
  matchingCode = randomInt(0, 1_000_000).toString().padStart(6, '0')
  pairingExpiresAt = Date.now() + PAIR_WINDOW_MS
  updateDiscovery()
  emitState()
  return publicState()
}

export function closePairingWindow(): PairServiceState {
  pairingSecret = null
  matchingCode = null
  pairingExpiresAt = 0
  updateDiscovery()
  emitState()
  return publicState()
}

export function removePairDevice(id: string): PairServiceState {
  saveDevices(devices().filter(device => device.id !== id))
  for (const client of clients) {
    if (client.deviceId === id) client.socket.close(4001, 'Pairing revoked')
  }
  emitState()
  return publicState()
}

export function getPairServiceState(): PairServiceState { return publicState() }
export function onPairServiceState(listener: (state: PairServiceState) => void): () => void {
  listeners.add(listener)
  return () => listeners.delete(listener)
}

export function publishPairJsonBatch(batch: string): void {
  const rows = batch.split('\n').filter(Boolean)
  for (const row of rows) {
    if (!row.includes('"type":"protocol_status"')) continue
    try {
      const status = JSON.parse(row) as Record<string, unknown>
      if (status.type === 'protocol_status') {
        latestProtocolStatusRow = row
        const activeFormat = Number(status.active_format)
        const detectedFormat = Number(status.detected_format)
        latestProtocolYear = Number.isInteger(activeFormat) && activeFormat > 0
          ? activeFormat
          : Number.isInteger(detectedFormat) && detectedFormat > 0
            ? detectedFormat
            : null
        const formula = status.formula == null ? null : Number(status.formula)
        latestFormula = Number.isInteger(formula) ? formula : null
      }
    } catch {}
  }
  if (clients.size === 0) return

  // Parse each engine row once. With multiple phones connected, reparsing the
  // same row inside every peer's filter needlessly multiplies main-process work.
  const typedRows = rows.map(row => {
    try {
      const value = JSON.parse(row) as Record<string, unknown>
      return { row, type: typeof value.type === 'string' ? value.type : '', value }
    } catch {
      return null
    }
  }).filter((entry): entry is { row: string; type: string; value: Record<string, unknown> } => entry !== null)

  for (const client of clients) {
    if (!client.authenticated || client.socket.readyState !== WebSocket.OPEN) continue
    const filtered = typedRows.filter(({ type }) => {
      const rowType = ROW_TYPES[type]
      return rowType === undefined
        ? PAIR_CONTROL_TYPES.has(type)
        : (client.streamMask & (1 << rowType)) !== 0
    }).map(({ row }) => row)
    if (filtered.length) sendJson(client.socket, { type: 'rows', rows: filtered })
  }
}

export function publishPairBinary(batch: Uint8Array): void {
  if (clients.size === 0) return
  const source = Buffer.from(batch)
  for (const client of clients) {
    if (!client.authenticated || client.socket.readyState !== WebSocket.OPEN) continue
    const filtered = filterBinaryRows(source, client.streamMask)
    if (!filtered.length) continue
    if (client.socket.bufferedAmount + filtered.length > MAX_CLIENT_BUFFERED_BYTES) {
      client.socket.close(1013, 'Telemetry client cannot keep up')
      continue
    }
    client.socket.send(filtered, { binary: true })
  }
}

/** Publish one latest-value snapshot after a desktop playback seek. */
export function publishPairSnapshot(batch: Uint8Array, coldJson: string): void {
  const source = Buffer.from(batch)
  const latestByTag = new Map<number, Buffer>()
  let offset = 0
  while (offset < source.length) {
    const tag = source[offset]
    const length = tag === 1 ? 46 : tag === 2 ? 29 : tag === 4 ? 21
      : tag === 3 && offset + 3 <= source.length ? 3 + source[offset + 2] * 16 : -1
    if (length < 0 || offset + length > source.length) break
    latestByTag.set(tag, Buffer.from(source.subarray(offset, offset + length)))
    offset += length
  }
  if (latestByTag.size) publishPairBinary(Buffer.concat([...latestByTag.values()]))

  const latestByType = new Map<string, string>()
  for (const row of coldJson.split('\n')) {
    if (!row) continue
    try {
      const type = JSON.parse(row).type
      if (typeof type === 'string') latestByType.set(type, row)
    } catch {}
  }
  if (latestByType.size) publishPairJsonBatch([...latestByType.values()].join('\n'))
}
