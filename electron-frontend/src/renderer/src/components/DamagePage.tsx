import { memo } from 'react'
import { useTelemetryStore } from '../stores/telemetryStore'
import { StatCard } from './LiveStats'
import { useLabels } from '../lib/labels'
import carSvgSource from '../assets/car/f1-car-wireframe.svg?raw'
import type { DensityMode } from '../lib/graphSections'
import type { DamageRow } from '../types'

const missing = '—'

// The shared wireframe (also used by Qt), placed inline so its zones can be styled by id.
// Its viewBox is offset into this page's drawing by CAR_X/CAR_Y; callouts sit around it.
const CAR_INNER = carSvgSource.replace(/^[\s\S]*?<svg[^>]*>/, '').replace(/<\/svg>\s*$/, '')
const CAR_X = 350
const CAR_Y = 70

type Severity = 'na' | 'ok' | 'warn' | 'crit'
type DamageKey = Extract<keyof DamageRow, string>

const num = (row: DamageRow | null, key: DamageKey): number | null => {
  const value = row?.[key]
  return typeof value === 'number' && Number.isFinite(value) ? value : null
}
// Body damage: any damage is worth seeing; above 20% it costs real performance.
const damageSeverity = (v: number | null): Severity => v === null ? 'na' : v === 0 ? 'ok' : v <= 20 ? 'warn' : 'crit'
// Power-unit and gearbox wear accumulate normally, so only high wear is a warning.
const wearSeverity = (v: number | null): Severity => v === null ? 'na' : v < 50 ? 'ok' : v < 75 ? 'warn' : 'crit'
// Tyre, brake and blister values use a five-band scale, 20% per band.
type TyreBand = 'na' | 'green' | 'greenYellow' | 'yellow' | 'yellowRed' | 'red'
const tyreBand = (v: number | null): TyreBand => v === null ? 'na'
  : v < 20 ? 'green' : v < 40 ? 'greenYellow' : v < 60 ? 'yellow' : v < 80 ? 'yellowRed' : 'red'
function tyrePalette(isDark: boolean): Record<TyreBand, string> {
  return {
    na: 'var(--text-muted)',
    green: isDark ? '#37872D' : '#137333',
    greenYellow: isDark ? '#9BBF2E' : '#5E7A12',
    yellow: isDark ? '#E0A800' : '#8a6500',
    yellowRed: isDark ? '#F2711C' : '#B84A08',
    red: '#C4162A',
  }
}

function palette(isDark: boolean): Record<Severity, string> {
  return {
    na: 'var(--text-muted)',
    ok: isDark ? '#37872D' : '#137333',
    warn: isDark ? '#E0A800' : '#8a6500',
    crit: '#C4162A',
  }
}

const BODY_PARTS: { key: DamageKey; label: string; zones: string[]; value: string }[] = [
  { key: 'wing_fl', label: 'Wing L', zones: ['z-fw-l'], value: 'v-fw-l' },
  { key: 'wing_fr', label: 'Wing R', zones: ['z-fw-r'], value: 'v-fw-r' },
  { key: 'wing_rear', label: 'Rear wing', zones: ['z-rw'], value: 'v-rw' },
  { key: 'floor_damage', label: 'Floor', zones: ['z-floor'], value: 'v-floor' },
  { key: 'sidepod_damage', label: 'Sidepods', zones: ['z-sp-l', 'z-sp-r'], value: 'v-sp' },
  { key: 'diffuser_damage', label: 'Diffuser', zones: ['z-diff'], value: 'v-diff' },
]

const WHEELS = [
  { id: 'fl', tyre: 'tyre_dmg_fl', brake: 'brake_dmg_fl', blisters: 'blisters_fl', color: '#e10600', lightColor: '#e10600', tx: 1320, ty: 56, anchor: 'start', cols: [1320, 1405, 1490] },
  { id: 'fr', tyre: 'tyre_dmg_fr', brake: 'brake_dmg_fr', blisters: 'blisters_fr', color: '#4488ff', lightColor: '#0B57D0', tx: 1320, ty: 456, anchor: 'start', cols: [1320, 1405, 1490] },
  { id: 'rl', tyre: 'tyre_dmg_rl', brake: 'brake_dmg_rl', blisters: 'blisters_rl', color: '#37872D', lightColor: '#137333', tx: 300, ty: 126, anchor: 'end', cols: [50, 140, 230] },
  { id: 'rr', tyre: 'tyre_dmg_rr', brake: 'brake_dmg_rr', blisters: 'blisters_rr', color: '#ffd700', lightColor: '#765900', tx: 300, ty: 406, anchor: 'end', cols: [50, 140, 230] },
] as const

