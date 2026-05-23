/**
 * protocolDispatcher.ts
 *
 * Central routing layer that sits between the UDP receiver and the
 * per-version packet parsers.  It:
 *   - Reads m_packetFormat (bytes 0-1 LE) from every incoming UDP buffer
 *   - Debounces format changes (3 consecutive same-format packets)
 *   - Persists the last auto-detected format across restarts
 *   - Applies the user's manual override (auto / f1_24 / f1_25)
 *   - Routes packets to the correct versioned PARSERS map
 *   - Emits protocol-status and protocol-mismatch IPC events
 */

import Store from 'electron-store'
import { BrowserWindow } from 'electron'
import { HEADER_SIZE, parseHeader } from './packetHeader'
import { RateLimiter } from './rateLimiter'
import {
  PARSERS as F1_25_PARSERS,
  SLOW_RATE_MS as F1_25_SLOW_RATE,
  FRAME_SAMPLED as F1_25_FRAME_SAMPLED,
} from './protocols/f1_25/packetParsers'
import {
  PARSERS as F1_24_PARSERS,
  SLOW_RATE_MS as F1_24_SLOW_RATE,
  FRAME_SAMPLED as F1_24_FRAME_SAMPLED,
} from './protocols/f1_24/packetParsers'

// -----------------------------------------------------------------------
// Types
// -----------------------------------------------------------------------

export type ProtocolOverride = 'auto' | 'f1_24' | 'f1_25'
export type GameFormat = 2024 | 2025

export interface ProtocolCapabilities {
  gameYear:        24 | 25 | null  // null = no packets received yet
  hasBlisters:     boolean
  hasLiveryColors: boolean
  hasLapPositions: boolean
}

// -----------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------

const DEBOUNCE_COUNT = 3     // consecutive same-format packets before switching
const SAMPLE_EVERY = parseInt(process.env.SAMPLE_EVERY_N_FRAMES ?? '1', 10)

// -----------------------------------------------------------------------
// State
// -----------------------------------------------------------------------

const store = new Store()

let override: ProtocolOverride = (store.get('udp.protocol', 'auto') as ProtocolOverride)
let detectedFormat: GameFormat | null = null
let debounceCandidate: GameFormat | null = null
let debounceCount = 0

/** The format that packets are actually being routed with right now. */
let activeFormat: GameFormat | null = null

let rateLimiter = new RateLimiter(SAMPLE_EVERY)

// Initialise activeFormat from persisted state so UI is correct on startup
// even before any packets arrive.
function initActiveFormat(): void {
  if (override === 'f1_25') {
    activeFormat = 2025
  } else if (override === 'f1_24') {
    activeFormat = 2024
  } else {
    // auto — restore last detected
    const last = store.get('udp.lastDetectedProtocol', null) as GameFormat | null
    activeFormat = last
  }
}

initActiveFormat()

// -----------------------------------------------------------------------
// Capability helper
// -----------------------------------------------------------------------

function getCapabilities(format: GameFormat | null): ProtocolCapabilities {
  if (format === null) {
    return { gameYear: null, hasBlisters: false, hasLiveryColors: false, hasLapPositions: false }
  }
  return {
    gameYear:        format === 2025 ? 25 : 24,
    hasBlisters:     format === 2025,
    hasLiveryColors: format === 2025,
    hasLapPositions: format === 2025,
  }
}

// -----------------------------------------------------------------------
// IPC broadcast helpers
// -----------------------------------------------------------------------

function broadcastToWindows(row: Record<string, unknown>): void {
  for (const win of BrowserWindow.getAllWindows()) {
    if (!win.isDestroyed()) win.webContents.send('telemetry', row)
  }
}

function emitProtocolStatus(): void {
  broadcastToWindows({
    type:            'protocol_status',
    detected_format: detectedFormat,
    active_format:   activeFormat,
    override,
    capabilities:    getCapabilities(activeFormat),
  })
}

function emitProtocolMismatch(incomingFormat: GameFormat, forcedFormat: GameFormat): void {
  broadcastToWindows({
    type:            'protocol_warning',
    detected_format: incomingFormat,
    forced_format:   forcedFormat,
  })
}

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------

