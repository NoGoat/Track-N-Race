import { HEADER_SIZE, PacketHeader } from './packetHeader'

// Packet IDs
export const PID_MOTION       = 0
export const PID_SESSION      = 1
export const PID_LAP_DATA     = 2
export const PID_EVENT        = 3
export const PID_PARTICIPANTS = 4
export const PID_CAR_TEL      = 6
export const PID_CAR_STATUS   = 7
export const PID_CAR_DAMAGE      = 10
export const PID_SESSION_HISTORY = 11
export const PID_TYRE_SETS       = 12
export const PID_MOTION_EX       = 13

// Per-car record sizes
const MOTION_SIZE    = 60
const LAP_SIZE       = 57
const PART_SIZE      = 57
const TEL_SIZE       = 60
const STATUS_SIZE    = 55
const DAMAGE_SIZE    = 46
const TYRE_SET_SIZE  = 10

// Packet 13 — MotionEx offsets
const MOTION_EX_FRONT_AERO_OFFSET = 217
const MOTION_EX_REAR_AERO_OFFSET  = 221

// Packet 1 — safetyCarStatus offset
const SESSION_SC_OFFSET = 153

export const FRAME_SAMPLED = new Set([PID_MOTION, PID_CAR_TEL, PID_MOTION_EX])

export const SLOW_RATE_MS: Record<number, number> = {
  [PID_SESSION]:      0,
  [PID_LAP_DATA]:     500,
  [PID_CAR_STATUS]:   500,
  [PID_CAR_DAMAGE]:   500,
  [PID_PARTICIPANTS]: 5000,
  [PID_EVENT]:        0,
}

type Row = Record<string, unknown>
type Parser = (data: Buffer, hdr: PacketHeader) => Row[]

export const PARSERS: Record<number, Parser[]> = {
  [PID_MOTION]:       [motion, allMotion],
  [PID_SESSION]:      [session],
  [PID_LAP_DATA]:     [lap, timing],
  [PID_EVENT]:        [event],
  [PID_PARTICIPANTS]: [participants],
  [PID_CAR_TEL]:      [telemetry],
  [PID_CAR_STATUS]:   [status, allStatus],
  [PID_CAR_DAMAGE]:      [damage],
  [PID_SESSION_HISTORY]: [sessionHistory],
  [PID_TYRE_SETS]:       [tyreSets],
  [PID_MOTION_EX]:       [motionEx],
}

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

function now(): string {
  return new Date().toISOString()
}

function round1(v: number): number {
  return Math.round(v * 10) / 10
}

function round3(v: number): number {
  return Math.round(v * 1000) / 1000
}

/** Returns a Buffer positioned at the player car's data offset, or null if too short. */
function carBuf(data: Buffer, playerIdx: number, carSize: number): { buf: Buffer; off: number } | null {
  const base = HEADER_SIZE + playerIdx * carSize
  if (data.length < base + carSize) return null
  return { buf: data, off: base }
}

// -----------------------------------------------------------------------
// Packet 1: Session
// -----------------------------------------------------------------------
function session(data: Buffer, _hdr: PacketHeader): Row[] {
  if (data.length < 708) return []

  const weather         = data[29]
  const trackTemp       = data.readInt8(30)
  const airTemp         = data.readInt8(31)
  const totalLaps       = data[32]
  const trackLengthM    = data.readUInt16LE(33)
  const sessionType     = data[35]
  const trackId         = data.readInt8(36)
  const sessionTimeLeft = data.readUInt16LE(38)
  const sessionDuration = data.readUInt16LE(40)
  const pitSpeedLimit   = data[42]
  const numMarshalZones = data[47]

  const marshalZones: Row[] = []
  for (let i = 0; i < numMarshalZones && i < 21; i++) {
    const o = 48 + i * 5
    marshalZones.push({ zone_start: data.readFloatLE(o), flag: data.readInt8(o + 4) })
  }

  const safetyCarStatus = data[SESSION_SC_OFFSET]  // 153

  const numForecastSamples = data[155]
  const weatherForecastSamples: Row[] = []
  for (let i = 0; i < numForecastSamples && i < 64; i++) {
    const o = 156 + i * 8
    weatherForecastSamples.push({
      time_offset:      data[o + 1],
      weather:          data[o + 2],
      rain_percentage:  data[o + 7],
    })
  }

  const forecastAccuracy    = data[668]
  const aiDifficulty        = data[669]
  const pitStopWindowIdealLap  = data[682]
  const pitStopWindowLatestLap = data[683]
  const pitStopRejoinPosition  = data[684]
  const timeOfDay           = data.readUInt32LE(696)  // minutes since midnight
  const numSafetyCarPeriods    = data[705]
  const numVirtualScPeriods    = data[706]
  const numRedFlagPeriods      = data[707]

  return [{
    type: 'session', ts: now(),
    weather, track_temp: trackTemp, air_temp: airTemp,
    track_length_m: trackLengthM,
    track_id: trackId, session_type: sessionType,
    total_laps: totalLaps, session_time_left: sessionTimeLeft,
    session_duration: sessionDuration, pit_speed_limit: pitSpeedLimit,
    pit_stop_window_ideal_lap: pitStopWindowIdealLap,
    pit_stop_window_latest_lap: pitStopWindowLatestLap,
    pit_stop_rejoin_position: pitStopRejoinPosition,
    num_marshal_zones: numMarshalZones, marshal_zones: marshalZones,
    weather_forecast_samples: weatherForecastSamples,
    safety_car_status: safetyCarStatus,
    forecast_accuracy: forecastAccuracy,
    ai_difficulty: aiDifficulty,
    time_of_day: timeOfDay,
    num_safety_car_periods: numSafetyCarPeriods,
    num_virtual_sc_periods: numVirtualScPeriods,
    num_red_flag_periods: numRedFlagPeriods,
  }]
}