// ICE combustion engine, MGU-H heat / MGU-K kinetic motor-generators, ES energy store,
// CE control electronics, TC turbocharger.
const PU_PARTS: { key: DamageKey; name: string }[] = [
  { key: 'engine_ice_wear', name: 'ICE' },
  { key: 'engine_mguh_wear', name: 'MGU-H' },
  { key: 'engine_mguk_wear', name: 'MGU-K' },
  { key: 'engine_es_wear', name: 'ES' },
  { key: 'engine_ce_wear', name: 'CE' },
  { key: 'engine_tc_wear', name: 'TC' },
]
// engine_damage / gearbox_damage are reported as "damage" but behave as wear.
const WEAR_TILES: { key: DamageKey; name: string }[] = [
  ...PU_PARTS,
  { key: 'engine_damage', name: 'Engine' },
  { key: 'gearbox_damage', name: 'Gearbox' },
]

// Leader lines (page drawing units), each ending in a dot on its part. Front tyres use
// elbows so the line clears the front wing.
const LEADERS: [string, number, number][] = [
  ['M310 120 H500', 500, 120], ['M310 240 H400', 400, 240], ['M310 320 H500', 500, 320], ['M310 400 H500', 500, 400],
  ['M944 72 V112', 944, 112], ['M794 452 V368', 794, 368], ['M1310 50 H1084 V112', 1084, 112],
  ['M1310 190 H1240', 1240, 190], ['M1310 320 H1240', 1240, 320], ['M1310 450 H1084 V398', 1084, 398],
]
const LABELS: { value: string; label: string; x: number; y: number; anchor: 'start' | 'middle' | 'end' }[] = [
  { value: 'v-rw', label: 'REAR WING', x: 300, y: 232, anchor: 'end' },
  { value: 'v-diff', label: 'DIFFUSER', x: 300, y: 312, anchor: 'end' },
  { value: 'v-floor', label: 'FLOOR', x: 944, y: 30, anchor: 'middle' },
  { value: 'v-sp', label: 'SIDEPODS', x: 794, y: 476, anchor: 'middle' },
  { value: 'v-fw-l', label: 'WING L', x: 1320, y: 182, anchor: 'start' },
  { value: 'v-fw-r', label: 'WING R', x: 1320, y: 312, anchor: 'start' },
]

function CarDiagram({ damage, isDark }: { damage: DamageRow | null; isDark: boolean }) {
  const colors = palette(isDark)
  const tyreColors = tyrePalette(isDark)
  const pct = (v: number | null) => v === null ? missing : `${v}%`
  // Zone styling is generated as CSS by id: it overrides the asset's presentation
  // attributes and keeps the inline SVG markup itself static.
  const rules: string[] = [
    `.damage-car .wire { stroke: ${isDark ? '#8a93c4' : '#3d434b'}; }`,
    '.damage-car .solid { fill: var(--bg-panel); }',
    `.damage-car .tube { stroke: ${isDark ? '#4a5175' : '#8f9397'}; }`,
  ]
  // A part with no data is drawn like an undamaged one;
  // its callout shows the missing value.
  const zone = (id: string, sev: Severity) => {
    if (sev === 'warn' || sev === 'crit')
      rules.push(`.damage-car #${id} { stroke: ${colors[sev]}; stroke-width: 3.2; }`,
        `.damage-car #${id} .solid { fill: color-mix(in srgb, ${colors[sev]} 12%, var(--bg-panel)); }`)
  }
  for (const part of BODY_PARTS) for (const id of part.zones) zone(id, damageSeverity(num(damage, part.key)))
  for (const wheel of WHEELS) {
    const values = [num(damage, wheel.tyre), num(damage, wheel.brake), num(damage, wheel.blisters)]
    const known = values.filter((v): v is number => v !== null)
    // Tyre outlines always take the band colour of the worst wheel value, green included.
    const band = known.length === 0 ? 'na' : tyreBand(Math.max(...known))
    if (band !== 'na')
      rules.push(`.damage-car #t-${wheel.id} { stroke: ${tyreColors[band]}; stroke-width: 3.2; }`,
        `.damage-car #t-${wheel.id} .solid { fill: color-mix(in srgb, ${tyreColors[band]} 12%, var(--bg-panel)); }`)
  }

  return <svg className="damage-car block w-full h-full" viewBox="0 0 1580 520" role="img" aria-label="Car damage by part">
    <style>{rules.join('\n')}</style>
    <g transform={`translate(${CAR_X} ${CAR_Y})`} dangerouslySetInnerHTML={{ __html: CAR_INNER }} />
    <g stroke="var(--text-muted)" strokeWidth="1.5" fill="none">
      {LEADERS.map(([d]) => <path key={d} d={d} />)}
    </g>
    <g fill="var(--text-dim)">
      {LEADERS.map(([d, x, y]) => <circle key={d} cx={x} cy={y} r="4" />)}
    </g>
    {LABELS.map(label => {
      const part = BODY_PARTS.find(p => p.value === label.value)!
      const v = num(damage, part.key)
      return <g key={label.value} fontFamily="inherit">
        <text x={label.x} y={label.y} textAnchor={label.anchor} fontSize="14" letterSpacing="1.4" fill="var(--text-secondary)">{label.label}</text>
        <text x={label.x} y={label.y + 34} textAnchor={label.anchor} fontSize="26" fontWeight="700" fill={colors[damageSeverity(v)]}>{pct(v)}</text>
      </g>
    })}
    {WHEELS.map(wheel => {
      const cells: [string, number | null][] = [['TYRE', num(damage, wheel.tyre)], ['BRAKE', num(damage, wheel.brake)], ['BLISTERS', num(damage, wheel.blisters)]]
      return <g key={wheel.id} fontFamily="inherit">
        <text x={wheel.tx} y={wheel.ty} textAnchor={wheel.anchor} fontSize="16" fontWeight="700" letterSpacing="1.6"
          fill={isDark ? wheel.color : wheel.lightColor}>{wheel.id.toUpperCase()}</text>
        {cells.map(([label, v], i) => <g key={label}>
          <text x={wheel.cols[i]} y={wheel.ty + 26} fontSize="13" letterSpacing="1" fill="var(--text-secondary)">{label}</text>
          <text x={wheel.cols[i]} y={wheel.ty + 50} fontSize="20" fontWeight="700" fill={tyreColors[tyreBand(v)]}>{pct(v)}</text>
        </g>)}
      </g>
    })}
  </svg>
}

