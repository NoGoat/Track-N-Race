import * as fs from 'fs'
import * as path from 'path'
import * as zlib from 'zlib'
import Store from 'electron-store'
import { TRACK_NAMES, SESSION_NAMES } from './constants'
import { getProtocolConfig } from './protocolDispatcher'

const store = new Store()

const BUFFER_WINDOW_S = 30

let activeFileStream: fs.WriteStream | null = null
let activeGzipStream: zlib.Gzip | null = null
let activeFilePath = ''
let lastSessionTime = -1

let currentTrackId: number | null = null
let currentSessionType: number | null = null
let isLoggingEnabled = store.get('logging.enabled', false) as boolean
let logDir = store.get('logging.directory', '') as string

interface BufferEntry {
  line: string
  sessionTime: number
}
let rollingBuffer: BufferEntry[] = []

const dedupeCache = new Map<string, string>()
const DEDUPE_TYPES = new Set(['session', 'tyre_sets', 'participants', 'all_status', 'status', 'timing', 'damage'])

function getDedupeString(row: any): string {
  const clone = { ...row }
  delete clone.ts
  delete clone.session_time
  return JSON.stringify(clone)
}

export function initSessionRecorder() {
  store.onDidChange('logging.enabled', (newVal) => {
    isLoggingEnabled = newVal as boolean
    if (!isLoggingEnabled) {
      closeActiveStream()
    }
  })

  store.onDidChange('logging.directory', (newVal) => {
    logDir = newVal as string
    if (isLoggingEnabled) {
      closeActiveStream()
    }
  })
}

function flushBufferToDisk(entries: BufferEntry[]) {
  if (!activeGzipStream || entries.length === 0) return
  for (const entry of entries) {
    activeGzipStream.write(entry.line)
  }
}

function flushOldBufferEntries() {
  if (lastSessionTime < 0) return
  const cutoff = lastSessionTime - BUFFER_WINDOW_S
  let flushIdx = 0
  while (flushIdx < rollingBuffer.length && rollingBuffer[flushIdx].sessionTime < cutoff) {
    flushIdx++
  }
  if (flushIdx > 0) {
    flushBufferToDisk(rollingBuffer.splice(0, flushIdx))
  }
}

function closeActiveStream() {
  if (activeGzipStream) {
    flushBufferToDisk(rollingBuffer)
    console.log(`[SessionRecorder] Closing active stream and flushing to disk.`)
    activeGzipStream.end()
    activeGzipStream = null
  }
  if (activeFileStream) {
    activeFileStream = null
  }
  currentTrackId = null
  currentSessionType = null
  lastSessionTime = -1
  activeFilePath = ''
  rollingBuffer = []
  dedupeCache.clear()
}

function startNewStream(trackId: number, sessionType: number) {
  closeActiveStream()

  if (!isLoggingEnabled || !logDir) return

  try {
    if (!fs.existsSync(logDir)) {
      fs.mkdirSync(logDir, { recursive: true })
    }

    const trackName = (TRACK_NAMES[trackId] || `Track_${trackId}`).replace(/[^a-z0-9]/gi, '_').toLowerCase()
    const sessionName = (SESSION_NAMES[sessionType] || `Session_${sessionType}`).replace(/[^a-z0-9]/gi, '_').toLowerCase()
    const { active } = getProtocolConfig()
    const protocol = active === 2024 ? 'f1_24' : 'f1_25'

    const timestamp = new Date().toISOString().replace(/[:.]/g, '-')
    const filename = `${protocol}_${trackId}_${trackName}_${sessionName}_${timestamp}.tnrd`

    activeFilePath = path.join(logDir, filename)
    lastSessionTime = -1
    rollingBuffer = []

    activeFileStream = fs.createWriteStream(activeFilePath, { flags: 'a' })
    activeGzipStream = zlib.createGzip()
    activeGzipStream.pipe(activeFileStream)

    const header = {
      magic: 'TNRD_V1',
      protocol: active,
      track_id: trackId,
      track_name: TRACK_NAMES[trackId] || 'Unknown',
      session_type: sessionType,
      session_name: SESSION_NAMES[sessionType] || 'Unknown',
      start_time: Date.now()
    }

    // Header goes directly to disk — it's metadata, not time-series data
    activeGzipStream.write(JSON.stringify(header) + '\n')

    currentTrackId = trackId
    currentSessionType = sessionType
    console.log(`[SessionRecorder] Started new recording: ${filename} (Track: ${header.track_name}, Session: ${header.session_name})`)
  } catch (err) {
    console.error('[SessionRecorder] Failed to start stream:', err)
  }
}

export function recordRow(row: any) {
  if (!isLoggingEnabled || !logDir) return

  if (row.type === 'session') {
    const trackId = row.track_id
    const sessionType = row.session_type

    if (trackId !== currentTrackId || sessionType !== currentSessionType || !activeGzipStream) {
      startNewStream(trackId, sessionType)
    }
  }

  if (!activeGzipStream) return

  if (lastSessionTime >= 0 && row.session_time !== undefined && row.session_time < lastSessionTime - 0.2) {
    console.log(`[SessionRecorder] Rewind detected! Truncating buffer from ${lastSessionTime}s to ${row.session_time}s...`)
    truncateTimeline(row.session_time)
    return
  } else if (row.session_time !== undefined && row.session_time > lastSessionTime) {
    lastSessionTime = row.session_time
  }

  if (DEDUPE_TYPES.has(row.type)) {
    const hash = getDedupeString(row)
    if (dedupeCache.get(row.type) === hash) return
    dedupeCache.set(row.type, hash)
  }

  const line = JSON.stringify(row) + '\n'

  if (row.type === 'race_event' && row.code === 'SEND') {
    console.log(`[SessionRecorder] Received SEND (Session End) race event. Triggering stream close.`)
    rollingBuffer.push({ line, sessionTime: lastSessionTime })
    closeActiveStream()
    return
  }

  // Tag rows without session_time with the most recent known time so the
  // buffer can age them out correctly
  rollingBuffer.push({ line, sessionTime: row.session_time ?? lastSessionTime })
  flushOldBufferEntries()
}

function truncateTimeline(newSessionTime: number) {
  const bufferStart = rollingBuffer.length > 0 ? rollingBuffer[0].sessionTime : Infinity

  if (newSessionTime < bufferStart) {
    // Rewind goes beyond the buffer window — on-disk data not truncated.
    // F1 flashbacks are ≤ 30s so this should never happen with a 30s buffer.
    console.warn(`[SessionRecorder] Rewind target ${newSessionTime}s is before buffer start ${bufferStart}s — on-disk portion not truncated`)
  }

  rollingBuffer = rollingBuffer.filter(entry => entry.sessionTime <= newSessionTime)
  lastSessionTime = newSessionTime
  dedupeCache.clear()
  console.log(`[SessionRecorder] Rewind complete. Resuming from ${newSessionTime}s.`)
}
