import * as fs from 'fs'
import * as zlib from 'zlib'
import * as os from 'os'
import * as path from 'path'
import { app } from 'electron'
import { broadcastToWindows } from './index'


let activeFilePath: string | null = null
let activeTempFilePath: string | null = null
let tempFileSize = 0

// Playback state
let isPlaying = false
let windowFocused = true
let speedMultiplier = 1
let totalDurationS = 1 // Prevent division by zero
let startSessionTime = 0
let currentSessionTime = 0
let virtualTime = 0
let lastUpdateRealTime = 0
let playbackTimer: NodeJS.Timeout | null = null
let headerData: any = null

// Binary offset indices in TypedArrays (extremely memory efficient)
let offsetsArray = new Float64Array(0)
let timesArray = new Float32Array(0)
let typesArray = new Uint8Array(0)
let playbackIndex = 0

// Sparse packets cache to prevent stuttering
let lastPackets: Record<string, any> = {}

export interface ScanLap {
  lapNum: number
  startSessionTime: number
  endSessionTime: number
  lapTimeMs: number
}

// Slim telemetry/status items for SpeedRPMERS comparative chart views
export interface SlimTelemetry {
  type: 'telemetry'
  session_time: number
  speed_kph: number
  rpm: number
}

export interface SlimStatus {
  type: 'status'
  session_time: number
  ers_pct: number
}

export interface SpeedRpmLapBlock {
  lapNum: number
  startSessionTime: number
  endSessionTime: number
  telemetry: SlimTelemetry[]
  statusHistory: SlimStatus[]
}

let scannedLaps: ScanLap[] = []
let scannedEvents: any[] = []
let fastestLapInfo: ScanLap | null = null
let isScanning = false
let lapBlocks = new Map<number, SpeedRpmLapBlock>()

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
// STARTUP AND LIFECYCLE RECOVERY
// --------------------------------------------------------

export function sweepTempDir() {
  try {
    const tempDir = os.tmpdir()
    const files = fs.readdirSync(tempDir)
    for (const file of files) {
      if (file.startsWith('tracknrace_temp_') && file.endsWith('.tmp')) {
        try {
          fs.unlinkSync(path.join(tempDir, file))
        } catch (e) {
          // Ignore files locked by other active instances
        }
      }
    }
  } catch (err) {
    console.error('[Player] Startup temp sweep failed:', err)
  }
}

// Sweep is now called explicitly from the main process (index.ts) once the single instance lock is verified,
// to prevent secondary startup instances from unlinking temporary files of an active session.

function cleanupTempFile() {
  if (activeTempFilePath) {
    const pathToDelete = activeTempFilePath
    activeTempFilePath = null
    tempFileSize = 0
    fs.unlink(pathToDelete, (err) => {
      if (err && (err as any).code !== 'ENOENT') {
        console.error('[Player] Failed to delete temp file:', err.message)
      }
    })
  }
}

// --------------------------------------------------------
// FILE SCANNING AND FLAT INDEXING
// --------------------------------------------------------

async function decompressToTempFile(gzipPath: string, tempPath: string): Promise<void> {
  return new Promise((resolve, reject) => {
    const readStream = fs.createReadStream(gzipPath)
    const gunzip = zlib.createGunzip()
    const writeStream = fs.createWriteStream(tempPath)
    
    readStream.on('error', reject)
    gunzip.on('error', reject)
    writeStream.on('error', reject)
    
    writeStream.on('finish', () => {
      resolve()
    })
    
    readStream.pipe(gunzip).pipe(writeStream)
  })
}