// -----------------------------------------------------------------------
// Packet 6: Car Telemetry
// -----------------------------------------------------------------------
function telemetry(data: Buffer, hdr: PacketHeader): Row[] {
  const c = carBuf(data, hdr.playerCarIndex, TEL_SIZE)
  if (!c) return []
  let o = c.off
  const b = c.buf

  const speed  = b.readUInt16LE(o); o += 2
  const throt  = b.readFloatLE(o);  o += 4
  const steer  = b.readFloatLE(o);  o += 4
  const brake  = b.readFloatLE(o);  o += 4
  o += 1                            // clutch skip
  const gear   = b.readInt8(o);     o += 1
  const rpm    = b.readUInt16LE(o); o += 2
  const drs    = b[o];              o += 1
  o += 1                            // revLightsPct skip
  o += 2                            // revLightsBitField skip
  // 4× uint16 brake temps [RL RR FL FR]
  const btRL = b.readUInt16LE(o); o += 2
  const btRR = b.readUInt16LE(o); o += 2
  const btFL = b.readUInt16LE(o); o += 2
  const btFR = b.readUInt16LE(o); o += 2
  // 4× uint8 surface temps
  const stRL = b[o++], stRR = b[o++], stFL = b[o++], stFR = b[o++]
  // 4× uint8 inner temps
  const itRL = b[o++], itRR = b[o++], itFL = b[o++], itFR = b[o++]
  const engT = b.readUInt16LE(o)

  return [{
    type: 'telemetry', ts: now(), session_time: hdr.sessionTime,
    speed_kph: speed, rpm, gear,
    throttle: throt, brake, steering: Math.round(steer * 10000) / 10000, drs,
    tyre_temp_surface_rl: stRL, tyre_temp_surface_rr: stRR,
    tyre_temp_surface_fl: stFL, tyre_temp_surface_fr: stFR,
    tyre_temp_inner_rl:   itRL, tyre_temp_inner_rr:   itRR,
    tyre_temp_inner_fl:   itFL, tyre_temp_inner_fr:   itFR,
    brake_temp_rl: btRL, brake_temp_rr: btRR,
    brake_temp_fl: btFL, brake_temp_fr: btFR,
    engine_temp: engT,
  }]
}

// -----------------------------------------------------------------------
// Packet 13: MotionEx — front/rear aero heights
// -----------------------------------------------------------------------
function motionEx(data: Buffer, hdr: PacketHeader): Row[] {
  if (data.length < MOTION_EX_REAR_AERO_OFFSET + 4) return []
  const frontAero = data.readFloatLE(MOTION_EX_FRONT_AERO_OFFSET)
  const rearAero  = data.readFloatLE(MOTION_EX_REAR_AERO_OFFSET)
  return [{
    type: 'motion_ex', ts: now(), session_time: hdr.sessionTime,
    front_aero_height_mm: Math.round(frontAero * 1000 * 100) / 100,
    rear_aero_height_mm:  Math.round(rearAero  * 1000 * 100) / 100,
  }]
}

