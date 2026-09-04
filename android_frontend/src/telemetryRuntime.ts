import { forEachDecodedBinaryRow } from '../../electron-frontend/src/renderer/src/lib/decodeBinaryBatch'
import type { DecodedHotRow } from '../../electron-frontend/src/renderer/src/lib/decodeBinaryBatch'
import { Telemetry, type SourceStateEvent } from './native'

export interface DashboardState {
  speedKph: number
  rpm: number
  revLightsPercent: number | null
  revLightsBitValue: number | null
  gear: number
  throttle: number
  brake: number
  steering: number
  drs: number
  slm: number
  engineTemp: number
  position: number
  lapNumber: number
  totalLaps: number
  currentLapMs: number
  lastLapMs: number
  lapInvalid: boolean
  ersPercent: number
  fuelLaps: number
  brakeBias: number
  tyreCompound: number
  tyreAgeLaps: number
  aeroMode: 'drs' | 'slm'
  protocolYear: number | null
  formula: number | null
  labels: Record<string, string>
  hotRowsReceived: number
  coldRowsReceived: number
  streamGaps: number
  overflowBytes: number
  revision: number
}

export const dashboardState: DashboardState = {
  speedKph: 0,
  rpm: 0,
  revLightsPercent: null,
  revLightsBitValue: null,
  gear: 0,
  throttle: 0,
  brake: 0,
  steering: 0,
  drs: 0,
  slm: 0,
  engineTemp: 0,
  position: 0,
  lapNumber: 0,
  totalLaps: 0,
  currentLapMs: 0,
  lastLapMs: 0,
  lapInvalid: false,
  ersPercent: 0,
  fuelLaps: 0,
  brakeBias: 0,
  tyreCompound: 0,
  tyreAgeLaps: 0,
  aeroMode: 'drs',
  protocolYear: null,
  formula: null,
  labels: {},
  hotRowsReceived: 0,
  coldRowsReceived: 0,
  streamGaps: 0,
  overflowBytes: 0,
  revision: 0
}

const sourceListeners = new Set<(state: SourceStateEvent) => void>()
let lastBinarySequence = 0

function acceptHotRow(row: DecodedHotRow): void {
  dashboardState.hotRowsReceived++
  if (row.type !== 'telemetry') return
  dashboardState.speedKph = row.speed_kph
  dashboardState.rpm = row.rpm
  dashboardState.revLightsPercent = row.rev_lights_pct ?? null
  dashboardState.revLightsBitValue = row.rev_lights_bit_value ?? null
  dashboardState.gear = row.gear
  dashboardState.throttle = row.throttle
  dashboardState.brake = row.brake
  dashboardState.steering = row.steering
  dashboardState.drs = row.drs
  dashboardState.slm = row.slm
  dashboardState.engineTemp = row.engine_temp
  dashboardState.revision++
}

function acceptBinaryEnvelope(data: ArrayBuffer): void {
  if (data.byteLength < 12) return
  const bytes = new Uint8Array(data)
  if (bytes[0] !== 0x54 || bytes[1] !== 0x4e || bytes[2] !== 0x52 || bytes[3] !== 0x42) return
  const view = new DataView(data)
  const sequence = view.getUint32(4, true)
  const overflow = view.getUint32(8, true)
  if (lastBinarySequence !== 0 && sequence !== (lastBinarySequence + 1) >>> 0) {
    dashboardState.streamGaps++
  }
  lastBinarySequence = sequence
  dashboardState.overflowBytes += overflow
  forEachDecodedBinaryRow(bytes.subarray(12), acceptHotRow)
}

function integerOrNull(value: unknown): number | null {
  if (value == null) return null
  const number = Number(value)
  return Number.isInteger(number) ? number : null
}

function applyProtocolContext(
  protocolYearValue: unknown,
  formulaValue: unknown,
  fallbackAeroMode?: unknown,
): void {
  const protocolYear = integerOrNull(protocolYearValue)
  const formula = integerOrNull(formulaValue)
  dashboardState.protocolYear = protocolYear
  dashboardState.formula = formula

  if (protocolYear === 2026) {
    // F1 26 is Formula 13. Until the first Session packet supplies m_formula,
    // retain the 2026 default used by the shared parser and Electron app.
    dashboardState.aeroMode = formula === null || formula === 13 ? 'slm' : 'drs'
  } else if (protocolYear !== null) {
    dashboardState.aeroMode = 'drs'
  } else {
    dashboardState.aeroMode = fallbackAeroMode === 'slm' ? 'slm' : 'drs'
  }
}