async function scanAndIndexTempFile(tempPath: string): Promise<void> {
  return new Promise((resolve, reject) => {
    try {
      const fd = fs.openSync(tempPath, 'r')
      const stats = fs.fstatSync(fd)
      tempFileSize = stats.size
      
      const readBuf = Buffer.alloc(256 * 1024) // 256KB chunks
      let leftover = Buffer.alloc(0)
      let currentFileOffset = 0
      
      const tempOffsets: number[] = []
      const tempTimes: number[] = []
      const tempTypes: number[] = []
      
      let firstTime: number | null = null
      let lastTime: number | null = null
      let currentLapNum: number | null = null
      let currentLapStart: number = 0
      let currentST: number = 0
      
      scannedLaps = []
      scannedEvents = []
      fastestLapInfo = null
      headerData = null
      lapBlocks.clear()
      
      const processLine = (lineStr: string, byteOffset: number) => {
        if (!lineStr.trim()) return
        try {
          const obj = JSON.parse(lineStr)
          if (obj.magic === 'TNRD_V1') {
            headerData = obj
            return
          }
          
          if (obj.session_time !== undefined) {
            currentST = obj.session_time
            if (firstTime === null) firstTime = currentST
            lastTime = currentST
          }
          
          // Index every single packet (including roster/participants and map coordinates)
          tempOffsets.push(byteOffset)
          tempTimes.push(currentST)
          
          // Map types: 1=telemetry, 2=motion, 3=status, 4=damage, 5=lap, 6=positions, 7=participants, 8=session, 9=timing, 10=all_status, 11=tyre_sets, 0=other
          let typeEnum = 0
          if (obj.type === 'telemetry') typeEnum = 1
          else if (obj.type === 'motion') typeEnum = 2
          else if (obj.type === 'status') typeEnum = 3
          else if (obj.type === 'damage') typeEnum = 4
          else if (obj.type === 'lap') typeEnum = 5
          else if (obj.type === 'positions') typeEnum = 6
          else if (obj.type === 'participants') typeEnum = 7
          else if (obj.type === 'session') typeEnum = 8
          else if (obj.type === 'timing') typeEnum = 9
          else if (obj.type === 'all_status') typeEnum = 10
          else if (obj.type === 'tyre_sets') typeEnum = 11
          tempTypes.push(typeEnum)
          
          if (obj.type === 'lap') {
            if (currentLapNum === null) {
              currentLapNum = obj.lap_num
              currentLapStart = obj.current_lap_ms > 0 ? currentST - obj.current_lap_ms / 1000 : currentST

            } else if (obj.lap_num > currentLapNum) {
              // Finalize previous lap block
              const prevBlock = lapBlocks.get(currentLapNum)
              if (prevBlock) prevBlock.endSessionTime = currentST

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

          // Build SpeedRPMERS comparative lap blocks
          if (currentLapNum !== null) {
            if (!lapBlocks.has(currentLapNum)) {
              lapBlocks.set(currentLapNum, {
                lapNum: currentLapNum,
                startSessionTime: currentST,
                endSessionTime: currentST,
                telemetry: [],
                statusHistory: []
              })

            }
            const block = lapBlocks.get(currentLapNum)!
            block.endSessionTime = currentST
            
            if (obj.type === 'telemetry') {
              block.telemetry.push({
                type: 'telemetry',
                session_time: obj.session_time,
                speed_kph: obj.speed_kph,
                rpm: obj.rpm
              })
            } else if (obj.type === 'status') {
              block.statusHistory.push({
                type: 'status',
                session_time: obj.session_time,
                ers_pct: obj.ers_pct
              })
            }
          }

          if (obj.type === 'race_event') {
            scannedEvents.push({ ...obj, session_time: obj.session_time ?? currentST })
          }
        } catch (e) {
          // Ignore malformed rows gracefully
        }
      }
      
      while (true) {
        const bytesRead = fs.readSync(fd, readBuf, 0, readBuf.length, null)
        if (bytesRead === 0) break
        
        const chunk = Buffer.concat([leftover, readBuf.subarray(0, bytesRead)])
        let lineStart = 0
        
        while (true) {
          const newlineIdx = chunk.indexOf(10, lineStart) // 10 is '\n'
          if (newlineIdx === -1) {
            leftover = chunk.subarray(lineStart)
            break
          }
          
          const lineBuf = chunk.subarray(lineStart, newlineIdx)
          const lineOffset = currentFileOffset + lineStart
          
          const lineStr = lineBuf.toString('utf8')
          processLine(lineStr, lineOffset)
          
          lineStart = newlineIdx + 1
        }
        
        currentFileOffset += chunk.length - leftover.length
      }
      
      if (leftover.length > 0) {
        processLine(leftover.toString('utf8'), currentFileOffset)
      }
      
      // Cap final lap block
      if (currentLapNum !== null) {
        const finalBlock = lapBlocks.get(currentLapNum)
        if (finalBlock) {
          finalBlock.endSessionTime = currentST
        }
      }

      // Sort each block's telemetry and statusHistory by session_time to handle
      // out-of-order UDP packets in the recording, which would cause advance() to get stuck
      for (const block of lapBlocks.values()) {
        block.telemetry.sort((a, b) => a.session_time - b.session_time)
        block.statusHistory.sort((a, b) => a.session_time - b.session_time)
      }


      fs.closeSync(fd)
      
      // Release raw arrays and move to binary indices
      offsetsArray = new Float64Array(tempOffsets)
      timesArray = new Float32Array(tempTimes)
      typesArray = new Uint8Array(tempTypes)
      
      startSessionTime = firstTime || 0
      currentSessionTime = startSessionTime
      totalDurationS = Math.max(0.1, (lastTime || 0) - startSessionTime)
      
      resolve()
    } catch (err) {
      reject(err)
    }
  })
}

// --------------------------------------------------------
// SEARCH & ATOMIC BLOCK READING HELPERS
// --------------------------------------------------------

function findFirstIndexTimeGte(targetTime: number): number {
  let low = 0
  let high = timesArray.length - 1
  let result = -1
  while (low <= high) {
    const mid = (low + high) >> 1
    if (timesArray[mid] >= targetTime) {
      result = mid
      high = mid - 1
    } else {
      low = mid + 1
    }
  }
  return result
}

function findLastIndexTimeLte(targetTime: number): number {
  let low = 0
  let high = timesArray.length - 1
  let result = -1
  while (low <= high) {
    const mid = (low + high) >> 1
    if (timesArray[mid] <= targetTime) {
      result = mid
      low = mid + 1
    } else {
      high = mid - 1
    }
  }
  return result
}

function tempOffsetsFindIndex(offset: number): number {
  let low = 0
  let high = offsetsArray.length - 1
  while (low <= high) {
    const mid = (low + high) >> 1
    if (offsetsArray[mid] === offset) return mid
    else if (offsetsArray[mid] < offset) low = mid + 1
    else high = mid - 1
  }
  return -1
}

function broadcastInitialState(upToIndex: number) {
  if (offsetsArray.length === 0 || !activeTempFilePath) return
  
  const neededTypes = new Set([6, 7, 8, 9, 10, 11]) // positions, participants, session, timing, all_status, tyre_sets
  const offsetsToRead: number[] = []
  
  const limit = Math.min(upToIndex, offsetsArray.length - 1)
  for (let i = limit; i >= 0; i--) {
    const t = typesArray[i]
    if (neededTypes.has(t)) {
      offsetsToRead.push(offsetsArray[i])
      neededTypes.delete(t)
    }
  }
  
  offsetsToRead.sort((a, b) => a - b)
  
  if (offsetsToRead.length === 0) return
  
  try {
    const fd = fs.openSync(activeTempFilePath, 'r')
    for (const offset of offsetsToRead) {
      const idx = tempOffsetsFindIndex(offset)
      if (idx === -1) continue
      
      const nextOffset = (idx + 1 < offsetsArray.length) ? offsetsArray[idx + 1] : tempFileSize
      const length = nextOffset - offset
      
      if (length <= 0) continue
      
      const buffer = Buffer.alloc(length)
      fs.readSync(fd, buffer, 0, length, offset)
      
      const lineStr = buffer.toString('utf8').trim()
      if (!lineStr) continue
      try {
        const row = JSON.parse(lineStr)
        if (row.magic !== 'TNRD_V1') {
          if (row.type) {
            lastPackets[row.type] = row
          }
          broadcastToWindows(row)
        }
      } catch (e) {}
    }
    fs.closeSync(fd)
  } catch (err) {
    console.error('[Player] Error broadcasting initial state:', err)
  }
}


function readTelemetryBlock(startTime: number, endTime: number): any[] {
  if (offsetsArray.length === 0 || !activeTempFilePath) return []
  
  const startIndex = findFirstIndexTimeGte(startTime)
  const endIndex = findLastIndexTimeLte(endTime)
  
  if (startIndex === -1 || endIndex === -1 || startIndex > endIndex) return []
  
  const startOffset = offsetsArray[startIndex]
  const endOffset = (endIndex + 1 < offsetsArray.length) ? offsetsArray[endIndex + 1] : tempFileSize
  const length = endOffset - startOffset
  
  if (length <= 0) return []
  
  try {
    const fd = fs.openSync(activeTempFilePath, 'r')
    const buffer = Buffer.alloc(length)
    fs.readSync(fd, buffer, 0, length, startOffset)
    fs.closeSync(fd)
    
    const lines = buffer.toString('utf8').split('\n')
    const results: any[] = []
    for (const line of lines) {
      if (!line.trim()) return results // stop early if buffer trailing spaces
      try {
        const obj = JSON.parse(line)
        if (obj.session_time !== undefined) {
          results.push(obj)
        }
      } catch (e) {}
    }
    return results
  } catch (err) {
    console.error('[Player] Error reading telemetry block:', err)
    return []
  }
}

function extractAndBroadcastLap(lapInfo: ScanLap, eventName: string) {
  const packets = readTelemetryBlock(lapInfo.startSessionTime, lapInfo.endSessionTime)
  
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
}

function extractAndBroadcastSeek(targetTime: number, currentLapStart: number, currentLapNum: number) {
  const startTime = Math.min(targetTime - 120, currentLapStart)
  const packets = readTelemetryBlock(startTime, targetTime)
  
  for (const p of packets) {
    if (p.type) {
      lastPackets[p.type] = p
    }
  }
  
  const telemetry = packets.filter(p => p.type === 'telemetry')
  const motion = packets.filter(p => p.type === 'motion')
  const status = packets.filter(p => p.type === 'status')
  const damage = packets.filter(p => p.type === 'damage')

  const flushLap = lastPackets['lap'] ?? null
  broadcastToWindows({
    type: 'playback_seek_flush',
    telemetry,
    motion,
    status,
    damage,
    // Current lap row at the seek point, so the renderer can refresh the singular
    // `lap` state (the strategy reads lap_num/last_lap_ms from it, not the buffers).
    lap: flushLap,
    currentLapStart,
    lapNum: currentLapNum
  })
}

// --------------------------------------------------------
// CORE PLAYER APIS
// --------------------------------------------------------

export async function loadFile(filePath: string): Promise<boolean> {
  closePlayer()
  activeFilePath = filePath


  isScanning = true
  emitState()
  
  const tempDir = os.tmpdir()
  const tempPath = path.join(tempDir, `tracknrace_temp_${Date.now()}.tmp`)
  activeTempFilePath = tempPath
  
  try {
    // Phase 1: Stream decompression to OS temp directory
    await decompressToTempFile(filePath, tempPath)
    
    // Phase 2: Index and scan laps in a single pass
    await scanAndIndexTempFile(tempPath)
    
    // Phase 3: Fast-broadcast fastest lap
    if (fastestLapInfo) {
      extractAndBroadcastLap(fastestLapInfo, 'playback_fastest_lap')
    }
  } catch (err) {
    console.error('[Player] Failed to load telemetry file:', err)
    isScanning = false
    cleanupTempFile()
    emitState()
    return false
  }
  
  isScanning = false
  playbackIndex = 0
  virtualTime = startSessionTime
  currentSessionTime = startSessionTime
  
  // Instantaneously reconstruction & broadcast the latest UI state
  broadcastInitialState(0)
  
  // Broadcast comparative SpeedRPMERS blocks exactly once on load to renderers
  const blocksArr = Array.from(lapBlocks.values())

  broadcastToWindows({
    type: 'playback_lap_blocks',
    blocks: blocksArr,
    fastestLapNum: fastestLapInfo ? fastestLapInfo.lapNum : 0,
    events: scannedEvents,
    // Authoritative per-lap times (seek-correct) for the Strategy page target tables.
    laps: scannedLaps.map(l => ({ lapNum: l.lapNum, lapTimeMs: l.lapTimeMs }))
  })
  
  emitState()
  return true
}

function playbackLoop() {
  if (!isPlaying) return
  
  const now = performance.now()
  const deltaRealSec = (now - lastUpdateRealTime) / 1000
  lastUpdateRealTime = now
  
  virtualTime += deltaRealSec * speedMultiplier
  
  if (playbackIndex < timesArray.length && activeTempFilePath) {
    let endIndex = playbackIndex
    while (endIndex < timesArray.length && timesArray[endIndex] <= virtualTime) {
      endIndex++
    }
    
    if (endIndex > playbackIndex) {
      const startOffset = offsetsArray[playbackIndex]
      const endOffset = (endIndex < offsetsArray.length) ? offsetsArray[endIndex] : tempFileSize
      const length = endOffset - startOffset
      
      if (length > 0) {
        try {
          const fd = fs.openSync(activeTempFilePath, 'r')
          const buffer = Buffer.alloc(length)
          fs.readSync(fd, buffer, 0, length, startOffset)
          fs.closeSync(fd)
          
          const lines = buffer.toString('utf8').split('\n')
          const seenTypes = new Set<string>()
          for (const line of lines) {
            if (!line.trim()) continue
            try {
              const row = JSON.parse(line)
              if (row.session_time !== undefined) {
                currentSessionTime = row.session_time
              }
              if (row.magic !== 'TNRD_V1') {
                if (row.type) {
                  lastPackets[row.type] = row
                  seenTypes.add(row.type)
                }
                if (windowFocused) broadcastToWindows(row)
              }
            } catch (e) {}
          }
          
          // Duplicate sparse packets not seen in this chunk
          const typesToDuplicate = ['status', 'damage', 'lap', 'positions', 'all_status', 'timing', 'session']
          for (const type of typesToDuplicate) {
            if (!seenTypes.has(type) && lastPackets[type]) {
              const duplicated = {
                ...lastPackets[type],
                session_time: currentSessionTime
              }
              if (windowFocused) broadcastToWindows(duplicated)
            }
          }
        } catch (err) {
          console.error('[Player] Playback block read error:', err)
        }
      }
      playbackIndex = endIndex
    }
  }
  
  if (playbackIndex >= timesArray.length) {
    isPlaying = false
  }
  
  emitState()
  
  if (isPlaying) {
    playbackTimer = setTimeout(playbackLoop, 1000 / 60)
  }
}

export function play() {
  if (!activeTempFilePath || isPlaying) return
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
  if (!activeTempFilePath || offsetsArray.length === 0) return
  
  const targetTime = startSessionTime + (totalDurationS * percent)
  virtualTime = targetTime
  currentSessionTime = targetTime
  
  // O(log N) lookup in TypedArray
  const newIndex = findFirstIndexTimeGte(targetTime)
  playbackIndex = newIndex !== -1 ? newIndex : offsetsArray.length
  
  // Instantaneously reconstruction & broadcast the latest UI state for this seek point
  broadcastInitialState(playbackIndex)
  
  emitState()
  
  // Broadcast comparisons asynchronously to keep UI seek completely fluid
  const prevLap = scannedLaps.slice().reverse().find(l => l.endSessionTime <= targetTime)
  if (prevLap) {
    setImmediate(() => {
      if (activeTempFilePath) extractAndBroadcastLap(prevLap, 'playback_previous_lap')
    })
  }
  
  const currentLap = scannedLaps.find(l => targetTime >= l.startSessionTime && targetTime <= l.endSessionTime)
  const currentLapStart = currentLap ? currentLap.startSessionTime : targetTime
  const currentLapNum = currentLap ? currentLap.lapNum : 0
  
  setImmediate(() => {
    if (activeTempFilePath) extractAndBroadcastSeek(targetTime, currentLapStart, currentLapNum)
  })
}

export function setSpeed(mult: number) {
  speedMultiplier = mult
  emitState()
}

export function setWindowFocused(focused: boolean): void {
  const wasFocused = windowFocused
  windowFocused = focused
  if (focused && !wasFocused && isPlaying) {
    // Reset the time baseline so the loop doesn't try to replay the entire gap
    lastUpdateRealTime = performance.now()
    // Immediately push current state to the renderer
    broadcastInitialState(playbackIndex)
  }
}

export function closePlayer() {
  pause()
  cleanupTempFile()
  activeFilePath = null
  scannedLaps = []
  scannedEvents = []
  fastestLapInfo = null
  isScanning = false
  playbackIndex = 0
  offsetsArray = new Float64Array(0)
  timesArray = new Float32Array(0)
  typesArray = new Uint8Array(0)
  lapBlocks.clear()
  lastPackets = {}
  broadcastToWindows({ type: 'playback_close' })
  emitState()
}

