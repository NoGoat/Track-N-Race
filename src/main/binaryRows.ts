// Main-process helpers for reading the hot-row binary format. MUST stay
// byte-compatible with protocol_parser_library/include/tnrp/BinaryRows.h (the
// C++ encoder, used by both live and playback) and
// src/renderer/src/lib/decodeBinaryBatch.ts (the renderer decoder). Only the
// decode-side record knowledge lives here — encoding happens in C++.

// ── Forward-fill support (live smoothing) ──────────────────────────────────
// session_time is a little-endian f32 at byte offset 1 (right after the tag) for
// telemetry/motion/motion_ex records.
export function recSessionTime(rec: Buffer): number { return rec.readFloatLE(1) }

export function recWithSessionTime(rec: Buffer, st: number): Buffer {
  const c = Buffer.from(rec)
  c.writeFloatLE(st, 1)
  return c
}

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

// Walk a batch and return the LAST record of each fixed-size hot type, as a tight
// copy. Used to hold-and-advance the last sample when a tick has no fresh data.
export function lastHotRecords(batch: Buffer): { tel?: Buffer; mot?: Buffer; motEx?: Buffer } {
  const out: { tel?: Buffer; mot?: Buffer; motEx?: Buffer } = {}
  let o = 0
  while (o < batch.length) {
    const len = recordLen(batch, o)
    if (len < 0 || o + len > batch.length) break
    const tag = batch[o]
    if (tag === 1) out.tel = Buffer.from(batch.subarray(o, o + len))
    else if (tag === 2) out.mot = Buffer.from(batch.subarray(o, o + len))
    else if (tag === 4) out.motEx = Buffer.from(batch.subarray(o, o + len))
    o += len
  }
  return out
}

// session_time of every telemetry (tag 1) record in the batch, in order. Used by
// the live rate detector to measure the real inter-packet period.
export function telemetrySessionTimes(batch: Buffer): number[] {
  const out: number[] = []
  let o = 0
  while (o < batch.length) {
    const len = recordLen(batch, o)
    if (len < 0 || o + len > batch.length) break
    if (batch[o] === 1) out.push(batch.readFloatLE(o + 1))
    o += len
  }
  return out
}