/** Called by udpReceiver for every incoming UDP message. */
export function dispatchPacket(msg: Buffer): void {
  if (msg.length < HEADER_SIZE) return

  // Read the packet format from bytes 0-1
  const incomingFormat = msg.readUInt16LE(0) as GameFormat
  const isKnownFormat = incomingFormat === 2024 || incomingFormat === 2025

  if (!isKnownFormat) {
    console.warn(`[Protocol] Unknown packet format: ${incomingFormat} — dropping packet`)
    return
  }

  // ------- Debounce detection -------
  if (incomingFormat !== debounceCandidate) {
    debounceCandidate = incomingFormat
    debounceCount = 1
  } else {
    debounceCount++
  }

  if (debounceCount >= DEBOUNCE_COUNT && incomingFormat !== detectedFormat) {
    const prev = detectedFormat
    detectedFormat = incomingFormat

    // Persist in auto mode
    if (override === 'auto') {
      store.set('udp.lastDetectedProtocol', incomingFormat)
      const prevActive = activeFormat
      activeFormat = incomingFormat
      if (prevActive !== activeFormat) {
        rateLimiter = new RateLimiter(SAMPLE_EVERY)  // reset rate limiter on switch
      }
    }

    if (prev !== detectedFormat) {
      emitProtocolStatus()
    }
  }

  // ------- Determine effective format -------
  let effectiveFormat: GameFormat
  if (override === 'f1_25') {
    effectiveFormat = 2025
  } else if (override === 'f1_24') {
    effectiveFormat = 2024
  } else {
    // auto — use what we've detected; fall back to last persisted
    effectiveFormat = detectedFormat ?? activeFormat ?? 2025
  }

  // ------- Mismatch warning -------
  if (override !== 'auto' && isKnownFormat && incomingFormat !== effectiveFormat) {
    emitProtocolMismatch(incomingFormat, effectiveFormat)
  }

  // ------- Select parsers and rate-limiter config -------
  const parsers    = effectiveFormat === 2025 ? F1_25_PARSERS    : F1_24_PARSERS
  const slowRates  = effectiveFormat === 2025 ? F1_25_SLOW_RATE  : F1_24_SLOW_RATE
  const frameSampled = effectiveFormat === 2025 ? F1_25_FRAME_SAMPLED : F1_24_FRAME_SAMPLED

  // ------- Parse header -------
  const hdr = parseHeader(msg)
  const packetParsers = parsers[hdr.packetId]
  if (!packetParsers) return

  // ------- Rate limiting -------
  // Frame-sampled packets: throttle by overallFrameId
  // Slow-rate packets: throttle by wall-clock
  if (frameSampled.has(hdr.packetId)) {
    if (!rateLimiter.allow(hdr.packetId, hdr.overallFrameId)) return
  } else if (hdr.packetId in slowRates) {
    const minMs = slowRates[hdr.packetId]
    if (minMs > 0 && !rateLimiter.allow(hdr.packetId, hdr.overallFrameId)) return
  }

  // ------- Dispatch to parsers -------
  for (const parser of packetParsers) {
    for (const row of parser(msg, hdr)) {
      broadcastToWindows(row)
    }
  }
}

/** Called from IPC handler when the user changes the protocol override. */
export function setOverride(newOverride: ProtocolOverride): void {
  override = newOverride
  store.set('udp.protocol', newOverride)

  // Update activeFormat immediately
  if (newOverride === 'f1_25') {
    activeFormat = 2025
  } else if (newOverride === 'f1_24') {
    activeFormat = 2024
  } else {
    // auto — restore from detected or last persisted
    activeFormat = detectedFormat ?? (store.get('udp.lastDetectedProtocol', null) as GameFormat | null)
  }

  rateLimiter = new RateLimiter(SAMPLE_EVERY)  // reset on override change
  emitProtocolStatus()
}

/** Returns current protocol capabilities for the active format. */
export function getCapabilitiesSnapshot(): ProtocolCapabilities {
  return getCapabilities(activeFormat)
}

/** Returns current config for IPC query. */
export function getProtocolConfig(): { override: ProtocolOverride; detected: GameFormat | null; lastDetected: GameFormat | null; active: GameFormat | null } {
  return {
    override,
    detected:     detectedFormat,
    lastDetected: store.get('udp.lastDetectedProtocol', null) as GameFormat | null,
    active:       activeFormat,
  }
}
