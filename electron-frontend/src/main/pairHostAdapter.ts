import { safeStorage } from 'electron'
import { hostname } from 'os'
import { configStore as store } from './configStore'

const DEFAULT_PORT = 20779
// Prefix of an engine state document encrypted with safeStorage.
const ENCRYPTED_PREFIX = 'safe:v1:'

export interface PairDevice {
  id: string
  name: string
  pairedAt: number
  lastSeenAt: number
  connected: boolean
}

export interface PairServiceState {
  enabled: boolean
  serverId: string
  port: number
  pairingOpen: boolean
  pairingExpiresAt: number
  matchingCode: string | null
  qrPayload: string | null
  pendingDevice: { id: string; name: string } | null
  devices: PairDevice[]
  error: string | null
}

// Deliberately contains only host controls and JSON state exchange. WebSocket,
// authentication, discovery, filtering, caching and backpressure are owned by
// tnrp::PairServer inside the shared C++ engine.
export interface NativePairEngine {
  pairStart(): string | null
  pairStop(persistDisabled?: boolean): string
  pairOpenWindow(): string
  pairCloseWindow(): string
  pairRespond(approve: boolean): string
  pairRemoveDevice(id: string): string
  pairGetState(): string
}

const listeners = new Set<(state: PairServiceState) => void>()
let engine: NativePairEngine | null = null
let state: PairServiceState = emptyState()
// Set when a stored encrypted document could not be decrypted (the OS key
// store was unavailable). The engine then starts from a fresh identity, and
// that must not overwrite the saved pairings.
let keepStoredState = false

function emptyState(error: string | null = null): PairServiceState {
  return {
    enabled: false,
    serverId: '',
    port: DEFAULT_PORT,
    pairingOpen: false,
    pairingExpiresAt: 0,
    matchingCode: null,
    qrPayload: null,
    pendingDevice: null,
    devices: [],
    error,
  }
}

function parsePendingDevice(value: unknown): PairServiceState['pendingDevice'] {
  if (!value || typeof value !== 'object') return null
  const { id, name } = value as { id?: unknown; name?: unknown }
  return typeof id === 'string' && typeof name === 'string' ? { id, name } : null
}

function parseState(json: string): PairServiceState {
  try {
    const value = JSON.parse(json) as Partial<PairServiceState>
    return {
      enabled: value.enabled === true,
      serverId: typeof value.serverId === 'string' ? value.serverId : '',
      port: typeof value.port === 'number' && Number.isInteger(value.port)
        ? value.port : DEFAULT_PORT,
      pairingOpen: value.pairingOpen === true,
      pairingExpiresAt: typeof value.pairingExpiresAt === 'number' &&
        Number.isFinite(value.pairingExpiresAt) ? value.pairingExpiresAt : 0,
      matchingCode: typeof value.matchingCode === 'string' ? value.matchingCode : null,
      qrPayload: typeof value.qrPayload === 'string' ? value.qrPayload : null,
      pendingDevice: parsePendingDevice(value.pendingDevice),
      devices: Array.isArray(value.devices) ? value.devices as PairDevice[] : [],
      error: typeof value.error === 'string' ? value.error : null,
    }
  } catch {
    return emptyState('The native paired-display state was invalid.')
  }
}

function publish(next: PairServiceState): PairServiceState {
  state = next
  for (const listener of listeners) listener(state)
  return state
}

function stateFromEngine(): PairServiceState {
  if (!engine) return state
  return publish(parseState(engine.pairGetState()))
}

// The engine document holds the desktop's signing seed. It is stored
// encrypted with the OS key store; plain text only where none exists (Linux
// without a keyring), which matches what Electron itself would offer.
function encryptState(json: string): string {
  if (!safeStorage.isEncryptionAvailable()) return json
  return ENCRYPTED_PREFIX + safeStorage.encryptString(json).toString('base64')
}

function decryptState(stored: string): string | null {
  if (!stored.startsWith(ENCRYPTED_PREFIX)) return stored
  if (!safeStorage.isEncryptionAvailable()) return null
  try {
    return safeStorage.decryptString(Buffer.from(stored.slice(ENCRYPTED_PREFIX.length), 'base64'))
  } catch {
    return null
  }
}

/** Construction settings for tnrp::Engine. */
export function pairEngineConfig(): {
  pairEnabled: boolean
  pairPort: number
  pairName: string
  pairStateJson: string
} {
  const stored = store.get('pairing.engineState', '') as string
  let persisted = ''
  if (stored) {
    const decrypted = decryptState(stored)
    if (decrypted === null) {
      keepStoredState = true
      console.warn('[pairing] saved pairings could not be decrypted; they are kept but unused this session')
    } else {
      persisted = decrypted
    }
  }
  return {
    pairEnabled: store.get('pairing.enabled', false) as boolean,
    pairPort: DEFAULT_PORT,
    pairName: hostname() || 'Track N Race',
    pairStateJson: persisted,
  }
}

export function configurePairService(nativeEngine: NativePairEngine): void {
  engine = nativeEngine
  stateFromEngine()
}

/** Called only by the N-API state callback. */
export function receivePairServiceState(publicJson: string,
                                        persistedJson: string): void {
  if (!keepStoredState) store.set('pairing.engineState', encryptState(persistedJson))
  const next = parseState(publicJson)
  // Persist the engine's desired-enabled flag, which intentionally remains
  // true during ordinary app shutdown even though the public running flag has
  // already changed to false.
  try {
    const persisted = JSON.parse(persistedJson) as { enabled?: unknown }
    store.set('pairing.enabled', persisted.enabled === true)
  } catch {
    // Keep the last valid preference if an interrupted callback is malformed.
  }
  publish(next)
}

export function startPairService(): PairServiceState {
  if (!engine) return publish(emptyState('The telemetry engine is not available.'))
  const error = engine.pairStart()
  const next = parseState(engine.pairGetState())
  if (error && !next.error) next.error = error
  return publish(next)
}

export function stopPairService(persistDisabled = true): PairServiceState {
  if (!engine) return state
  return publish(parseState(engine.pairStop(persistDisabled)))
}

export function openPairingWindow(): PairServiceState {
  if (!engine) return publish(emptyState('The telemetry engine is not available.'))
  return publish(parseState(engine.pairOpenWindow()))
}

export function closePairingWindow(): PairServiceState {
  if (!engine) return state
  return publish(parseState(engine.pairCloseWindow()))
}

export function respondToPairing(approve: boolean): PairServiceState {
  if (!engine) return state
  return publish(parseState(engine.pairRespond(approve)))
}

export function removePairDevice(id: string): PairServiceState {
  if (!engine) return state
  return publish(parseState(engine.pairRemoveDevice(id)))
}

export function getPairServiceState(): PairServiceState {
  return engine ? parseState(engine.pairGetState()) : state
}

export function onPairServiceState(
  listener: (state: PairServiceState) => void,
): () => void {
  listeners.add(listener)
  return () => listeners.delete(listener)
}