// The page follows the streamed driver: the player live, the driver selector's car in
// V6 playback. Everything comes from the latest damage row.
export default memo(function DamagePage({ isDark, compact }: { isDark: boolean; compact: DensityMode }) {
  const damage = useTelemetryStore(s => s.damage)
  const { t } = useLabels()
  const colors = palette(isDark)
  const pctText = (v: number | null) => v === null ? missing : String(v)

  const engine = num(damage, 'engine_damage')
  const gearbox = num(damage, 'gearbox_damage')
  const flag = (key: DamageKey) => num(damage, key)
  const drs = flag('drs_fault'), ers = flag('ers_fault')
  const blown = flag('engine_blown'), seized = flag('engine_seized')
  // The game sets blown/seized for some failures only (e.g. not an MGU-H failure), so
  // overall engine wear at 100% without either flag reads as FAIL.
  const engineStatus = seized ? 'SEIZED' : blown ? 'BLOWN' : engine !== null && engine >= 100 ? 'FAIL'
    : seized === null && blown === null ? null : 'OK'

  const fault = (v: number | null) => v === null ? missing : v ? 'FAULT' : 'OK'
  const faultColor = (v: number | null) => v === null ? undefined : v ? colors.crit : colors.ok

  return <div className="h-full min-h-0 overflow-hidden bg-[var(--bg-panel)] border-t border-[var(--border)] flex flex-col">
    <div className="shrink-0 flex divide-x divide-[var(--border)] border-b border-[var(--border)]">
      {[
        { label: 'Engine', value: pctText(engine), unit: '%', color: engine === null ? undefined : colors[wearSeverity(engine)], sub: 'Overall wear' },
        { label: 'Gearbox', value: pctText(gearbox), unit: '%', color: gearbox === null ? undefined : colors[wearSeverity(gearbox)], sub: 'Wear' },
        // m_drsFault: titled DRS, or Rear Wing for F1 26 sessions (format catalog, like the Overview wing card).
        { label: t('ui.damage.wing_fault'), value: fault(drs), color: faultColor(drs), sub: 'Fault flag' },
        { label: 'ERS', value: fault(ers), color: faultColor(ers), sub: 'Fault flag' },
        { label: 'Engine status', value: engineStatus ?? missing, color: engineStatus === null ? undefined : engineStatus === 'OK' ? colors.ok : colors.crit, sub: 'Blown / seized / fail' },
      ].map(card => <StatCard key={card.label} label={card.label} value={card.value} unit={card.value === missing ? undefined : card.unit}
        textColor={card.color} sub={compact === 'spacious' ? card.sub : undefined} compact={compact} />)}
    </div>
    <div className="flex-1 min-h-0 px-3 pt-3 pb-2" aria-label="Car damage">
      <CarDiagram damage={damage} isDark={isDark} />
    </div>
    {/* One row of Overview stat cards, laid out like the Overview stats row. */}
    <div className="shrink-0 flex divide-x divide-[var(--border)] border-t border-[var(--border)]" aria-label="Power unit and gearbox wear">
      {WEAR_TILES.map(tile => {
        const value = num(damage, tile.key)
        return <StatCard key={tile.key} label={`${tile.name} (Wear)`} value={pctText(value)} unit={value === null ? undefined : '%'}
          textColor={value === null ? undefined : colors[wearSeverity(value)]} compact={compact} />
      })}
    </div>
  </div>
})