// -----------------------------------------------------------------------
// Packet 0: Motion — g-forces
// -----------------------------------------------------------------------
function motion(data: Buffer, hdr: PacketHeader): Row[] {
  const base = HEADER_SIZE + hdr.playerCarIndex * MOTION_SIZE
  if (data.length < base + MOTION_SIZE) return []
  // skip worldPos(12)+worldVel(12)+fwdDir(6)+rightDir(6) = 36 bytes
  const o = base + 36
  const gLat  = data.readFloatLE(o)
  const gLong = data.readFloatLE(o + 4)
  const gVert = data.readFloatLE(o + 8)
  return [{
    type: 'motion', ts: now(), session_time: hdr.sessionTime,
    g_lat: round3(gLat), g_long: round3(gLong), g_vert: round3(gVert),
  }]
}

// -----------------------------------------------------------------------
// Packet 0: Motion — all-car world positions (x, z only)
// -----------------------------------------------------------------------
function allMotion(data: Buffer, hdr: PacketHeader): Row[] {
  if (data.length < HEADER_SIZE + 22 * MOTION_SIZE) return []
  const cars: Row[] = []
  for (let i = 0; i < 22; i++) {
    const base = HEADER_SIZE + i * MOTION_SIZE
    const x = data.readFloatLE(base)
    const z = data.readFloatLE(base + 8)
    cars.push({ idx: i, x: Math.round(x * 100) / 100, z: Math.round(z * 100) / 100 })
  }
  return [{ type: 'positions', ts: now(), player_idx: hdr.playerCarIndex, cars }]
}

// -----------------------------------------------------------------------
// Packet 2: Lap (player only)
// -----------------------------------------------------------------------
function lap(data: Buffer, hdr: PacketHeader): Row[] {
  const c = carBuf(data, hdr.playerCarIndex, LAP_SIZE)
  if (!c) return []
  let o = c.off
  const b = c.buf

  const lastLap = b.readUInt32LE(o); o += 4
  const curLap  = b.readUInt32LE(o); o += 4
  const s1H = b.readUInt16LE(o); o += 2; const s1M = b[o++]
  const s2H = b.readUInt16LE(o); o += 2; const s2M = b[o++]
  o += 3  // skip t[6-7]
  o += 3  // skip t[8-9] gap
  o += 12 // skip 3 floats t[10-12]
  const position  = b[o++]
  const lapNum    = b[o++]
  const pitStatus = b[o++]
  const numPits   = b[o++]
  const sector    = b[o++]
  const invalid   = b[o++] !== 0
  const penaltiesS = b[o]

  return [{
    type: 'lap', ts: now(), session_time: hdr.sessionTime,
    last_lap_ms:    lastLap,
    current_lap_ms: curLap,
    s1_ms: s1M * 60000 + s1H,
    s2_ms: s2M * 60000 + s2H,
    position, lap_num: lapNum,
    pit_status: pitStatus, num_pit_stops: numPits,
    sector, lap_invalid: invalid, penalties_s: penaltiesS,
  }]
}

// -----------------------------------------------------------------------
// Packet 2: Timing tower (all 22 cars)
// -----------------------------------------------------------------------
function timing(data: Buffer, hdr: PacketHeader): Row[] {
  if (data.length < HEADER_SIZE + 22 * LAP_SIZE) return []
  const cars: Row[] = []

  for (let i = 0; i < 22; i++) {
    let o = HEADER_SIZE + i * LAP_SIZE
    const b = data

    const lastLap = b.readUInt32LE(o); o += 4
    const curLap  = b.readUInt32LE(o); o += 4
    const s1H = b.readUInt16LE(o); o += 2; const s1M = b[o++]
    const s2H = b.readUInt16LE(o); o += 2; const s2M = b[o++]
    o += 3                                               // skip t[6-7]
    const gapH = b.readUInt16LE(o); o += 2; const gapM = b[o++]  // t[8-9]
    o += 12                                              // skip 3 floats t[10-12]
    const position    = b[o++]
    const lapNum      = b[o++]
    const pitStatus   = b[o++]
    o += 1                          // numPitStops skip
    const sector      = b[o++]
    const invalid     = b[o++] !== 0
    const penaltiesS  = b[o++]
    o += 2                          // skip t[20-21]
    const numDtPens   = b[o++]
    const numSgPens   = b[o++]
    o += 1                          // skip t[24]
    const driverStatus = b[o++]
    const resultStatus = b[o]

    cars.push({
      idx: i, position, lap_num: lapNum,
      current_lap_ms: curLap, last_lap_ms: lastLap,
      s1_ms: s1M * 60000 + s1H,
      s2_ms: s2M * 60000 + s2H,
      gap_ms: gapM * 60000 + gapH,
      pit_status: pitStatus, lap_invalid: invalid,
      penalties_s: penaltiesS, num_dt_pens: numDtPens,
      num_sg_pens: numSgPens, sector,
      result_status: resultStatus, driver_status: driverStatus,
    })
  }
  return [{ type: 'timing', ts: now(), session_time: hdr.sessionTime, player_idx: hdr.playerCarIndex, cars }]
}

