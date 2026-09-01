// Main-process helpers for reading the hot-row binary format. MUST stay
// byte-compatible with protocol_parser_library/include/tnrp/BinaryRows.h (the
// C++ encoder, used by both live and playback) and
// src/renderer/src/lib/decodeBinaryBatch.ts (the renderer decoder). Only the
// decode-side record knowledge lives here — encoding happens in C++.

// Byte length of the record starting at offset `o`, or -1 for an unknown tag.
function recordLen(batch: Buffer, o: number): number {
  switch (batch[o]) {
    case 1: return 46                       // telemetry
    case 2: return 29                       // motion
    case 4: return 21                       // motion_ex
    case 3: return 3 + batch[o + 2] * 16    // positions: tag + player_idx + n + n*(x,z)
    default: return -1
  }
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
