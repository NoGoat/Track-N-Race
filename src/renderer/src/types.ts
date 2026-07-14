export interface TelemetryRow {
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
  slm: number   // 2026 active-aero / straight line mode (0/1), separate from drs
  tyre_temp_surface_rl: number; tyre_temp_surface_rr: number
  tyre_temp_surface_fl: number; tyre_temp_surface_fr: number
  tyre_temp_inner_rl: number;   tyre_temp_inner_rr: number
  tyre_temp_inner_fl: number;   tyre_temp_inner_fr: number
  brake_temp_rl: number; brake_temp_rr: number
  brake_temp_fl: number; brake_temp_fr: number
  engine_temp: number
}

export interface MotionRow {
  type: 'motion'
  ts: string
  session_time: number
  g_lat: number   // lateral G (+ve = right)
  g_long: number  // longitudinal G (+ve = acceleration, -ve = braking)
  g_vert: number  // vertical G
}

export interface MotionExRow {
  type: 'motion_ex'
  ts: string
  session_time: number
  front_aero_height_mm: number  // front plank edge height above road surface (mm)
  rear_aero_height_mm: number   // rear plank edge height above road surface (mm)
}

export interface LapRow {
  type: 'lap'
  ts: string
  last_lap_ms: number
  current_lap_ms: number
  s1_ms: number
  s2_ms: number
  position: number
  lap_num: number
  pit_status: number   // 0=none 1=pitting 2=in pit area
  num_pit_stops: number
  sector: number       // current sector 0/1/2
  lap_invalid: boolean
  penalties_s: number
}

export interface StatusRow {
  type: 'status'
  ts: string
  session_time: number
  fuel_mix: number         // 0=lean 1=std 2=rich 3=max
  front_brake_bias: number // %
  fuel_kg: number
  fuel_laps: number
  drs_allowed: boolean
  tyre_compound: number    // actual: 16=C5 17=C4 18=C3 19=C2 20=C1 7=Int 8=Wet
  visual_compound: number  // visual: 16=soft 17=med 18=hard 7=int 8=wet
  tyre_age_laps: number
  ers_j: number
  ers_pct: number
  ers_mode: number         // 0=none 1=auto 2=hotlap 3=overtake
  ers_deployed_j: number
  engine_power_ice_kw: number
  engine_power_mguk_kw: number
  ers_harvested_mguk_j: number
  ers_harvested_mguh_j: number
}

export interface DamageRow {
  type: 'damage'
  ts: string
  session_time: number
  tyre_wear_rl: number; tyre_wear_rr: number
  tyre_wear_fl: number; tyre_wear_fr: number
  tyre_dmg_rl: number;  tyre_dmg_rr: number
  tyre_dmg_fl: number;  tyre_dmg_fr: number
  brake_dmg_rl: number; brake_dmg_rr: number
  brake_dmg_fl: number; brake_dmg_fr: number
  blisters_rl: number; blisters_rr: number
  blisters_fl: number; blisters_fr: number
  wing_fl: number
  wing_fr: number
  wing_rear: number
  floor_damage: number
  diffuser_damage: number
  sidepod_damage: number
  drs_fault: number   // 0=OK 1=fault
  ers_fault: number   // 0=OK 1=fault
  gearbox_damage: number
  engine_damage: number
}

export interface CarPosition {
  idx: number
  x:   number
  z:   number
}

export interface PositionsMsg {
  type:       'positions'
  ts:         string
  player_idx: number
  cars:       CarPosition[]
}

export interface TimingCar {
  idx: number
  position: number
  lap_num: number
  current_lap_ms: number
  last_lap_ms: number
  s1_ms: number
  s2_ms: number
  gap_ms: number
  pit_status: number
  lap_invalid: boolean
  penalties_s: number
  num_dt_pens: number
  num_sg_pens: number
  sector: number
  result_status: number  // 0=invalid 1=inactive 2=active 3=finished 4=dnf 5=dsq 7=retired
  driver_status: number  // 0=garage 1=flying 2=inlap 3=outlap 4=ontrack
}

export interface TimingMsg {
  type: 'timing'
  ts: string
  player_idx: number
  cars: TimingCar[]
}

