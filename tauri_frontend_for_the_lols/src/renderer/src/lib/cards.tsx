import { createContext, useContext, useMemo, type ReactNode } from 'react'
import type { TelemetryRow, StatusRow, LapRow, DamageRow, SessionMsg, ColorSpec } from '../types'
import { evalColorToken, tokenColor } from './cardColors'
import { tyreCompoundColor } from './tyreCompounds'

// Key-driven card model. A card is a { key, label } pair: the label (format-aware,
// from the i18n catalog) is the title; the key selects a resolver that extracts +
// formats the value/unit/sub from the current data, and a colour from the shared
// library colour spec (see lib/cardColors.ts + tnrp/CardColors.h). This decouples
// "what the card is called" from "what data it shows" — so the wing card reads
// `drs` under 2025 and `slm` under 2026 while everything else stays identical.

// ── Colour spec context (fed from protocol_status.cardColors) ────────────────
const CardColorsContext = createContext<Record<string, ColorSpec>>({})

export function CardColorsProvider({
  specs,
  children,
}: {
  specs?: Record<string, ColorSpec> | null
  children: ReactNode
}): React.JSX.Element {
  const value = useMemo(() => specs ?? {}, [specs])
  return <CardColorsContext.Provider value={value}>{children}</CardColorsContext.Provider>
}

export interface CardView {
  value: string
  unit?: string
  color?: string
  sub?: string
  subColor?: string
}

export interface CardCtx {
  latest: TelemetryRow | null
  status: StatusRow | null
  lap: LapRow | null
  damage: DamageRow | null
  session: SessionMsg | null
  isDark: boolean
  t: (key: string) => string
  tn: (group: string, n: number) => string
  // Resolve a colour: evaluates the named spec against the data + `self` value.
  color: (specKey: string, self?: number) => string | undefined
}

export type CardResolver = (c: CardCtx) => CardView
// `key` selects the resolver + data field; `vis` (defaults to key) is the
// visibility flag — they differ for the wing card (key drs/slm, vis 'drs').
export interface CardDesc { key: string; label: string; vis?: string }

// Builds the colour function bound to the current data + theme.
export function useColorFn(
  latest: TelemetryRow | null,
  status: StatusRow | null,
  isDark: boolean,
): (specKey: string, self?: number) => string | undefined {
  const specs = useContext(CardColorsContext)
  return useMemo(() => {
    const fields: Record<string, number> = {
      engine_temp:     latest?.engine_temp     ?? NaN,
      brake:           latest?.brake           ?? NaN,
      ers_mode:        status?.ers_mode        ?? NaN,
      ers_pct:         status?.ers_pct         ?? NaN,
      fuel_laps:       status?.fuel_laps       ?? NaN,
      visual_compound: status?.visual_compound ?? NaN,
    }
    return (specKey: string, self = NaN) =>
      tokenColor(evalColorToken(specs[specKey], fields, self), isDark)
  }, [specs, latest, status, isDark])
}

const COMPOUND_LABEL = (tn: CardCtx['tn'], c: number) => tn('tyre.actual', c)
const FUEL_MIX = ['Lean', 'Std', 'Rich', 'Max']
const MISSING = '-'
const finite = (value: unknown): value is number => typeof value === 'number' && Number.isFinite(value)

