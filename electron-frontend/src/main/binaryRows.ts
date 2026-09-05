// Main-process helpers for reading the hot-row binary format. MUST stay
// byte-compatible with protocol_parser_library/include/tnrp/BinaryRows.h (the
// C++ encoder, used by both live and playback) and
// src/renderer/src/lib/decodeBinaryBatch.ts (the renderer decoder). Only the
// decode-side record knowledge lives here — encoding happens in C++.

// Byte length of the record starting at offset `o`, or -1 for an unknown tag.
function recordLen(batch: Buffer, o: number): number {
  if (o < 0 || o >= batch.length) return -1
  switch (batch[o]) {
    case 1: return 46                       // telemetry
    case 2: return 29                       // motion
    case 4: return 21                       // motion_ex
    case 3: return o + 2 < batch.length
      ? 3 + batch[o + 2] * 16               // positions: tag + player_idx + n + n*(x,z)
      : -1
    default: return -1
  }
}

export interface BinaryBatchInspection {
  bytes: number
  records: number
  telemetry: number
  motion: number
  positions: number
  motionEx: number
  valid: boolean
  invalidOffset: number | null
  invalidTag: number | null
  expectedRecordBytes: number | null
  remainingBytes: number | null
}

// Cheap structural validation for diagnostics. This intentionally reads only
// record tags/lengths; the renderer remains the sole full decoder.
export function inspectBinaryBatch(batch: Buffer): BinaryBatchInspection {
  const result: BinaryBatchInspection = {
    bytes: batch.length,
    records: 0,
    telemetry: 0,
    motion: 0,
    positions: 0,
    motionEx: 0,
    valid: true,
    invalidOffset: null,
    invalidTag: null,
    expectedRecordBytes: null,
    remainingBytes: null,
  }
  let o = 0
  while (o < batch.length) {
    const tag = batch[o]
    const len = recordLen(batch, o)
    if (len < 0 || o + len > batch.length) {
      result.valid = false
      result.invalidOffset = o
      result.invalidTag = tag ?? null
      result.expectedRecordBytes = len >= 0 ? len : null
      result.remainingBytes = batch.length - o
      break
    }
    result.records++
    if (tag === 1) result.telemetry++
    else if (tag === 2) result.motion++
    else if (tag === 3) result.positions++
    else if (tag === 4) result.motionEx++
    o += len
  }
  return result
}

// Keep only the time-series records needed to reconstruct chart history after
// a hidden renderer resumes. Positions are intentionally excluded: the track
// map only needs the next/latest frame, while retaining every 24-car position
// record for a long chart window would dominate the bounded resume cache.
export function chartHistoryRecords(batch: Buffer): Buffer {
  const parts: Buffer[] = []
  let o = 0
  while (o < batch.length) {
    const len = recordLen(batch, o)
    if (len < 0 || o + len > batch.length) break
    if (batch[o] !== 3) parts.push(Buffer.from(batch.subarray(o, o + len)))
    o += len
  }
  return parts.length === 0 ? Buffer.alloc(0) : Buffer.concat(parts)
}
