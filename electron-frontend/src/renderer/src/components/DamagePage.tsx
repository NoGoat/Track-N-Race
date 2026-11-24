import { memo } from 'react'
import { useTelemetryStore } from '../stores/telemetryStore'
import { StatCard } from './LiveStats'
import { useLabels } from '../lib/labels'
import carSvgSource from '../assets/car/f1-car-wireframe.svg?raw'
import type { DensityMode } from '../lib/graphSections'
import type { DamageRow } from '../types'
import type { DamageLayout } from '../app/appConfig'
import FileTableCards, { type FileTableCard, type FileTableValue } from './FileTableCards'
import { useSecondaryCards, type FileHeaderCell } from '../lib/secondaryLive'
import { useSecondaryState } from '../lib/secondaryStream'

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
type WearTileId = keyof DamageLayout['wearTiles']
const PU_PARTS: { id: WearTileId; key: DamageKey; name: string }[] = [
  { id: 'ice', key: 'engine_ice_wear', name: 'ICE' },
  { id: 'mguh', key: 'engine_mguh_wear', name: 'MGU-H' },
  { id: 'mguk', key: 'engine_mguk_wear', name: 'MGU-K' },
  { id: 'es', key: 'engine_es_wear', name: 'ES' },
  { id: 'ce', key: 'engine_ce_wear', name: 'CE' },
  { id: 'tc', key: 'engine_tc_wear', name: 'TC' },
]
// engine_damage / gearbox_damage are reported as "damage" but behave as wear.
const WEAR_TILES: { id: WearTileId; key: DamageKey; name: string }[] = [
  ...PU_PARTS,
  { id: 'engine', key: 'engine_damage', name: 'Engine' },
  { id: 'gearbox', key: 'gearbox_damage', name: 'Gearbox' },
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

/**
 * The wireframe's zone styling, as CSS by id under `scope` (one class per drawn
 * car, so two cars on the page don't style each other): it overrides the
 * asset's presentation attributes and keeps the inline SVG markup static.
 */
function carRules(scope: string, damage: DamageRow | null, isDark: boolean): string[] {
  const colors = palette(isDark)
  const tyreColors = tyrePalette(isDark)
  const rules: string[] = [
    `.${scope} .wire { stroke: ${isDark ? '#8a93c4' : '#3d434b'}; }`,
    `.${scope} .solid { fill: var(--bg-panel); }`,
    `.${scope} .tube { stroke: ${isDark ? '#4a5175' : '#8f9397'}; }`,
  ]
  // A part with no data is drawn like an undamaged one;
  // its callout shows the missing value.
  const zone = (id: string, sev: Severity) => {
    if (sev === 'warn' || sev === 'crit')
      rules.push(`.${scope} #${id} { stroke: ${colors[sev]}; stroke-width: 3.2; }`,
        `.${scope} #${id} .solid { fill: color-mix(in srgb, ${colors[sev]} 12%, var(--bg-panel)); }`)
  }
  for (const part of BODY_PARTS) for (const id of part.zones) zone(id, damageSeverity(num(damage, part.key)))
  for (const wheel of WHEELS) {
    const values = [num(damage, wheel.tyre), num(damage, wheel.brake), num(damage, wheel.blisters)]
    const known = values.filter((v): v is number => v !== null)
    // Tyre outlines always take the band colour of the worst wheel value, green included.
    const band = known.length === 0 ? 'na' : tyreBand(Math.max(...known))
    if (band !== 'na')
      rules.push(`.${scope} #t-${wheel.id} { stroke: ${tyreColors[band]}; stroke-width: 3.2; }`,
        `.${scope} #t-${wheel.id} .solid { fill: color-mix(in srgb, ${tyreColors[band]} 12%, var(--bg-panel)); }`)
  }
  return rules
}

type WheelId = typeof WHEELS[number]['id']
type Anchor = 'start' | 'middle' | 'end'
/** Where a car diagram puts the car and its callouts, in its own drawing units. */
interface DiagramLayout {
  viewBox: string
  carX: number; carY: number
  leaders: [string, number, number][]
  labels: { value: string; label: string; x: number; y: number; anchor: Anchor }[]
  wheels: Record<WheelId, { tx: number; ty: number; anchor: Anchor; cols: readonly number[] }>
  // Font sizes and the offsets of a value under its label.
  partLabel: number; partValue: number; partGap: number
  wheelTitle: number; cellLabel: number; cellValue: number; cellLabelGap: number; cellValueGap: number
}

const FULL_LAYOUT: DiagramLayout = {
  viewBox: '0 0 1580 520', carX: CAR_X, carY: CAR_Y, leaders: LEADERS, labels: LABELS,
  wheels: Object.fromEntries(WHEELS.map(w => [w.id, { tx: w.tx, ty: w.ty, anchor: w.anchor, cols: w.cols }])) as unknown as DiagramLayout['wheels'],
  partLabel: 14, partValue: 26, partGap: 34, wheelTitle: 16, cellLabel: 13, cellValue: 20, cellLabelGap: 26, cellValueGap: 50,
}

// The half-width car for two files: the same callouts, but the wheel blocks sit above
// and below their wheels instead of beside them, so the drawing is narrower and every
// number can be drawn larger relative to the car.
const HALF_X = 190
const HALF_Y = 260
const HALF_LAYOUT: DiagramLayout = {
  viewBox: '0 90 1280 700', carX: HALF_X, carY: HALF_Y,
  leaders: [
    ['M318 144 H340 V310', 340, 310], ['M318 684 H340 V590', 340, 590],
    ['M950 144 H924 V302', 924, 302], ['M950 684 H924 V588', 924, 588],
    ['M180 430 H240', 240, 430], ['M180 510 H340', 340, 510],
    ['M784 194 V302', 784, 302], ['M634 558 V660', 634, 558],
    ['M1160 380 H1080', 1080, 380], ['M1160 510 H1080', 1080, 510],
  ],
  labels: [
    { value: 'v-rw', label: 'REAR WING', x: 170, y: 436, anchor: 'end' },
    { value: 'v-diff', label: 'DIFFUSER', x: 170, y: 516, anchor: 'end' },
    { value: 'v-floor', label: 'FLOOR', x: 784, y: 136, anchor: 'middle' },
    { value: 'v-sp', label: 'SIDEPODS', x: 634, y: 690, anchor: 'middle' },
    { value: 'v-fw-l', label: 'WING L', x: 1170, y: 386, anchor: 'start' },
    { value: 'v-fw-r', label: 'WING R', x: 1170, y: 516, anchor: 'start' },
  ],
  wheels: {
    rl: { tx: 308, ty: 150, anchor: 'end', cols: [20, 120, 220] },
    rr: { tx: 308, ty: 690, anchor: 'end', cols: [20, 120, 220] },
    fl: { tx: 960, ty: 150, anchor: 'start', cols: [960, 1060, 1160] },
    fr: { tx: 960, ty: 690, anchor: 'start', cols: [960, 1060, 1160] },
  },
  partLabel: 18, partValue: 32, partGap: 40, wheelTitle: 22, cellLabel: 15, cellValue: 26, cellLabelGap: 32, cellValueGap: 64,
}

function CarDiagram({ damage, isDark, scope = 'damage-car', label = 'Car damage by part', layout = FULL_LAYOUT }: {
  damage: DamageRow | null; isDark: boolean; scope?: string; label?: string; layout?: DiagramLayout
}) {
  const colors = palette(isDark)
  const tyreColors = tyrePalette(isDark)
  const pct = (v: number | null) => v === null ? missing : `${v}%`
  const rules = carRules(scope, damage, isDark)
  const L = layout

  return <svg className={`${scope} block w-full h-full`} viewBox={L.viewBox} role="img" aria-label={label}>
    <style>{rules.join('\n')}</style>
    <g transform={`translate(${L.carX} ${L.carY})`} dangerouslySetInnerHTML={{ __html: CAR_INNER }} />
    <g stroke="var(--text-muted)" strokeWidth="1.5" fill="none">
      {L.leaders.map(([d]) => <path key={d} d={d} />)}
    </g>
    <g fill="var(--text-dim)">
      {L.leaders.map(([d, x, y]) => <circle key={d} cx={x} cy={y} r="4" />)}
    </g>
    {L.labels.map(label => {
      const part = BODY_PARTS.find(p => p.value === label.value)!
      const v = num(damage, part.key)
      return <g key={label.value} fontFamily="inherit">
        <text x={label.x} y={label.y} textAnchor={label.anchor} fontSize={L.partLabel} letterSpacing="1.4" fill="var(--text-secondary)">{label.label}</text>
        <text x={label.x} y={label.y + L.partGap} textAnchor={label.anchor} fontSize={L.partValue} fontWeight="700" fill={colors[damageSeverity(v)]}>{pct(v)}</text>
      </g>
    })}
    {WHEELS.map(wheel => {
      const at = L.wheels[wheel.id]
      const cells: [string, number | null][] = [['TYRE', num(damage, wheel.tyre)], ['BRAKE', num(damage, wheel.brake)], ['BLISTERS', num(damage, wheel.blisters)]]
      return <g key={wheel.id} fontFamily="inherit">
        <text x={at.tx} y={at.ty} textAnchor={at.anchor} fontSize={L.wheelTitle} fontWeight="700" letterSpacing="1.6"
          fill={isDark ? wheel.color : wheel.lightColor}>{wheel.id.toUpperCase()}</text>
        {cells.map(([label, v], i) => <g key={label}>
          <text x={at.cols[i]} y={at.ty + L.cellLabelGap} fontSize={L.cellLabel} letterSpacing="1" fill="var(--text-secondary)">{label}</text>
          <text x={at.cols[i]} y={at.ty + L.cellValueGap} fontSize={L.cellValue} fontWeight="700" fill={tyreColors[tyreBand(v)]}>{pct(v)}</text>
        </g>)}
      </g>
    })}
  </svg>
}

/** One file's half: its coloured date, driver and lap, then the car diagram in its half-width layout. */
function FileHalf({ scope, date, colorVar, driver, detail, damage, isDark, showDiagram }: {
  scope: string; date: string; colorVar: '--file-a' | '--file-b'; driver: string | null; detail: string
  damage: DamageRow | null; isDark: boolean; showDiagram: boolean
}) {
  return <div className="flex-1 min-w-0 min-h-0 flex flex-col font-mono">
    <div className="shrink-0 h-8 flex items-baseline gap-2.5 px-4 pt-[9px] whitespace-nowrap overflow-hidden">
      <span className="text-[14px] font-bold" style={{ color: `var(${colorVar})` }}>{date}</span>
      {driver && <span className="text-[11.5px] font-bold uppercase truncate text-[var(--text-primary)]">{driver}</span>}
      {detail && <span className="text-[11px] truncate text-[var(--text-secondary)]">{detail}</span>}
    </div>
    {showDiagram
      ? <div className="flex-1 min-h-0 px-3 pt-3 pb-2"><CarDiagram scope={scope} damage={damage} isDark={isDark} label={`Car damage for ${date}`} layout={HALF_LAYOUT} /></div>
      : <div className="flex-1 min-h-0" />}
  </div>
}

type StatusCardId = keyof DamageLayout['statusCards']
interface StatusValue { value: string; unit?: string; color?: string }

/** The five status cards' values for one damage row, by card. */
function statusValues(damage: DamageRow | null, isDark: boolean): Record<StatusCardId, StatusValue> {
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
  return {
    engine: { value: pctText(engine), unit: '%', color: engine === null ? undefined : colors[wearSeverity(engine)] },
    gearbox: { value: pctText(gearbox), unit: '%', color: gearbox === null ? undefined : colors[wearSeverity(gearbox)] },
    wingFault: { value: fault(drs), color: faultColor(drs) },
    ersFault: { value: fault(ers), color: faultColor(ers) },
    engineStatus: { value: engineStatus ?? missing, color: engineStatus === null ? undefined : engineStatus === 'OK' ? colors.ok : colors.crit },
  }
}

/** A wear tile's value for one damage row. */
function wearValue(damage: DamageRow | null, key: DamageKey, isDark: boolean): StatusValue {
  const value = num(damage, key)
  return value === null ? { value: missing } : { value: String(value), unit: '%', color: palette(isDark)[wearSeverity(value)] }
}

const tableValue = ({ value, unit, color }: StatusValue): FileTableValue => value === missing ? { value: missing } : { value, unit, color }

/** "race · lap 25 / 71": the session from the header's detail, then the lap the file is on. */
function lapDetail(header: FileHeaderCell, lap: number | null, totalLaps: number | null): string {
  const session = header.detail.split(' · ')[0] ?? ''
  if (lap === null || lap <= 0) return session
  return [session, `lap ${lap}${totalLaps && totalLaps > 0 ? ` / ${totalLaps}` : ''}`].filter(Boolean).join(' · ')
}

// The page follows the streamed driver: the player live, the driver selector's car in
// V6 playback. Everything comes from the latest damage row.
export default memo(function DamagePage({ isDark, compact, visible }: { isDark: boolean; compact: DensityMode; visible: DamageLayout }) {
  const damage = useTelemetryStore(s => s.damage)
  const lapA = useTelemetryStore(s => s.lap?.lap_num ?? null)
  const totalLapsA = useTelemetryStore(s => s.session?.total_laps ?? null)
  // Side-by-side with a second file (Damage 9): B's newest damage row at its moment on the playhead.
  const secondary = useSecondaryCards()
  const totalLapsB = useSecondaryState().session?.total_laps ?? null
  const { t } = useLabels()
  const colors = palette(isDark)
  const pctText = (v: number | null) => v === null ? missing : String(v)

  const statusLabels: { id: StatusCardId; label: string; sub: string }[] = [
    { id: 'engine', label: 'Engine', sub: 'Overall wear' },
    { id: 'gearbox', label: 'Gearbox', sub: 'Wear' },
    // m_drsFault: titled DRS, or Rear Wing for F1 26 sessions (format catalog, like the Overview wing card).
    { id: 'wingFault', label: t('ui.damage.wing_fault'), sub: 'Fault flag' },
    { id: 'ersFault', label: 'ERS', sub: 'Fault flag' },
    { id: 'engineStatus', label: 'Engine status', sub: 'Blown / seized / fail' },
  ]
  const shownStatus = statusLabels.filter(card => visible.statusCards[card.id])
  const statusA = statusValues(damage, isDark)
  const statusCards = shownStatus.map(card => ({ ...card, ...statusA[card.id] }))
  const wearTiles = WEAR_TILES.filter(tile => visible.wearTiles[tile.id])

  if (secondary) {
    const statusB = statusValues(secondary.damage, isDark)
    const statusTable: FileTableCard[] = shownStatus.map(card => ({
      key: card.id, label: card.label, a: tableValue(statusA[card.id]), b: tableValue(statusB[card.id]),
    }))
    const wearTable: FileTableCard[] = wearTiles.map(tile => ({
      key: tile.id, label: `${tile.name} (Wear)`,
      a: tableValue(wearValue(damage, tile.key, isDark)), b: tableValue(wearValue(secondary.damage, tile.key, isDark)),
    }))
    return <div className="h-full min-h-0 overflow-hidden bg-[var(--bg-panel)] border-t border-[var(--border)] flex flex-col">
      <FileTableCards cards={statusTable} />
      <div className="flex-1 min-h-0 flex divide-x divide-[var(--border)]">
        <FileHalf scope="damage-car-a" date={secondary.dateA} colorVar="--file-a" driver={secondary.headerA.driver}
          detail={lapDetail(secondary.headerA, lapA, totalLapsA)} damage={damage} isDark={isDark} showDiagram={visible.showDiagram} />
        <FileHalf scope="damage-car-b" date={secondary.dateB} colorVar="--file-b" driver={secondary.headerB.driver}
          detail={lapDetail(secondary.headerB, secondary.lap?.lap_num ?? null, totalLapsB)} damage={secondary.damage} isDark={isDark} showDiagram={visible.showDiagram} />
      </div>
      {wearTable.length > 0 && <div className="shrink-0 border-t border-[var(--border)] [&>div]:border-b-0"><FileTableCards cards={wearTable} /></div>}
    </div>
  }

  return <div className="h-full min-h-0 overflow-hidden bg-[var(--bg-panel)] border-t border-[var(--border)] flex flex-col">
    {statusCards.length > 0 && <div className="shrink-0 flex divide-x divide-[var(--border)] border-b border-[var(--border)]">
      {statusCards.map(card => <StatCard key={card.id} label={card.label} value={card.value} unit={card.value === missing ? undefined : card.unit}
        textColor={card.color} sub={compact === 'spacious' ? card.sub : undefined} compact={compact} />)}
    </div>}
    {visible.showDiagram ? <div className="flex-1 min-h-0 px-3 pt-3 pb-2" aria-label="Car damage">
      <CarDiagram damage={damage} isDark={isDark} />
    </div> : <div className="flex-1 min-h-0" />}
    {/* One row of Overview stat cards, laid out like the Overview stats row. */}
    {wearTiles.length > 0 && <div className="shrink-0 flex divide-x divide-[var(--border)] border-t border-[var(--border)]" aria-label="Power unit and gearbox wear">
      {wearTiles.map(tile => {
        const value = num(damage, tile.key)
        return <StatCard key={tile.key} label={`${tile.name} (Wear)`} value={pctText(value)} unit={value === null ? undefined : '%'}
          textColor={value === null ? undefined : colors[wearSeverity(value)]} compact={compact} />
      })}
    </div>}
  </div>
})
