import { hostname } from 'os'
import { configStore as store } from './configStore'

const DEFAULT_PORT = 20779

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
  pairRemoveDevice(id: string): string
  pairGetState(): string
}

const listeners = new Set<(state: PairServiceState) => void>()
let engine: NativePairEngine | null = null
let state: PairServiceState = emptyState()

function emptyState(error: string | null = null): PairServiceState {
  return {
    enabled: false,
    serverId: '',
    port: DEFAULT_PORT,
    pairingOpen: false,
    pairingExpiresAt: 0,
    matchingCode: null,
    qrPayload: null,
    devices: [],
    error,
  }
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

/**
 * Construction settings for tnrp::Engine. The legacy fields are read only to
 * migrate existing paired devices into the engine's opaque state document.
 */
export function pairEngineConfig(): {
  pairEnabled: boolean
  pairPort: number
  pairName: string
  pairStateJson: string
} {
  let persisted = store.get('pairing.engineState', '') as string
  if (!persisted) {
    persisted = JSON.stringify({
      serverId: store.get('pairing.serverId', ''),
      enabled: store.get('pairing.enabled', false),
      devices: store.get('pairing.devices', []),
    })
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
  store.set('pairing.engineState', persistedJson)
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