// -----------------------------------------------------------------------
// Packet 7: Car Status (player)
// -----------------------------------------------------------------------
function status(data: Buffer, hdr: PacketHeader): Row[] {
  const c = carBuf(data, hdr.playerCarIndex, STATUS_SIZE)
  if (!c) return []
  let o = c.off
  const b = c.buf

  o += 2                              // skip t[0-1]
  const fuelMix    = b[o++]
  const brakeBias  = b[o++]
  o += 1                              // skip t[4]
  const fuelKg     = b.readFloatLE(o); o += 4
  o += 4                              // skip t[6]
  const fuelLaps   = b.readFloatLE(o); o += 4
  o += 2                              // H skip t[8]
  o += 2                              // H skip t[9]
  o += 1                              // B skip t[10]
  const drsAllowed  = b[o++] !== 0
  o += 2                              // H skip t[12]
  const tyreCompound  = b[o++]
  const visualCompound = b[o++]
  const tyreAgeLaps   = b[o++]
  o += 1                              // skip vehicleFiaFlags
  const enginePowerICE    = b.readFloatLE(o); o += 4
  const enginePowerMGUK   = b.readFloatLE(o); o += 4
  const ersJ              = b.readFloatLE(o); o += 4
  const ersMode           = b[o++]
  const ersHarvestedMGUK  = b.readFloatLE(o); o += 4
  const ersHarvestedMGUH  = b.readFloatLE(o); o += 4
  const ersDeployedJ      = b.readFloatLE(o)

  const ersPct = Math.round(ersJ / 4_000_000 * 100 * 10) / 10

  return [{
    type: 'status', ts: now(), session_time: hdr.sessionTime,
    fuel_mix:              fuelMix,
    front_brake_bias:      brakeBias,
    fuel_kg:               Math.round(fuelKg * 100) / 100,
    fuel_laps:             Math.round(fuelLaps * 10) / 10,
    drs_allowed:           drsAllowed,
    tyre_compound:         tyreCompound,
    visual_compound:       visualCompound,
    tyre_age_laps:         tyreAgeLaps,
    ers_j:                 Math.round(ersJ),
    ers_pct:               ersPct,
    ers_mode:              ersMode,
    ers_deployed_j:        Math.round(ersDeployedJ),
    engine_power_ice_kw:   Math.round(enginePowerICE  / 1000 * 10) / 10,
    engine_power_mguk_kw:  Math.round(enginePowerMGUK / 1000 * 10) / 10,
    ers_harvested_mguk_j:  Math.round(ersHarvestedMGUK),
    ers_harvested_mguh_j:  Math.round(ersHarvestedMGUH),
  }]
}