export interface DriverInfo {
  idx: number
  name: string
  team_id: number
  race_number: number
  ai: boolean
  livery_color: string
}

export interface ParticipantsMsg {
  type: 'participants'
  ts: string
  drivers: DriverInfo[]
}

export interface CarStatusEntry {
  idx: number
  fuel_mix: number
  front_brake_bias: number
  fuel_kg: number
  fuel_laps: number
  drs_allowed: boolean
  tyre_compound: number
  visual_compound: number
  tyre_age_laps: number
  ers_j: number
  ers_pct: number
  ers_mode: number
  ers_deployed_j: number
  engine_power_ice_kw: number
  engine_power_mguk_kw: number
  ers_harvested_mguk_j: number
  ers_harvested_mguh_j: number
}

export interface AllStatusMsg {
  type: 'all_status'
  ts: string
  cars: CarStatusEntry[]
}

export interface TyreSetEntry {
  idx: number
  actual_compound: number
  visual_compound: number
  wear: number
  available: boolean
  recommended_session: number
  life_span: number
  usable_life: number
  lap_delta_ms: number
  fitted: boolean
}

export interface TyreSetsMsg {
  type: 'tyre_sets'
  ts: string
  session_time: number
  sets: TyreSetEntry[]
  fitted_idx: number
}

export interface FastestLapMsg {
  type: 'fastest_lap'
  ts: string
  car_idx: number
  lap_time_s: number
}

export interface SessionHistoryFastestMsg {
  type: 'session_history_fastest'
  ts: string
  car_idx: number
  best_lap_time_ms: number
}

export interface RaceEventMsg {
  type: 'race_event'
  ts: string
  session_time?: number
  code: string
  car_idx?: number
  lap_time_s?: number
  safety_car_type?: number        // 1=Full 2=Virtual 3=Formation
  event_type?: number             // 0=Deployed 1=Returning 2=Returned 3=Resume
  penalty_type?: number           // 0=DT 1=SG 2=Grid 4=Time 5=Warning 6=DSQ
  infringement_type?: number      // see INFRINGEMENT_LABELS
  penalty_time_s?: number         // seconds (SG and time penalties)
  overtaking_car_idx?: number     // OVTK: car doing the overtake
  being_overtaken_car_idx?: number // OVTK: car being overtaken
  speed_kph?: number              // SPTP: speed at the speed trap
  is_overall_fastest?: boolean    // SPTP: new session fastest
  is_driver_fastest?: boolean     // SPTP: new personal fastest for this driver
}

export interface WeatherForecastSample {
  time_offset: number
  weather: number
  rain_percentage: number
}

export interface MarshalZoneInfo {
  zone_start: number   // 0.0–1.0 fraction of lap
  flag: number         // -1=invalid, 0=none, 1=green, 2=blue, 3=yellow
}

export interface SessionMsg {
  type: 'session'
  ts: string
  weather: number
  track_temp: number
  air_temp: number
  track_length_m: number
  track_id: number
  session_type: number
  total_laps: number
  session_time_left: number
  session_duration: number
  pit_speed_limit: number
  pit_stop_window_ideal_lap: number
  pit_stop_window_latest_lap: number
  pit_stop_rejoin_position: number
  num_marshal_zones: number
  marshal_zones: MarshalZoneInfo[]
  weather_forecast_samples: WeatherForecastSample[]
  safety_car_status: number
  forecast_accuracy: number
  ai_difficulty: number
  time_of_day: number
  num_safety_car_periods: number
  num_virtual_sc_periods: number
  num_red_flag_periods: number
  active_aero_track_status?: number   // 2026 SLM: 0 = Full, 1 = Partial, -1 = n/a
}

export interface LapData {
  lapNum: number
  startSessionTime: number
  endSessionTime: number
  telemetry: TelemetryRow[]
  motion: MotionRow[]
  statusHistory: StatusRow[]
}

export interface AnalyzeLapData {
  lapNum: number
  startSessionTime: number
  endSessionTime: number
  telemetry: TelemetryRow[]
  motion: MotionRow[]
  motionEx: MotionExRow[]
  statusHistory: StatusRow[]
  damageHistory: DamageRow[]
}

