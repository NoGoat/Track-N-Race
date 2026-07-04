import * as fs from 'fs'
import * as zlib from 'zlib'
import * as os from 'os'
import * as path from 'path'
import { app } from 'electron'
import { broadcastToWindows, broadcastBatchToWindows } from './index'
import { labelsForFormat, getLiveFormat, cardColorSpecs } from './bridgeManager'
import { BinaryWriter, encodeTelemetry, encodeMotion, encodeMotionEx } from './binaryRows'
import { medianRoundedPeriodS } from './binaryForwardFill'


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

// Pre-encoded binary store for the hot 60 Hz rows (telemetry + motion), built
// once during the load scan so a seek flush is a binary slice + tiny cold parse
// instead of the renderer re-parsing ~10 min of JSON. `hotStart` has length
// hotTimes.length + 1 (byte offsets into hotBin, last entry = total length).
let hotBin = Buffer.alloc(0)
let hotTimes = new Float32Array(0)
let hotStart = new Float64Array(0)
// Sparse cold rows kept as raw JSONL lines + time, for the seek flush.
interface ColdRow { t: number; line: string }
let coldStatus: ColdRow[] = []
let coldDamage: ColdRow[] = []
let coldLap: ColdRow[] = []

// Measured telemetry period of the recording (seconds), so playback emits at the
// recording's true rate with hot-row forward-fill — mirroring the live path. Set at
// load from the indexed telemetry session_time deltas; 1/60 until then.
let recordingPeriodS = 1 / 60
// Last delivered hot rows (for hold-and-advance fills) + last emitted telemetry time.
let lastTelRow: any = null
let lastMotionRow: any = null
let lastMotionExRow: any = null
let lastEmitTelST = 0

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

      // Hot-row binary store + sparse cold rows, built in this same parse pass.
      const hotWriter = new BinaryWriter()
      const hotTimesArr: number[] = []
      const hotStartArr: number[] = []
      coldStatus = []
      coldDamage = []
      coldLap = []
      // Telemetry session_time deltas, to measure the recording's frame period.
      const telDeltas: number[] = []
      let prevTelST: number | null = null

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
          
          // Map types: 1=telemetry, 2=motion, 3=status, 4=damage, 5=lap, 6=positions, 7=participants, 8=session, 9=timing, 10=all_status, 0=other
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
          else if (obj.type === 'motion_ex') typeEnum = 11
          tempTypes.push(typeEnum)

          // Pre-encode the heavy hot rows to binary; stash sparse cold rows whole.
          if (obj.type === 'telemetry') {
            hotStartArr.push(hotWriter.length); hotTimesArr.push(currentST)
            encodeTelemetry(hotWriter, obj)
            if (prevTelST !== null) {
              const d = currentST - prevTelST
              if (d >= 0.005 && d <= 0.2) telDeltas.push(d)   // 200..5 Hz window
            }
            prevTelST = currentST
          } else if (obj.type === 'motion') {
            hotStartArr.push(hotWriter.length); hotTimesArr.push(currentST)
            encodeMotion(hotWriter, obj)
          } else if (obj.type === 'motion_ex') {
            hotStartArr.push(hotWriter.length); hotTimesArr.push(currentST)
            encodeMotionEx(hotWriter, obj)
          } else if (obj.type === 'status') {
            coldStatus.push({ t: currentST, line: lineStr })
          } else if (obj.type === 'damage') {
            coldDamage.push({ t: currentST, line: lineStr })
          } else if (obj.type === 'lap') {
            coldLap.push({ t: currentST, line: lineStr })
          }

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

      // Finalize the hot-row binary store (tight copy; sentinel end offset appended).
      hotStartArr.push(hotWriter.length)
      hotBin = Buffer.from(hotWriter.view())
      hotTimes = new Float32Array(hotTimesArr)
      hotStart = new Float64Array(hotStartArr)

      // Measured recording frame period (same estimator as the live path); keep the
      // 1/60 default if there aren't enough telemetry rows to trust a median.
      recordingPeriodS = telDeltas.length >= 8 ? medianRoundedPeriodS(telDeltas) : 1 / 60

      startSessionTime = firstTime || 0
      currentSessionTime = startSessionTime
      totalDurationS = Math.max(0.1, (lastTime || 0) - startSessionTime)

      // Tell the renderer which protocol this clip is, with its library i18n
      // catalog, so playback shows the recorded format's text (e.g. Boost /
      // Straight Line Mode under 2026) instead of the live/default labels.
      const pbFormat = typeof headerData?.protocol === 'number' ? headerData.protocol : 2025
      broadcastToWindows({
        type: 'protocol_status',
        detected_format: pbFormat,
        active_format: pbFormat,
        override: 'auto',
        capabilities: {
          gameYear: pbFormat - 2000,
          hasBlisters: pbFormat >= 2025,
          hasLiveryColors: pbFormat >= 2025,
          hasLapPositions: pbFormat >= 2025,
        },
        labels: labelsForFormat(pbFormat),
        cardColors: cardColorSpecs(),
        aero_mode: pbFormat >= 2026 ? 'slm' : 'drs',
      })

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
  
  const offsetsToRead: number[] = []
  let neededMask = (1 << 6) | (1 << 7) | (1 << 8) | (1 << 9) | (1 << 10)
  
  const limit = Math.min(upToIndex, offsetsArray.length - 1)
  for (let i = limit; i >= 0 && neededMask !== 0; i--) {
    const t = typesArray[i]
    if (t >= 6 && t <= 10) {
      if ((neededMask & (1 << t)) !== 0) {
        offsetsToRead.push(offsetsArray[i])
        neededMask &= ~(1 << t)
      }
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


// Removed extractAndBroadcastLap since frontend uses speedRpmBlocks

// first index i with arr[i] >= v
function lowerBoundF32(arr: Float32Array, v: number): number {
  let lo = 0, hi = arr.length
  while (lo < hi) { const mid = (lo + hi) >> 1; if (arr[mid] < v) lo = mid + 1; else hi = mid }
  return lo
}
// first index i with arr[i] > v
function upperBoundF32(arr: Float32Array, v: number): number {
  let lo = 0, hi = arr.length
  while (lo < hi) { const mid = (lo + hi) >> 1; if (arr[mid] <= v) lo = mid + 1; else hi = mid }
  return lo
}

// Collect sparse cold rows in [fromT, toT] as joined JSONL, and refresh
// lastPackets[type] with the most recent row at/<= toT (for playback dedup).
function gatherCold(rows: ColdRow[], fromT: number, toT: number, type: string, out: string[]): void {
  // Full linear scan (these arrays are small, ~2 Hz) — robust to the occasional
  // out-of-order packet the recordings can contain.
  let lastLine: string | null = null
  for (const r of rows) {
    if (r.t > toT) continue
    lastLine = r.line
    if (r.t >= fromT) out.push(r.line)
  }
  if (lastLine) { try { lastPackets[type] = JSON.parse(lastLine) } catch (e) {} }
}

function extractAndBroadcastSeek(targetTime: number, currentLapStart: number, currentLapNum: number) {
  if (hotTimes.length === 0 && coldStatus.length === 0) return
  const startTime = Math.min(targetTime - 600, currentLapStart)

  // Hot rows (telemetry+motion): a zero-parse binary slice of the pre-built store.
  const lo = lowerBoundF32(hotTimes, startTime)
  const hi = upperBoundF32(hotTimes, targetTime)
  const binary = (hi > lo)
    ? Buffer.from(hotBin.subarray(hotStart[lo], hotStart[hi]))   // copy: detach from the big store before IPC
    : Buffer.alloc(0)

  // Cold rows (status/damage/lap): tiny, kept as JSON.
  const coldLines: string[] = []
  gatherCold(coldStatus, startTime, targetTime, 'status', coldLines)
  gatherCold(coldDamage, startTime, targetTime, 'damage', coldLines)
  gatherCold(coldLap, startTime, targetTime, 'lap', coldLines)

  broadcastToWindows({
    type: 'playback_seek_flush_bin',
    binary,
    coldJson: coldLines.join('\n'),
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
    events: scannedEvents
  })
  
  emitState()
  return true
}

function playbackLoop() {
  if (!isPlaying) return

  const now = Date.now()
  const deltaRealSec = (now - lastUpdateRealTime) / 1000
  lastUpdateRealTime = now

  virtualTime += deltaRealSec * speedMultiplier
  currentSessionTime = Math.min(virtualTime, startSessionTime + totalDurationS)

  let outBatch = ''            // real rows + sparse dups + hot fills for this tick
  let sawTelemetry = false

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

          const batchStr = buffer.toString('utf8')

          const TYPE_MAP: Record<number, string> = {
            1: 'telemetry', 2: 'motion', 3: 'status', 4: 'damage', 5: 'lap', 6: 'positions',
            7: 'participants', 8: 'session', 9: 'timing', 10: 'all_status'
          }
          const typesToDup = [3, 4, 5, 6, 10, 9, 8]
          const seenTypes = new Set<number>()

          // Capture the last hot row of each kind so a later gap tick can hold it.
          let lastTelLine: string | null = null
          let lastMotLine: string | null = null
          let lastMotExLine: string | null = null

          let scanOffset = 0
          for (let idx = playbackIndex; idx < endIndex; idx++) {
            const type = typesArray[idx]
            seenTypes.add(type)
            let nl = batchStr.indexOf('\n', scanOffset)
            if (nl === -1) nl = batchStr.length
            const line = batchStr.slice(scanOffset, nl)
            scanOffset = nl + 1
            if (typesToDup.includes(type)) {
              try { const row = JSON.parse(line); if (row.type) lastPackets[row.type] = row } catch (e) {}
            } else if (type === 1) { sawTelemetry = true; lastTelLine = line }
            else if (type === 2) { lastMotLine = line }
            else if (type === 11) { lastMotExLine = line }
          }

          if (lastTelLine) { try { lastTelRow = JSON.parse(lastTelLine); lastEmitTelST = lastTelRow.session_time } catch (e) {} }
          if (lastMotLine) { try { lastMotionRow = JSON.parse(lastMotLine) } catch (e) {} }
          if (lastMotExLine) { try { lastMotionExRow = JSON.parse(lastMotExLine) } catch (e) {} }

          // Duplicate sparse packets not seen in this chunk and bundle them into the batch!
          const dupLines: string[] = []
          for (const typeNum of typesToDup) {
            if (!seenTypes.has(typeNum)) {
              const strType = TYPE_MAP[typeNum]
              if (lastPackets[strType]) {
                dupLines.push(JSON.stringify({
                  ...lastPackets[strType],
                  session_time: currentSessionTime
                }))
              }
            }
          }

          outBatch = dupLines.length > 0
            ? batchStr + (batchStr.endsWith('\n') ? '' : '\n') + dupLines.join('\n')
            : batchStr
        } catch (err) {
          console.error('[Player] Playback block read error:', err)
        }
      }
      playbackIndex = endIndex
    }
  }

  // Forward-fill the hot rows on a gap tick (no telemetry delivered) so the chart's
  // leading edge keeps advancing at the recording's rate — held value, session_time
  // nudged forward by one measured frame, capped at the playhead and never backward
  // (mirrors the live HotRowSmoother; recording itself is untouched).
  if (!sawTelemetry && lastTelRow) {
    const target = Math.min(lastEmitTelST + recordingPeriodS, currentSessionTime)
    if (target > lastEmitTelST + 1e-6) {
      lastEmitTelST = target
      const fills = [JSON.stringify({ ...lastTelRow, session_time: target })]
      if (lastMotionRow) fills.push(JSON.stringify({ ...lastMotionRow, session_time: target }))
      if (lastMotionExRow) fills.push(JSON.stringify({ ...lastMotionExRow, session_time: target }))
      outBatch = outBatch
        ? outBatch + (outBatch.endsWith('\n') ? '' : '\n') + fills.join('\n')
        : fills.join('\n')
    }
  }

  if (windowFocused && outBatch) broadcastBatchToWindows(outBatch)

  if (playbackIndex >= timesArray.length) {
    isPlaying = false
  }

  emitState()

  if (isPlaying) {
    playbackTimer = setTimeout(playbackLoop, Math.max(1, Math.round(recordingPeriodS * 1000)))
  }
}

