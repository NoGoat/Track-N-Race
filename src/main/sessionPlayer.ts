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
      filename: activeFilePath ? activeFilePath.split(/[\\/]/).pop() : null,
      isScanning
    })
  }
}

// --------------------------------------------------------
// SCANNING AND LAP BUFFERING
// --------------------------------------------------------

export interface ScanLap {
  lapNum: number
  startSessionTime: number
  endSessionTime: number
  lapTimeMs: number
}

let scannedLaps: ScanLap[] = []
let fastestLapInfo: ScanLap | null = null
let isScanning = false

async function scanFileForLaps(filePath: string): Promise<void> {
  return new Promise((resolve) => {
    const s = fs.createReadStream(filePath)
    const gz = zlib.createGunzip()
    const r = readline.createInterface({ input: s.pipe(gz) })
    
    let firstTime: number | null = null
    let lastTime: number | null = null
    
    let currentLapNum: number | null = null
    let currentLapStart: number = 0
    let currentST: number = 0
    
    scannedLaps = []
    fastestLapInfo = null
    
    r.on('line', (line) => {
      try {
        const obj = JSON.parse(line)
        if (obj.magic === 'TNRD_V1') {
          headerData = obj
          return
        }
        if (obj.session_time !== undefined) {
          currentST = obj.session_time
          if (firstTime === null) firstTime = currentST
          lastTime = currentST
        }
        
        if (obj.type === 'lap') {
          if (currentLapNum === null) {
            currentLapNum = obj.lap_num
            currentLapStart = obj.current_lap_ms > 0 ? currentST - obj.current_lap_ms / 1000 : currentST
          } else if (obj.lap_num > currentLapNum) {
            // Lap finished!
            const lapTimeMs = obj.last_lap_ms
            const newLap: ScanLap = {
              lapNum: currentLapNum,
              startSessionTime: currentLapStart,
              endSessionTime: currentST,
              lapTimeMs
            }
            scannedLaps.push(newLap)
            if (lapTimeMs > 0 && lapTimeMs < 300000) {
              if (!fastestLapInfo || lapTimeMs < fastestLapInfo.lapTimeMs) {
                fastestLapInfo = newLap
              }
            }
            currentLapNum = obj.lap_num
            currentLapStart = currentST
          }
        }
      } catch (e) {}
    })
    
    const finalize = () => {
      startSessionTime = firstTime || 0
      currentSessionTime = startSessionTime
      totalDurationS = Math.max(0.1, (lastTime || 0) - startSessionTime)
      resolve()
    }
    
    const handleError = (err: Error) => {
      console.warn('[Player] Scan stream error (likely truncated file):', err.message)
      finalize()
    }
    
    r.on('close', finalize)
    r.on('error', handleError)
    s.on('error', handleError)
    gz.on('error', handleError)
  })
}

async function extractLapTelemetry(filePath: string, lapInfo: ScanLap, eventName: string): Promise<void> {
  return new Promise((resolve) => {
    const s = fs.createReadStream(filePath)
    const gz = zlib.createGunzip()
    const r = readline.createInterface({ input: s.pipe(gz) })
    
    const packets: any[] = []
    
    r.on('line', (line) => {
      try {
        const obj = JSON.parse(line)
        if (obj.session_time !== undefined) {
          if (obj.session_time >= lapInfo.startSessionTime && obj.session_time <= lapInfo.endSessionTime) {
            packets.push(obj)
          } else if (obj.session_time > lapInfo.endSessionTime) {
            r.close() // Early exit!
          }
        }
      } catch(e) {}
    })
    
    const finalize = () => {
      // Build LapData
      const telemetry = packets.filter(p => p.type === 'telemetry')
      const motion = packets.filter(p => p.type === 'motion')
      const statusHistory = packets.filter(p => p.type === 'status')
      
      const lapData = {
        lapNum: lapInfo.lapNum,
        startSessionTime: lapInfo.startSessionTime,
        endSessionTime: lapInfo.endSessionTime,
        telemetry,
        motion,
        statusHistory
      }
      
      broadcastToWindows({ type: eventName, data: lapData })
      
      // Cleanup early exit
      gz.destroy()
      s.destroy()
      resolve()
    }
    
    r.on('close', finalize)
    r.on('error', finalize)
    gz.on('error', finalize)
    s.on('error', finalize)
  })
}

// --------------------------------------------------------

export async function loadFile(filePath: string): Promise<boolean> {
  closePlayer()
  activeFilePath = filePath
  
  isScanning = true
  emitState()
  
  // Pass 1: Comprehensive scan for laps and duration
  try {
    await scanFileForLaps(filePath)
    
    // Pass 2: Extract fastest lap
    if (fastestLapInfo) {
      await extractLapTelemetry(filePath, fastestLapInfo, 'playback_fastest_lap')
    }
  } catch (err) {
    console.error('[Player] Failed to scan file:', err)
    isScanning = false
    emitState()
    return false
  }

  isScanning = false
  
  // Pass 3: Open stream and start reading into buffer
  startStreamingFrom(startSessionTime) 
  
  // Wait briefly for initial buffer to fill
  await new Promise(r => setTimeout(r, 200))
  
  virtualTime = startSessionTime
  currentSessionTime = startSessionTime
  emitState()
  return true
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
      if (row.magic !== 'TNRD_V1') {
        broadcastToWindows(row)
      }
    } catch (e) {}
    
    bufferIndex++
  }

  if (bufferIndex > LINE_BUFFER_LIMIT / 2 && rl) {
    lineBuffer = lineBuffer.slice(bufferIndex)
    bufferIndex = 0
    rl.resume()
  }

  if (bufferIndex >= lineBuffer.length && isStreamEOF) {
    isPlaying = false
  }

  emitState()
  
  if (isPlaying) {
    playbackTimer = setTimeout(playbackLoop, 1000 / 60)
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
  
  // Async fetch previous lap
  const prevLap = scannedLaps.slice().reverse().find(l => l.endSessionTime <= targetTime)
  if (prevLap) {
    extractLapTelemetry(activeFilePath, prevLap, 'playback_previous_lap')
  }
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
  scannedLaps = []
  fastestLapInfo = null
  isScanning = false
  emitState()
}
