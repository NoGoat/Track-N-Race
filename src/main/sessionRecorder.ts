import * as fs from 'fs'
import * as path from 'path'
import * as zlib from 'zlib'
import Store from 'electron-store'
import { TRACK_NAMES, SESSION_NAMES } from './constants'
import { getProtocolConfig } from './protocolDispatcher'

const store = new Store()

let activeFileStream: fs.WriteStream | null = null
let activeGzipStream: zlib.Gzip | null = null

let currentTrackId: number | null = null
let currentSessionType: number | null = null
let isLoggingEnabled = store.get('logging.enabled', false) as boolean
let logDir = store.get('logging.directory', '') as string

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

function closeActiveStream() {
  if (activeGzipStream) {
    console.log(`[SessionRecorder] Closing active stream and flushing to disk.`)
    activeGzipStream.end()
    activeGzipStream = null
  }
  if (activeFileStream) {
    activeFileStream = null
  }
  currentTrackId = null
  currentSessionType = null
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
    
    const filePath = path.join(logDir, filename)
    
    activeFileStream = fs.createWriteStream(filePath, { flags: 'a' })
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

  if (activeGzipStream) {
    if (DEDUPE_TYPES.has(row.type)) {
      const hash = getDedupeString(row)
      if (dedupeCache.get(row.type) === hash) {
        return
      }
      dedupeCache.set(row.type, hash)
    }

    // Write row as JSON Lines
    activeGzipStream.write(JSON.stringify(row) + '\n')
    
    // Close stream and flush to disk when the session ends
    if (row.type === 'race_event' && row.code === 'SEND') {
      console.log(`[SessionRecorder] Received SEND (Session End) race event. Triggering stream close.`)
      closeActiveStream()
    }
  }
}
