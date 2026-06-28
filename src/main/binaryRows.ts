// Main-process encoder for the hot-row binary format. MUST stay byte-compatible
// with protocol_parser_library/include/tnrp/BinaryRows.h (the live encoder) and
// src/renderer/src/lib/decodeBinaryBatch.ts (the renderer decoder). Used by the
// playback player to ship the heavy telemetry/motion rows of a seek flush as
// binary instead of a giant JSON string the renderer would re-parse line by line.

// A growable little-endian buffer.
export class BinaryWriter {
  private buf: Buffer
  private len = 0
  constructor(initial = 1 << 20) { this.buf = Buffer.allocUnsafe(initial) }
  private ensure(n: number): void {
    if (this.len + n <= this.buf.length) return
    let cap = this.buf.length * 2
    while (cap < this.len + n) cap *= 2
    const nb = Buffer.allocUnsafe(cap)
    this.buf.copy(nb, 0, 0, this.len)
    this.buf = nb
  }
  get length(): number { return this.len }
  u8(v: number): void { this.ensure(1); this.buf.writeUInt8(v & 0xff, this.len); this.len += 1 }
  i8(v: number): void { this.ensure(1); this.buf.writeInt8(Math.max(-128, Math.min(127, v | 0)), this.len); this.len += 1 }
  u16(v: number): void { this.ensure(2); this.buf.writeUInt16LE(v & 0xffff, this.len); this.len += 2 }
  f32(v: number): void { this.ensure(4); this.buf.writeFloatLE(v, this.len); this.len += 4 }
  f64(v: number): void { this.ensure(8); this.buf.writeDoubleLE(v, this.len); this.len += 8 }
  // Returns a view over the written bytes (no copy).
  view(): Buffer { return this.buf.subarray(0, this.len) }
}

const TAG_TELEMETRY = 1
const TAG_MOTION = 2

export function encodeTelemetry(w: BinaryWriter, o: any): void {
  w.u8(TAG_TELEMETRY)
  w.f32(o.session_time)
  w.u16(o.speed_kph)
  w.u16(o.rpm)
  w.i8(o.gear)
  w.u8(o.drs)
  w.f32(o.throttle)
  w.f32(o.brake)
  w.f64(o.steering)
  w.u8(o.tyre_temp_surface_rl); w.u8(o.tyre_temp_surface_rr); w.u8(o.tyre_temp_surface_fl); w.u8(o.tyre_temp_surface_fr)
  w.u8(o.tyre_temp_inner_rl); w.u8(o.tyre_temp_inner_rr); w.u8(o.tyre_temp_inner_fl); w.u8(o.tyre_temp_inner_fr)
  w.u16(o.brake_temp_rl); w.u16(o.brake_temp_rr); w.u16(o.brake_temp_fl); w.u16(o.brake_temp_fr)
  w.u16(o.engine_temp)
}

export function encodeMotion(w: BinaryWriter, o: any): void {
  w.u8(TAG_MOTION)
  w.f32(o.session_time)
  w.f64(o.g_lat)
  w.f64(o.g_long)
  w.f64(o.g_vert)
}

// ── Forward-fill support (live smoothing) ──────────────────────────────────
// session_time is a little-endian f32 at byte offset 1 (right after the tag) for
// telemetry/motion/motion_ex records.
export function recSessionTime(rec: Buffer): number { return rec.readFloatLE(1) }

export function recWithSessionTime(rec: Buffer, st: number): Buffer {
  const c = Buffer.from(rec)
  c.writeFloatLE(st, 1)
  return c
}

// Walk a batch and return the LAST record of each fixed-size hot type, as a tight
// copy. Used to hold-and-advance the last sample when a tick has no fresh data.
export function lastHotRecords(batch: Buffer): { tel?: Buffer; mot?: Buffer; motEx?: Buffer } {
  const out: { tel?: Buffer; mot?: Buffer; motEx?: Buffer } = {}
  let o = 0
  while (o < batch.length) {
    const tag = batch[o]
    let len: number
    if (tag === 1) len = 45         // telemetry
    else if (tag === 2) len = 29    // motion
    else if (tag === 4) len = 21    // motion_ex
    else if (tag === 3) len = 3 + batch[o + 2] * 16  // positions: tag + player_idx + n + n*(x,z)
    else break                      // unknown tag — can't know length
    if (o + len > batch.length) break
    if (tag === 1) out.tel = Buffer.from(batch.subarray(o, o + len))
    else if (tag === 2) out.mot = Buffer.from(batch.subarray(o, o + len))
    else if (tag === 4) out.motEx = Buffer.from(batch.subarray(o, o + len))
    o += len
  }
  return out
}
