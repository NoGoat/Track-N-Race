// Keep these wire-row shapes local. Importing the renderer-wide `types.ts`
// makes this otherwise standalone decoder depend on React's module graph,
// which prevents non-Electron consumers (the Capacitor app) from sharing it.
interface TelemetryRow {
  type: 'telemetry'
  ts: string
  session_time: number
  speed_kph: number
  rpm: number
  gear: number
  throttle: number
  brake: number
  steering: number
  drs: number
  rev_lights_pct?: number
  rev_lights_bit_value?: number
  slm: number
  tyre_temp_surface_rl: number
  tyre_temp_surface_rr: number
  tyre_temp_surface_fl: number
  tyre_temp_surface_fr: number
  tyre_temp_inner_rl: number
  tyre_temp_inner_rr: number
  tyre_temp_inner_fl: number
  tyre_temp_inner_fr: number
  brake_temp_rl: number
  brake_temp_rr: number
  brake_temp_fl: number
  brake_temp_fr: number
  engine_temp: number
}

interface MotionRow {
  type: 'motion'
  ts: string
  session_time: number
  g_lat: number
  g_long: number
  g_vert: number
}

interface MotionExRow {
  type: 'motion_ex'
  ts: string
  session_time: number
  front_aero_height_mm: number
  rear_aero_height_mm: number
}

interface PositionsMsg {
  type: 'positions'
  ts: string
  player_idx: number
  cars: Array<{ idx: number; x: number; z: number }>
}

// Decoder for the hot-row binary wire format produced by the native engine.
// MUST stay in lockstep with protocol_parser_library/include/tnrp/BinaryRows.h.
//
// A batch is a concatenation of records, each `u8 tag` + fixed little-endian
// fields. `ts` is not transmitted (the live charts key off session_time). The
// decoded objects intentionally match the JSON row shapes so they can be fed
// straight through the existing message handlers.

const TAG_TELEMETRY = 1
const TAG_MOTION = 2
const TAG_POSITIONS = 3
const TAG_MOTION_EX = 4
const REV_LIGHTS_PERCENT_UNAVAILABLE = 0xff
const REV_LIGHTS_BIT_VALUE_UNAVAILABLE = 0xffff

export type DecodedHotRow = TelemetryRow | MotionRow | MotionExRow | PositionsMsg

// Decode at most maxRows starting at a record boundary and return the next byte
// offset. Seek backfills call this repeatedly with a bounded row count so the
// renderer can yield between chunks; ordinary live batches use the wrapper
// below and remain a single tight loop.
export function decodeBinaryBatchRange(
  batch: Uint8Array | ArrayBuffer,
  visit: (row: DecodedHotRow) => void,
  byteOffset = 0,
  maxRows = Number.POSITIVE_INFINITY,
): number {
  const u8 = batch instanceof Uint8Array ? batch : new Uint8Array(batch)
  const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength)
  const len = u8.byteLength
  let o = Math.max(0, Math.min(len, byteOffset))
  let decoded = 0

  while (o < len && decoded < maxRows) {
    const tag = dv.getUint8(o); o += 1
    switch (tag) {
      case TAG_TELEMETRY: {
        const session_time = dv.getFloat32(o, true); o += 4
        const speed_kph = dv.getUint16(o, true); o += 2
        const rpm = dv.getUint16(o, true); o += 2
        const gear = dv.getInt8(o); o += 1
        const drs = dv.getUint8(o); o += 1
        const rawRevLightsPercent = dv.getUint8(o); o += 1
        const rawRevLightsBitValue = dv.getUint16(o, true); o += 2
        const rev_lights_pct = rawRevLightsPercent === REV_LIGHTS_PERCENT_UNAVAILABLE
          ? undefined
          : rawRevLightsPercent
        const rev_lights_bit_value = rawRevLightsBitValue === REV_LIGHTS_BIT_VALUE_UNAVAILABLE
          ? undefined
          : rawRevLightsBitValue
        const throttle = dv.getFloat32(o, true); o += 4
        const brake = dv.getFloat32(o, true); o += 4
        const steering = dv.getFloat64(o, true); o += 8
        const tsrl = dv.getUint8(o++), tsrr = dv.getUint8(o++), tsfl = dv.getUint8(o++), tsfr = dv.getUint8(o++)
        const tirl = dv.getUint8(o++), tirr = dv.getUint8(o++), tifl = dv.getUint8(o++), tifr = dv.getUint8(o++)
        const brl = dv.getUint16(o, true); o += 2
        const brr = dv.getUint16(o, true); o += 2
        const bfl = dv.getUint16(o, true); o += 2
        const bfr = dv.getUint16(o, true); o += 2
        const engine_temp = dv.getUint16(o, true); o += 2
        const slm = dv.getUint8(o); o += 1
        visit({
          type: 'telemetry', ts: '', session_time, speed_kph, rpm, gear, throttle, brake, steering,
          drs, rev_lights_pct, rev_lights_bit_value, slm,
          tyre_temp_surface_rl: tsrl, tyre_temp_surface_rr: tsrr, tyre_temp_surface_fl: tsfl, tyre_temp_surface_fr: tsfr,
          tyre_temp_inner_rl: tirl, tyre_temp_inner_rr: tirr, tyre_temp_inner_fl: tifl, tyre_temp_inner_fr: tifr,
          brake_temp_rl: brl, brake_temp_rr: brr, brake_temp_fl: bfl, brake_temp_fr: bfr, engine_temp,
        })
        break
      }
      case TAG_MOTION: {
        const session_time = dv.getFloat32(o, true); o += 4
        const g_lat = dv.getFloat64(o, true); o += 8
        const g_long = dv.getFloat64(o, true); o += 8
        const g_vert = dv.getFloat64(o, true); o += 8
        visit({ type: 'motion', ts: '', session_time, g_lat, g_long, g_vert })
        break
      }
      case TAG_POSITIONS: {
        const player_idx = dv.getUint8(o); o += 1
        const n = dv.getUint8(o); o += 1
        const cars = new Array(n)
        for (let i = 0; i < n; i++) {
          const x = dv.getFloat64(o, true); o += 8
          const z = dv.getFloat64(o, true); o += 8
          cars[i] = { idx: i, x, z }
        }
        visit({ type: 'positions', ts: '', player_idx, cars })
        break
      }
      case TAG_MOTION_EX: {
        const session_time = dv.getFloat32(o, true); o += 4
        const front_aero_height_mm = dv.getFloat64(o, true); o += 8
        const rear_aero_height_mm = dv.getFloat64(o, true); o += 8
        visit({ type: 'motion_ex', ts: '', session_time, front_aero_height_mm, rear_aero_height_mm })
        break
      }
      default:
        // Unknown tag: record length is unknown, so we can't safely continue.
        return len
    }
    decoded++
  }
  return o
}

export function forEachDecodedBinaryRow(
  batch: Uint8Array | ArrayBuffer,
  visit: (row: DecodedHotRow) => void,
): void {
  decodeBinaryBatchRange(batch, visit)
}

export function decodeBinaryBatch(batch: Uint8Array | ArrayBuffer): DecodedHotRow[] {
  const rows: DecodedHotRow[] = []
  forEachDecodedBinaryRow(batch, row => rows.push(row))
  return rows
}
