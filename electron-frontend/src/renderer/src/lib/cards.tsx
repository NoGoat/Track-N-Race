import { createContext, useContext, useMemo, type ReactNode } from 'react'
import type { TelemetryRow, StatusRow, LapRow, DamageRow, SessionMsg, ColorSpec } from '../types'
import { evalColorToken, tokenColor } from './cardColors'

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

// ── Overview resolvers (LiveStats) ───────────────────────────────────────────
export const OVERVIEW_RESOLVERS: Record<string, CardResolver> = {
  speed: c => ({ value: c.latest ? String(c.latest.speed_kph) : '—', unit: 'kph', color: c.color('speed') }),
  rpm:   c => ({ value: c.latest ? c.latest.rpm.toLocaleString() : '—', color: c.color('rpm') }),
  gear:  c => {
    const g = c.latest?.gear ?? 0
    return { value: g === 0 ? 'N' : g < 0 ? 'R' : String(g), color: c.color('gear', g) }
  },
  throttle: c => ({ value: c.latest ? String(Math.round(c.latest.throttle * 100)) : '—', unit: '%', color: c.color('throttle') }),
  brake:    c => ({ value: c.latest ? String(Math.round(c.latest.brake * 100)) : '—', unit: '%', color: c.color('brake') }),
  // Wing card — two keys share one behaviour, each reading its own field.
  drs: c => ({ value: c.latest?.drs ? 'ON' : 'OFF', color: c.color('wing', c.latest?.drs ?? 0),
               sub: c.damage?.drs_fault === 1 ? 'FAULT' : undefined, subColor: '#C4162A' }),
  slm: c => ({ value: c.latest?.slm ? 'ON' : 'OFF', color: c.color('wing', c.latest?.slm ?? 0) }),
  engine: c => ({ value: c.latest ? String(c.latest.engine_temp) : '—', unit: '°C', color: c.color('engine', c.latest?.engine_temp ?? NaN) }),
  ers: c => ({
    value: c.status ? c.status.ers_pct.toFixed(0) : '—',
    unit: c.status ? '%' : undefined,
    color: c.color('ers', c.status?.ers_pct ?? NaN),
    sub: c.damage?.ers_fault === 1 ? 'FAULT' : c.status ? c.tn('ers.mode', c.status.ers_mode) : undefined,
    subColor: c.damage?.ers_fault === 1 ? '#C4162A' : undefined,
  }),
  fuel: c => ({
    value: c.status ? c.status.fuel_kg.toFixed(1) : '—',
    unit: c.status ? 'kg' : undefined,
    color: c.color('fuel'),
    sub: c.status ? `${c.status.fuel_laps >= 0 ? '+' : ''}${c.status.fuel_laps.toFixed(1)} vs fin` : undefined,
  }),
  pos: c => ({ value: c.lap ? `P${c.lap.position}` : '—', sub: c.lap ? `Lap ${c.lap.lap_num}` : undefined }),
  tyre: c => ({
    value: c.status ? COMPOUND_LABEL(c.tn, c.status.tyre_compound) : '—',
    color: c.color('tyre'),
    sub: c.status ? `${c.status.tyre_age_laps}L · ${FUEL_MIX[c.status.fuel_mix] ?? ''}` : undefined,
  }),
}

// ── Power resolvers (PowerStatsBar) ──────────────────────────────────────────
function powerVals(c: CardCtx) {
  const ice = c.status?.engine_power_ice_kw ?? 0
  const mguk = c.status?.engine_power_mguk_kw ?? 0
  const total = ice + mguk
  const ersPct = c.status?.ers_pct ?? 0
  return { ice, mguk, total, ersPct, ersMJ: (ersPct / 100) * 4,
           iceSplit: total > 0 ? (ice / total * 100) : 0, ersSplit: total > 0 ? (mguk / total * 100) : 0 }
}
export const POWER_RESOLVERS: Record<string, CardResolver> = {
  totalPower: c => { const v = powerVals(c); return { value: c.status ? v.total.toFixed(0) : '—', unit: c.status ? 'kW' : undefined, color: c.color('power.total', v.total) } },
  ice:        c => { const v = powerVals(c); return { value: c.status ? v.ice.toFixed(0) : '—',   unit: c.status ? 'kW' : undefined, color: c.color('power.ice') } },
  mguk:       c => { const v = powerVals(c); return { value: c.status ? v.mguk.toFixed(0) : '—',  unit: c.status ? 'kW' : undefined, color: c.color('power.mguk') } },
  split:      c => { const v = powerVals(c); return { value: c.status ? `${v.iceSplit.toFixed(0)}:${v.ersSplit.toFixed(0)}` : '—', sub: undefined, color: c.color('power.split') } },
  ersStore:   c => { const v = powerVals(c); return { value: c.status ? v.ersMJ.toFixed(2) : '—',  unit: c.status ? 'MJ' : undefined, color: c.color('power.ers', v.ersPct) } },
  ersPct:     c => { const v = powerVals(c); return { value: c.status ? v.ersPct.toFixed(0) : '—', unit: c.status ? '%' : undefined,  color: c.color('power.ers', v.ersPct) } },
  fuel:       c => ({ value: c.status ? (c.status.fuel_kg.toFixed(1)) : '—', unit: c.status ? 'kg' : undefined, color: c.color('power.fuel') }),
}