export function play() {
  if (!activeTempFilePath || isPlaying) return
  isPlaying = true
  lastUpdateRealTime = Date.now()
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

  // Realign the forward-fill cursor to the seek point; held rows repopulate from the
  // next delivered telemetry (the seek flush already reset the renderer's buffers).
  lastTelRow = lastMotionRow = lastMotionExRow = null
  lastEmitTelST = targetTime

  // O(log N) lookup in TypedArray
  const newIndex = findFirstIndexTimeGte(targetTime)
  playbackIndex = newIndex !== -1 ? newIndex : offsetsArray.length
  
  // Instantaneously reconstruction & broadcast the latest UI state for this seek point
  broadcastInitialState(playbackIndex)
  
  emitState()
  
  // Broadcast comparisons asynchronously to keep UI seek completely fluid
  // Removed broadcast of playback_previous_lap since renderer uses speedRpmBlocks
  
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
    lastUpdateRealTime = Date.now()
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
  hotBin = Buffer.alloc(0)
  hotTimes = new Float32Array(0)
  hotStart = new Float64Array(0)
  coldStatus = []
  coldDamage = []
  coldLap = []
  recordingPeriodS = 1 / 60
  lastTelRow = lastMotionRow = lastMotionExRow = null
  lastEmitTelST = 0
  lapBlocks.clear()
  lastPackets = {}
  broadcastToWindows({ type: 'playback_close' })
  // Restore the live format's labels (playback may have switched them, e.g. a
  // 2026 clip showing Boost while the live game is 2025).
  const liveFormat = getLiveFormat()
  broadcastToWindows({
    type: 'protocol_status',
    detected_format: liveFormat,
    active_format: liveFormat,
    override: 'auto',
    capabilities: {
      gameYear: liveFormat - 2000,
      hasBlisters: liveFormat >= 2025,
      hasLiveryColors: liveFormat >= 2025,
      hasLapPositions: liveFormat >= 2025,
    },
    labels: labelsForFormat(liveFormat),
    cardColors: cardColorSpecs(),
    aero_mode: liveFormat >= 2026 ? 'slm' : 'drs',
  })
  emitState()
}