function acceptColdRow(json: string): void {
  try {
    const row = JSON.parse(json) as Record<string, unknown>
    dashboardState.coldRowsReceived++
    switch (row.type) {
      case 'lap':
        dashboardState.position = Number(row.position ?? 0)
        dashboardState.lapNumber = Number(row.lap_num ?? 0)
        dashboardState.currentLapMs = Number(row.current_lap_ms ?? 0)
        dashboardState.lastLapMs = Number(row.last_lap_ms ?? 0)
        dashboardState.lapInvalid = Boolean(row.lap_invalid)
        dashboardState.revision++
        break
      case 'status':
        dashboardState.ersPercent = Math.round(Number(row.ers_pct ?? 0))
        dashboardState.fuelLaps = Number(row.fuel_laps ?? 0)
        dashboardState.brakeBias = Number(row.front_brake_bias ?? 0)
        dashboardState.tyreCompound = Number(row.visual_compound ?? 0)
        dashboardState.tyreAgeLaps = Number(row.tyre_age_laps ?? 0)
        dashboardState.revision++
        break
      case 'session':
        dashboardState.totalLaps = Number(row.total_laps ?? 0)
        dashboardState.revision++
        break
      case 'protocol_context':
        // Prevent labels from the previous source/session overriding the new
        // year/formula decision before its full protocol_status row arrives.
        dashboardState.labels = {}
        applyProtocolContext(row.protocol_year, row.formula)
        dashboardState.revision++
        break
      case 'protocol_status':
        applyProtocolContext(
          row.active_format ?? row.detected_format,
          row.formula,
          row.aero_mode,
        )
        if (row.labels && typeof row.labels === 'object' && !Array.isArray(row.labels)) {
          const labels: Record<string, string> = {}
          for (const [key, value] of Object.entries(row.labels)) {
            if (typeof value === 'string') labels[key] = value
          }
          dashboardState.labels = labels
        }
        dashboardState.revision++
        break
      case 'recording_error':
        publishSource({
          state: 'error',
          detail: `Recording ${String(row.operation ?? 'save')} failed: ${String(row.message ?? 'Unknown error')}`
        })
        break
    }
  } catch {
    // Future or malformed control rows must not interrupt the stream.
  }
}

function acceptColdBatch(batch: string): void {
  let start = 0
  while (start < batch.length) {
    let end = batch.indexOf('\n', start)
    if (end === -1) end = batch.length
    if (end > start) acceptColdRow(batch.slice(start, end))
    start = end + 1
  }
}

function decodeBase64(value: string): ArrayBuffer {
  const decoded = atob(value)
  const bytes = new Uint8Array(decoded.length)
  for (let i = 0; i < decoded.length; i++) bytes[i] = decoded.charCodeAt(i)
  return bytes.buffer
}

function publishSource(state: SourceStateEvent): void {
  for (const listener of sourceListeners) listener(state)
}

export function subscribeSourceState(listener: (state: SourceStateEvent) => void): () => void {
  sourceListeners.add(listener)
  return () => sourceListeners.delete(listener)
}

export async function installTelemetryRuntime(): Promise<() => void> {
  const onWindowMessage = (event: MessageEvent<unknown>) => {
    if (event.data instanceof ArrayBuffer) acceptBinaryEnvelope(event.data)
    else if (typeof event.data === 'string') acceptColdBatch(event.data)
  }
  window.addEventListener('message', onWindowMessage)

  const handles = await Promise.all([
    Telemetry.addListener('telemetryRow', event => acceptColdRow(event.row)),
    Telemetry.addListener('telemetryBinaryFallback', event => acceptBinaryEnvelope(decodeBase64(event.base64))),
    Telemetry.addListener('sourceState', publishSource)
  ])

  return () => {
    window.removeEventListener('message', onWindowMessage)
    for (const handle of handles) void handle.remove()
  }
}