// ── Overview resolvers (LiveStats) ───────────────────────────────────────────
export const OVERVIEW_RESOLVERS: Record<string, CardResolver> = {
  speed: c => ({ value: finite(c.latest?.speed_kph) ? String(c.latest.speed_kph) : MISSING,
    unit: finite(c.latest?.speed_kph) ? 'kph' : undefined, color: c.color('speed') }),
  rpm:   c => {
    const rpm = c.latest?.rpm
    return { value: finite(rpm) ? rpm.toLocaleString() : MISSING, color: c.color('rpm') }
  },
  gear:  c => {
    const g = c.latest?.gear
    return finite(g)
      ? { value: g === 0 ? 'N' : g < 0 ? 'R' : String(g), color: c.color('gear', g) }
      : { value: MISSING }
  },
  throttle: c => ({ value: finite(c.latest?.throttle) ? String(Math.round(c.latest.throttle * 100)) : MISSING,
    unit: finite(c.latest?.throttle) ? '%' : undefined, color: c.color('throttle') }),
  brake: c => ({ value: finite(c.latest?.brake) ? String(Math.round(c.latest.brake * 100)) : MISSING,
    unit: finite(c.latest?.brake) ? '%' : undefined, color: c.color('brake') }),
  // Wing card — two keys share one behaviour, each reading its own field.
  drs: c => ({ value: finite(c.latest?.drs) ? (c.latest.drs ? 'ON' : 'OFF') : MISSING,
               color: c.color('wing', c.latest?.drs ?? NaN),
               sub: c.damage?.drs_fault === 1 ? 'FAULT' : undefined, subColor: '#C4162A' }),
  slm: c => ({ value: finite(c.latest?.slm) ? (c.latest.slm ? 'ON' : 'OFF') : MISSING,
               color: c.color('wing', c.latest?.slm ?? NaN) }),
  engine: c => ({ value: finite(c.latest?.engine_temp) ? String(c.latest.engine_temp) : MISSING,
    unit: finite(c.latest?.engine_temp) ? '°C' : undefined, color: c.color('engine', c.latest?.engine_temp ?? NaN) }),
  ers: c => ({
    value: finite(c.status?.ers_pct) ? c.status.ers_pct.toFixed(0) : MISSING,
    unit: finite(c.status?.ers_pct) ? '%' : undefined,
    color: c.color('ers', c.status?.ers_pct ?? NaN),
    sub: c.damage?.ers_fault === 1 ? 'FAULT' : finite(c.status?.ers_mode) ? c.tn('ers.mode', c.status.ers_mode) : undefined,
    subColor: c.damage?.ers_fault === 1 ? '#C4162A' : undefined,
  }),
  fuel: c => ({
    value: finite(c.status?.fuel_kg) ? c.status.fuel_kg.toFixed(1) : MISSING,
    unit: finite(c.status?.fuel_kg) ? 'kg' : undefined,
    color: c.color('fuel'),
    sub: finite(c.status?.fuel_laps) ? `${c.status.fuel_laps >= 0 ? '+' : ''}${c.status.fuel_laps.toFixed(1)} vs fin` : undefined,
  }),
  pos: c => ({ value: finite(c.lap?.position) ? `P${c.lap.position}` : MISSING,
    sub: finite(c.lap?.lap_num) ? `Lap ${c.lap.lap_num}` : undefined }),
  tyre: c => ({
    value: finite(c.status?.tyre_compound) ? COMPOUND_LABEL(c.tn, c.status.tyre_compound) : MISSING,
    color: finite(c.status?.tyre_compound) && finite(c.status?.visual_compound)
      ? tyreCompoundColor(c.status.tyre_compound, c.status.visual_compound) ?? c.color('tyre') : c.color('tyre'),
    sub: finite(c.status?.tyre_age_laps)
      ? `${c.status.tyre_age_laps}L${finite(c.status?.fuel_mix) ? ` · ${FUEL_MIX[c.status.fuel_mix] ?? ''}` : ''}` : undefined,
  }),
}

// ── Power resolvers (PowerStatsBar) ──────────────────────────────────────────
function powerVals(c: CardCtx) {
  const ice = finite(c.status?.engine_power_ice_kw) ? c.status.engine_power_ice_kw : NaN
  const mguk = finite(c.status?.engine_power_mguk_kw) ? c.status.engine_power_mguk_kw : NaN
  const total = finite(ice) && finite(mguk) ? ice + mguk : NaN
  const ersPct = finite(c.status?.ers_pct) ? c.status.ers_pct : NaN
  return { ice, mguk, total, ersPct, ersMJ: finite(ersPct) ? (ersPct / 100) * 4 : NaN,
           iceSplit: total > 0 ? (ice / total * 100) : NaN, ersSplit: total > 0 ? (mguk / total * 100) : NaN }
}
export const POWER_RESOLVERS: Record<string, CardResolver> = {
  totalPower: c => { const v = powerVals(c); return { value: finite(v.total) ? v.total.toFixed(0) : MISSING, unit: finite(v.total) ? 'kW' : undefined, color: c.color('power.total', v.total) } },
  ice:        c => { const v = powerVals(c); return { value: finite(v.ice) ? v.ice.toFixed(0) : MISSING, unit: finite(v.ice) ? 'kW' : undefined, color: c.color('power.ice') } },
  mguk:       c => { const v = powerVals(c); return { value: finite(v.mguk) ? v.mguk.toFixed(0) : MISSING, unit: finite(v.mguk) ? 'kW' : undefined, color: c.color('power.mguk') } },
  split:      c => { const v = powerVals(c); return { value: finite(v.iceSplit) && finite(v.ersSplit) ? `${v.iceSplit.toFixed(0)}:${v.ersSplit.toFixed(0)}` : MISSING, sub: undefined, color: c.color('power.split') } },
  ersStore:   c => { const v = powerVals(c); return { value: finite(v.ersMJ) ? v.ersMJ.toFixed(2) : MISSING, unit: finite(v.ersMJ) ? 'MJ' : undefined, color: c.color('power.ers', v.ersPct) } },
  ersPct:     c => { const v = powerVals(c); return { value: finite(v.ersPct) ? v.ersPct.toFixed(0) : MISSING, unit: finite(v.ersPct) ? '%' : undefined, color: c.color('power.ers', v.ersPct) } },
  fuel:       c => ({ value: finite(c.status?.fuel_kg) ? c.status.fuel_kg.toFixed(1) : MISSING, unit: finite(c.status?.fuel_kg) ? 'kg' : undefined, color: c.color('power.fuel') }),
}