// -----------------------------------------------------------------------
// Packet 7: All-car status
// -----------------------------------------------------------------------
function allStatus(data: Buffer, hdr: PacketHeader): Row[] {
  if (data.length < HEADER_SIZE + 22 * STATUS_SIZE) return []
  const cars: Row[] = []

  for (let i = 0; i < 22; i++) {
    let o = HEADER_SIZE + i * STATUS_SIZE
    const b = data

    o += 2
    const fuelMix      = b[o++]
    const brakeBias    = b[o++]
    o += 1
    const fuelKg       = b.readFloatLE(o); o += 4
    o += 4
    const fuelLaps     = b.readFloatLE(o); o += 4
    o += 2                              // H skip
    o += 2                              // H skip
    o += 1                              // B skip
    const drsAllowed   = b[o++] !== 0
    o += 2                              // H skip
    const tyreCompound  = b[o++]
    const visualCompound = b[o++]
    const tyreAgeLaps   = b[o++]
    o += 1                              // skip vehicleFiaFlags
    const enginePowerICE   = b.readFloatLE(o); o += 4
    const enginePowerMGUK  = b.readFloatLE(o); o += 4
    const ersJ             = b.readFloatLE(o); o += 4
    const ersMode          = b[o++]
    const ersHarvestedMGUK = b.readFloatLE(o); o += 4
    const ersHarvestedMGUH = b.readFloatLE(o); o += 4
    const ersDeployedJ     = b.readFloatLE(o)

    const ersPct = Math.round(ersJ / 4_000_000 * 100 * 10) / 10
    cars.push({
      idx: i,
      fuel_mix:             fuelMix,
      front_brake_bias:     brakeBias,
      fuel_kg:              Math.round(fuelKg * 100) / 100,
      fuel_laps:            Math.round(fuelLaps * 10) / 10,
      drs_allowed:          drsAllowed,
      tyre_compound:        tyreCompound,
      visual_compound:      visualCompound,
      tyre_age_laps:        tyreAgeLaps,
      ers_j:                Math.round(ersJ),
      ers_pct:              ersPct,
      ers_mode:             ersMode,
      ers_deployed_j:       Math.round(ersDeployedJ),
      engine_power_ice_kw:  Math.round(enginePowerICE  / 1000 * 10) / 10,
      engine_power_mguk_kw: Math.round(enginePowerMGUK / 1000 * 10) / 10,
      ers_harvested_mguk_j: Math.round(ersHarvestedMGUK),
      ers_harvested_mguh_j: Math.round(ersHarvestedMGUH),
    })
  }
  return [{ type: 'all_status', ts: now(), session_time: hdr.sessionTime, cars }]
}

// -----------------------------------------------------------------------
// Packet 10: Car Damage
// -----------------------------------------------------------------------
function damage(data: Buffer, hdr: PacketHeader): Row[] {
  const c = carBuf(data, hdr.playerCarIndex, DAMAGE_SIZE)
  if (!c) return []
  let o = c.off
  const b = c.buf

  // 4× float tyre wear [RL RR FL FR]
  const wRL = b.readFloatLE(o); o += 4
  const wRR = b.readFloatLE(o); o += 4
  const wFL = b.readFloatLE(o); o += 4
  const wFR = b.readFloatLE(o); o += 4
  o += 8  // skip 4B tyre_damage% + 4B detached
  // 4B blisters [RL RR FL FR]
  const blRL = b[o++], blRR = b[o++], blFL = b[o++], blFR = b[o++]
  // 18B damage fields
  const wingFL      = b[o++]
  const wingFR      = b[o++]
  const wingRear    = b[o++]
  const floorDmg    = b[o++]
  const carpetDmg   = b[o++]
  const diffuserDmg = b[o++]
  const gearboxDmg  = b[o++]
  const engineDmg   = b[o]

  return [{
    type: 'damage', ts: now(), session_time: hdr.sessionTime,
    tyre_wear_rl: round1(wRL), tyre_wear_rr: round1(wRR),
    tyre_wear_fl: round1(wFL), tyre_wear_fr: round1(wFR),
    blisters_rl: blRL, blisters_rr: blRR, blisters_fl: blFL, blisters_fr: blFR,
    wing_fl: wingFL, wing_fr: wingFR, wing_rear: wingRear,
    floor_damage: floorDmg, carpet_damage: carpetDmg,
    diffuser_damage: diffuserDmg, gearbox_damage: gearboxDmg, engine_damage: engineDmg,
  }]
}

// -----------------------------------------------------------------------
// Packet 4: Participants
// -----------------------------------------------------------------------
function participants(data: Buffer, _hdr: PacketHeader): Row[] {
  if (data.length < HEADER_SIZE + 1 + 22 * PART_SIZE) return []
  const drivers: Row[] = []

  for (let i = 0; i < 22; i++) {
    let o = HEADER_SIZE + 1 + i * PART_SIZE
    const b = data

    const ai      = b[o++] !== 0
    o += 1                              // driverId skip
    o += 1                              // networkId skip
    const teamId  = b[o++]
    o += 1                              // myTeam skip
    const raceNum = b[o++]
    o += 1                              // nationality skip
    const nameStart = o
    const name = b.slice(nameStart, nameStart + 32).toString('utf8').split('\0')[0].trim()
    if (!name) continue
    const numColors  = b[nameStart + 37]
    const r = b[nameStart + 38], g = b[nameStart + 39], bv = b[nameStart + 40]
    const livery_color = numColors > 0
      ? `#${r.toString(16).padStart(2, '0')}${g.toString(16).padStart(2, '0')}${bv.toString(16).padStart(2, '0')}`
      : '#8e8e8e'
    drivers.push({ idx: i, name, team_id: teamId, race_number: raceNum, ai, livery_color })
  }
  return [{ type: 'participants', ts: now(), drivers }]
}

