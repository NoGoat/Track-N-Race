import * as fs from 'fs'
import * as zlib from 'zlib'
import * as readline from 'readline'
import { broadcastToWindows } from './index'

let activeFilePath: string | null = null
let rl: readline.Interface | null = null
let stream: fs.ReadStream | null = null
let gunzip: zlib.Gunzip | null = null

// Playback state
let isPlaying = false
let speedMultiplier = 1
let totalDurationS = 1 // Prevent division by zero
let startSessionTime = 0
let currentSessionTime = 0
let virtualTime = 0
let lastUpdateRealTime = 0
let playbackTimer: NodeJS.Timeout | null = null
let headerData: any = null

// Streaming buffer (the "chunk")
const LINE_BUFFER_LIMIT = 50000 // roughly 25-50MB of uncompressed text
let lineBuffer: string[] = []
let bufferIndex = 0
let isStreamEOF = false

let onStateChange: ((state: any) => void) | null = null

export function setOnPlayerStateChange(cb: (state: any) => void) {
  onStateChange = cb
}

function emitState() {
  if (onStateChange) {
    onStateChange({
      isPlaying,
      speed: speedMultiplier,
      progressPct: totalDurationS > 0 ? (currentSessionTime - startSessionTime) / totalDurationS : 0,
      currentTime: currentSessionTime,
      totalTime: totalDurationS,
      filename: activeFilePath ? activeFilePath.split(/[\\/]/).pop() : null
    })
  }
}

export async function loadFile(filePath: string): Promise<boolean> {
  closePlayer()
  activeFilePath = filePath
  
  // Pass 1: Fast scan to get total duration and start time
  try {
    await scanFileForDuration(filePath)
  } catch (err) {
    console.error('[Player] Failed to scan file:', err)
    return false
  }

  // Pass 2: Open stream and start reading into buffer
  startStreamingFrom(0) // Start from beginning of session time offset 0
  
  // Wait briefly for initial buffer to fill
  await new Promise(r => setTimeout(r, 200))
  
  virtualTime = startSessionTime
  currentSessionTime = startSessionTime
  emitState()
  return true
}

async function scanFileForDuration(filePath: string): Promise<void> {
  return new Promise((resolve, reject) => {
    const s = fs.createReadStream(filePath)
    const gz = zlib.createGunzip()
    const r = readline.createInterface({ input: s.pipe(gz) })
    
    let firstTime: number | null = null
    let lastTime: number | null = null
    
    r.on('line', (line) => {
      try {
        const obj = JSON.parse(line)
        if (obj.magic === 'TNRD_V1') {
          headerData = obj
          return
        }
        if (obj.session_time !== undefined) {
          if (firstTime === null) firstTime = obj.session_time
          lastTime = obj.session_time
        }
      } catch (e) {}
    })
    
    r.on('close', () => {
      startSessionTime = firstTime || 0
      currentSessionTime = startSessionTime
      totalDurationS = Math.max(0.1, (lastTime || 0) - startSessionTime)
      resolve()
    })
    
    const handleError = (err: Error) => {
      console.warn('[Player] Scan stream error (likely truncated file):', err.message)
      startSessionTime = firstTime || 0
      currentSessionTime = startSessionTime
      totalDurationS = Math.max(0.1, (lastTime || 0) - startSessionTime)
      resolve()
    }
    
    r.on('error', handleError)
    s.on('error', handleError)
    gz.on('error', handleError)
  })
}

function startStreamingFrom(targetSessionTime: number) {
  if (rl) {
    rl.close()
  }
  if (gunzip) gunzip.destroy()
  if (stream) stream.destroy()
  
  lineBuffer = []
  bufferIndex = 0
  isStreamEOF = false
  
  stream = fs.createReadStream(activeFilePath!)
  gunzip = zlib.createGunzip()
  rl = readline.createInterface({ input: stream.pipe(gunzip) })
  
  let fastForwarding = targetSessionTime > startSessionTime
  
  rl.on('line', (line) => {
    // If we are fast forwarding, we discard lines until we reach target time
    if (fastForwarding) {
      try {
        const obj = JSON.parse(line)
        if (obj.session_time !== undefined && obj.session_time >= targetSessionTime) {
          fastForwarding = false
          lineBuffer.push(line)
        }
      } catch (e) {}
    } else {
      lineBuffer.push(line)
      if (lineBuffer.length > bufferIndex + LINE_BUFFER_LIMIT) {
        // We've buffered enough ahead, pause stream
        rl?.pause()
      }
    }
  })
  
  rl.on('close', () => {
    isStreamEOF = true
  })
  
  const handleStreamError = (err: Error) => {
    console.warn('[Player] Streaming error (likely truncated file):', err.message)
    isStreamEOF = true
  }
  
  rl.on('error', handleStreamError)
  gunzip.on('error', handleStreamError)
  stream.on('error', handleStreamError)
}

function playbackLoop() {
  if (!isPlaying) return

  const now = performance.now()
  const deltaRealSec = (now - lastUpdateRealTime) / 1000
  lastUpdateRealTime = now
  
  virtualTime += deltaRealSec * speedMultiplier

  // Dispatch all packets up to virtualTime
  while (bufferIndex < lineBuffer.length) {
    const line = lineBuffer[bufferIndex]
    try {
      const row = JSON.parse(line)
      if (row.session_time !== undefined) {
        if (row.session_time > virtualTime) {
          break // Packet is in the future, wait
        }
        currentSessionTime = row.session_time
      }
      // Broadcast to frontend
      if (row.magic !== 'TNRD_V1') {
        broadcastToWindows(row)
      }
    } catch (e) {}
    
    bufferIndex++
  }

  // Clean up buffer and resume stream if getting empty
  if (bufferIndex > LINE_BUFFER_LIMIT / 2 && rl) {
    lineBuffer = lineBuffer.slice(bufferIndex)
    bufferIndex = 0
    rl.resume()
  }

  if (bufferIndex >= lineBuffer.length && isStreamEOF) {
    // Reached end of file
    isPlaying = false
  }

  emitState()
  
  if (isPlaying) {
    playbackTimer = setTimeout(playbackLoop, 1000 / 60) // 60Hz loop
  }
}

export function play() {
  if (!activeFilePath || isPlaying) return
  isPlaying = true
  lastUpdateRealTime = performance.now()
  playbackLoop()
}

export function pause() {
  isPlaying = false
  if (playbackTimer) clearTimeout(playbackTimer)
  emitState()
}

export function seek(percent: number) {
  if (!activeFilePath) return
  const targetTime = startSessionTime + (totalDurationS * percent)
  virtualTime = targetTime
  currentSessionTime = targetTime
  startStreamingFrom(targetTime)
  emitState()
}

export function setSpeed(mult: number) {
  speedMultiplier = mult
  emitState()
}

export function closePlayer() {
  pause()
  if (rl) rl.close()
  if (gunzip) gunzip.destroy()
  if (stream) stream.destroy()
  rl = null
  gunzip = null
  stream = null
  lineBuffer = []
  bufferIndex = 0
  activeFilePath = null
  emitState()
}
