import { safeStorage } from 'electron'
import { hostname } from 'os'
import { configStore as store } from './configStore'

const DEFAULT_PORT = 20779
// Prefix of an engine state document encrypted with safeStorage.
const ENCRYPTED_PREFIX = 'safe:v1:'

// macOS builds are ad-hoc signed, so the keychain cannot tell a new build
// from a stranger and asks for the login password before safeStorage can read
// "track-n-race Safe Storage". Pairing is its only user, so on macOS it stays
// off and never touches the keychain unless the user opts in.
const KEYCHAIN_OPT_IN_KEY = 'pairing.macKeychainOptIn'
const keychainGated = process.platform === 'darwin'
// Fixed for the session: the engine is built once, with or without the saved
// identity, so a changed opt-in applies after a relaunch.
const keychainOptedIn = !keychainGated || store.get(KEYCHAIN_OPT_IN_KEY, false) === true
// Set when the user denies the keychain password prompt. Pairing then stays
// off for the session so the prompt is not raised again.
let keychainDenied = false

function pairingAvailable(): boolean {
  return keychainOptedIn && !keychainDenied
}

export interface PairAvailability {
  // The platform needs the keychain opt-in before pairing can run.
  keychainGated: boolean
  // The stored opt-in, which may differ from `available` until a relaunch.
  keychainOptIn: boolean
  // Pairing can run in this session.
  available: boolean
  // The user denied the macOS keychain prompt this session.
  keychainDenied: boolean
}

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
// macOS never falls back to plain text: there the key store exists and a
// failure means access was refused. Returns null when nothing may be written.
function encryptState(json: string): string | null {
  try {
    if (!safeStorage.isEncryptionAvailable()) return keychainGated ? null : json
    return ENCRYPTED_PREFIX + safeStorage.encryptString(json).toString('base64')
  } catch {
    return null
  }
}

// Raises the macOS keychain password prompt at most once per session, before
// the engine is built, and reports whether the user allowed access.
function keychainAccessible(): boolean {
  try {
    if (!safeStorage.isEncryptionAvailable()) return false
    return safeStorage.decryptString(safeStorage.encryptString('track-n-race')) === 'track-n-race'
  } catch {
    return false
  }
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

export function getPairAvailability(): PairAvailability {
  return {
    keychainGated,
    keychainOptIn: store.get(KEYCHAIN_OPT_IN_KEY, false) === true,
    available: pairingAvailable(),
    keychainDenied,
  }
}

export function setKeychainOptIn(optIn: boolean): PairAvailability {
  if (keychainGated) store.set(KEYCHAIN_OPT_IN_KEY, optIn)
  return getPairAvailability()
}

/** Construction settings for tnrp::Engine. */
export function pairEngineConfig(): {
  pairEnabled: boolean
  pairPort: number
  pairName: string
  pairStateJson: string
} {
  if (keychainOptedIn && keychainGated && !keychainDenied && !keychainAccessible()) {
    keychainDenied = true
    console.warn('[pairing] macOS keychain access was denied; paired mode is off this session')
  }
  if (!pairingAvailable()) {
    // Leave the saved identity, pairings and enabled preference untouched for
    // a later opt-in; the engine runs this session with pairing off.
    keepStoredState = true
    return { pairEnabled: false, pairPort: DEFAULT_PORT, pairName: hostname() || 'Track N Race', pairStateJson: '' }
  }
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
  if (!pairingAvailable()) {
    publish(parseState(publicJson))
    return
  }
  const encrypted = keepStoredState ? null : encryptState(persistedJson)
  if (encrypted !== null) store.set('pairing.engineState', encrypted)
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
  if (!pairingAvailable()) {
    return publish({ ...state, error: keychainDenied
      ? 'macOS keychain access denied.'
      : 'Paired mode is disabled on macOS until you allow keychain access.' })
  }
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