// -----------------------------------------------------------------------
// Packet 3: Event codes
// -----------------------------------------------------------------------
function event(data: Buffer, hdr: PacketHeader): Row[] {
  if (data.length < HEADER_SIZE + 4) return []
  const code = data.toString('ascii', HEADER_SIZE, HEADER_SIZE + 4)
  const base: Row = { type: 'race_event', ts: now(), session_time: hdr.sessionTime, code }
  const o = HEADER_SIZE + 4

  switch (code) {
    case 'FTLP': {
      if (data.length < o + 5) return []
      const vehicleIdx = data[o]
      const lapTimeS = Math.round(data.readFloatLE(o + 1) * 1000) / 1000
      return [
        { type: 'fastest_lap', ts: now(), car_idx: vehicleIdx, lap_time_s: lapTimeS },
        { ...base, car_idx: vehicleIdx, lap_time_s: lapTimeS },
      ]
    }
    case 'DRSE': case 'DRSD': case 'RDFL': case 'CHQF': case 'LGOT':
    case 'SSTA': case 'SEND':
      return [base]

    case 'SCAR': {
      if (data.length < o + 2) return []
      const scType = data[o]
      const evType = data[o + 1]
      if (scType === 0) return []
      return [{ ...base, safety_car_type: scType, event_type: evType }]
    }
    case 'RTMT': case 'RCWN': {
      if (data.length < o + 1) return []
      return [{ ...base, car_idx: data[o] }]
    }
    case 'PENA': {
      if (data.length < o + 7) return []
      const penType         = data[o]
      const infringementType = data[o + 1]
      const vehicleIdx      = data[o + 2]
      const timeS           = data[o + 4]
      return [{ ...base, car_idx: vehicleIdx, penalty_type: penType, infringement_type: infringementType, penalty_time_s: timeS }]
    }
    case 'DTSV': case 'SGSV': {
      if (data.length < o + 1) return []
      return [{ ...base, car_idx: data[o] }]
    }
    case 'OVTK': case 'SPTP':
      return []
    default:
      return []
  }
}

// -----------------------------------------------------------------------
// Packet 12: TyreSets — full weekend allocation (player car only)
// -----------------------------------------------------------------------
function tyreSets(data: Buffer, hdr: PacketHeader): Row[] {
  if (data.length < 231) return []
  const carIdx = data[HEADER_SIZE]
  if (carIdx !== hdr.playerCarIndex) return []

  const sets: Row[] = []
  for (let i = 0; i < 20; i++) {
    const o = HEADER_SIZE + 1 + i * TYRE_SET_SIZE
    sets.push({
      idx:                  i,
      actual_compound:      data[o],
      visual_compound:      data[o + 1],
      wear:                 data[o + 2],
      available:            data[o + 3] === 1,
      recommended_session:  data[o + 4],
      life_span:            data[o + 5],
      usable_life:          data[o + 6],
      lap_delta_ms:         data.readInt16LE(o + 7),
      fitted:               data[o + 9] === 1,
    })
  }
  const fitted_idx = data[230]
  return [{ type: 'tyre_sets', ts: now(), session_time: hdr.sessionTime, sets, fitted_idx }]
}

// -----------------------------------------------------------------------
// Packet 11: SessionHistory — fallback fastest lap source
// -----------------------------------------------------------------------
const SESSION_HISTORY_LAP_SIZE = 14  // bytes per LapHistoryData entry
const SESSION_HISTORY_LAP_OFFSET = HEADER_SIZE + 7  // header + 7 uint8 fields before lap array

function sessionHistory(data: Buffer, _hdr: PacketHeader): Row[] {
  const carIdx        = data[HEADER_SIZE]
  const bestLapNum    = data[HEADER_SIZE + 3]  // m_bestLapTimeLapNum (1-indexed, 0 = not set)
  if (bestLapNum === 0) return []

  const lapOff = SESSION_HISTORY_LAP_OFFSET + (bestLapNum - 1) * SESSION_HISTORY_LAP_SIZE
  if (data.length < lapOff + SESSION_HISTORY_LAP_SIZE) return []

  const lapValidBitFlags = data[lapOff + 13]
  if ((lapValidBitFlags & 0x01) === 0) return []

  const bestLapTimeMs = data.readUInt32LE(lapOff)
  return [{ type: 'session_history_fastest', ts: now(), car_idx: carIdx, best_lap_time_ms: bestLapTimeMs }]
}