export interface PlaybackLapDataMsg {
  type: 'playback_lap_data'
  lapNum: number
  startSessionTime: number
  endSessionTime: number
  telemetry: TelemetryRow[]
  statusHistory: StatusRow[]
  motionHistory: MotionRow[]
  motionExHistory: MotionExRow[]
  damageHistory: DamageRow[]
}

export interface PlaybackFastestLapMsg {
  type: 'playback_fastest_lap'
  data: LapData
}

export interface PlaybackPreviousLapMsg {
  type: 'playback_previous_lap'
  data: LapData
}

export interface PlaybackSeekFlushMsg {
  type: 'playback_seek_flush'
  telemetry: TelemetryRow[]
  motion: MotionRow[]
  status: StatusRow[]
  damage: DamageRow[]
  currentLapStart: number
  lapNum: number
}

export type GatewayMsg =
  | TelemetryRow
  | MotionRow
  | MotionExRow
  | LapRow
  | StatusRow
  | DamageRow
  | TimingMsg
  | ParticipantsMsg
  | AllStatusMsg
  | FastestLapMsg
  | SessionHistoryFastestMsg
  | RaceEventMsg
  | SessionMsg
  | TyreSetsMsg
  | PositionsMsg
  | ProtocolStatusMsg
  | ProtocolWarningMsg
  | PlaybackFastestLapMsg
  | PlaybackPreviousLapMsg
  | PlaybackSeekFlushMsg
  | PlaybackLapDataMsg

export interface ProtocolCapabilities {
  gameYear:        24 | 25 | 26 | null  // null = no packets received yet
  hasBlisters:     boolean
  hasLiveryColors: boolean
  hasLapPositions: boolean
  hasMguh:         boolean
}

export interface ColorRuleSpec {
  on:    string
  op:    'lt' | 'lte' | 'gt' | 'gte' | 'eq'
  value: number
  color: string
}
export interface ColorSpec {
  default: string
  rules:   ColorRuleSpec[]
}

export interface ProtocolStatusMsg {
  type:            'protocol_status'
  detected_format: 2024 | 2025 | 2026 | null
  active_format:   2024 | 2025 | 2026 | null
  override:        'auto' | 'f1_24' | 'f1_25' | 'f1_26'
  capabilities:    ProtocolCapabilities
  labels?:         Record<string, string>
  cardColors?:     Record<string, ColorSpec>
  aero_mode?:      'drs' | 'slm'   // overtaking-aid mode for the active format
}

export interface ProtocolWarningMsg {
  type:            'protocol_warning'
  detected_format: 2024 | 2025 | 2026
  forced_format:   2024 | 2025 | 2026
}

declare global {
  interface Window {
    windowControls: {
      minimize:   () => void
      maximize:   () => void
      close:      () => void
      fullscreen: () => void
      minimizeToTray: () => void
      onMaximizeChange:   (cb: (isMaximized: boolean) => void) => () => void
      onFullscreenChange: (cb: (isFullscreen: boolean) => void) => () => void
    }
    udpBridge: {
      restart: () => Promise<{ ok: boolean; error?: string }>
    }
    protocolBridge: {
      getConfig:   () => Promise<{ override: string; detected: number | null; lastDetected: number | null; active: number | null }>
      setOverride: (value: 'auto' | 'f1_24' | 'f1_25' | 'f1_26') => void
      requestStatus: () => void
    }
    fsBridge: {
      selectDirectory: () => Promise<string | null>
      selectTNRDFile: () => Promise<string | null>
    }
    playerBridge: {
      load: (filePath: string) => Promise<boolean>
      play: () => void
      pause: () => void
      seek: (pct: number) => void
      setSpeed: (mult: number) => void
      getLapData: (lapNum: number) => void
      close: () => void
      exportXlsx: () => Promise<{ ok: boolean; error?: string }>
      onExportProgress: (cb: (pct: number, stage: string) => void) => () => void
      onStateChange: (cb: (state: any) => void) => () => void
      onRequestOpenConfirm: (cb: (filePath: string) => void) => () => void
    }

  }
}

declare module 'react' {
  interface CSSProperties {
    WebkitAppRegion?: 'drag' | 'no-drag'
  }
}
