import { useMemo, memo, useEffect, useRef, useState } from 'react'
import type { LapRow, StatusRow, DamageRow, TimingMsg, ParticipantsMsg, SessionMsg, TyreSetsMsg, TyreSetEntry, AllStatusMsg } from '../types'
import { TRACK_MAPS } from '../lib/trackMaps'
import { useLabels, type Labels } from '../lib/labels'

// Resolves a tyre compound code to its display label (protocol-aware).
type TyreNamer = Labels['tn']

// ─── Circuit pit-lane time loss ────────────────────────────────────────────────
// Read from the track map (assets/maps/track_<id>.json, same files as the map
// renderer). Falls back to 11.25s in-lap / 13.75s out-lap / 25s total when a
// circuit has no map. Mirrors the native StrategyPage::loadPitLoss.
interface PitLoss { inlapMs: number; outlapMs: number; totalMs: number }
const DEFAULT_PIT_LOSS: PitLoss = { inlapMs: 11250, outlapMs: 13750, totalMs: 25000 }

function pitLossForTrack(trackId: number): PitLoss {
  const m = TRACK_MAPS[trackId] as unknown as Record<string, number> | undefined
  if (!m) return DEFAULT_PIT_LOSS
  return {
    inlapMs:  (m.inlap_pit_time  ?? DEFAULT_PIT_LOSS.inlapMs  / 1000) * 1000,
    outlapMs: (m.outlap_pit_time ?? DEFAULT_PIT_LOSS.outlapMs / 1000) * 1000,
    totalMs:  (m.pit_time        ?? DEFAULT_PIT_LOSS.totalMs  / 1000) * 1000,
  }
}

// ─── Lookup tables ────────────────────────────────────────────────────────────

const VISUAL_COLORS: Record<number, string> = {
  16: 'var(--compound-soft)',
  17: 'var(--compound-medium)',
  18: 'var(--compound-hard)',
   7: 'var(--compound-inter)',
   8: 'var(--compound-wet)',
}

// ─── Types ────────────────────────────────────────────────────────────────────

interface Props {
  lap: LapRow | null
  session: SessionMsg | null
  status: StatusRow | null
  damage: DamageRow | null
  timing: TimingMsg | null
  participants: ParticipantsMsg | null
  tyreSets: TyreSetsMsg | null
  allStatus: AllStatusMsg | null
  lapTimesByNum: Record<number, number>
  isDark: boolean
  compact?: boolean
}

interface StrategyCall {
  type: 'undercut' | 'overcut'
  targetIdx: number
  targetName: string
  gapMs: number
  crossoverLaps?: number
}

interface StintInfo {
  compoundName: string
  tyreColor: string
  actualCompound: number
  visualCompound: number
  lapCount: number
  startLap: number
  pitLap: number | null
  isLast: boolean
  currentWear?: number
}

interface StrategyResult {
  stops: number
  stints: StintInfo[]
}

interface StintTargetInfo {
  targetMs: number
  isEstimate: boolean
  lastLapDeltaMs: number | null  // positive = player over target (bad), negative = under (building gap)
}

// One row of a stint's per-lap target table.
interface TableLapRow {
  lapNum: number
  requiredMs: number
  actualRequiredMs: number
  actualMs: number
  deltaLapMs: number
  deltaStintMs: number
  deltaTotalMs: number
  hasActual: boolean
}

// A stint captured the moment it begins, so it can be kept on screen after it ends
// (the live plan only carries the current + future stints).
interface PastStint {
  startLap: number
  reqBaseMs: number
  compoundName: string
  color: string
  postPit: boolean       // its out-lap carries the boxing-time penalty
  expectedLaps: number   // planned length, frozen once the stint ends
}

// Everything needed to render one stint card + its per-lap table.
interface DisplayStint {
  compoundName: string
  color: string
  stintNumber: number    // 1-based position in the race (oldest = 1)
  startLap: number
  endLap: number
  lapCount: number       // expected/planned laps
  actualLaps: number     // laps actually completed on this set so far
  isLast: boolean
  rows: TableLapRow[]     // present once a target exists for the stint
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

function driverName(p: ParticipantsMsg | null, idx: number): string {
  const d = p?.drivers.find(d => d.idx === idx)
  if (!d) return `Car ${idx}`
  const parts = d.name.trim().split(/\s+/)
  return parts[parts.length - 1].toUpperCase()
}

function wearColor(pct: number): string {
  if (pct < 20) return '#73BF69'   // green
  if (pct < 40) return '#A8D436'   // yellow-green
  if (pct < 60) return '#FADE2A'   // yellow
  if (pct < 80) return '#FF9830'   // orange
  return '#C4162A'                  // red
}

function fmtLapTime(ms: number): string {
  if (ms <= 0 || ms > 600_000) return '—'
  const totalSecs = ms / 1000
  const mins = Math.floor(totalSecs / 60)
  const secs = totalSecs % 60
  return `${mins}:${secs.toFixed(1).padStart(4, '0')}`
}

// Signed seconds, e.g. +0.4 / −1.2 (used by the per-lap delta columns).
function fmtDelta(ms: number): string {
  const s = ms / 1000
  return `${s > 0 ? '+' : s < 0 ? '−' : ''}${Math.abs(s).toFixed(1)}`
}

// ─── Strategy calculation ────────────────────────────────────────────────────

function buildStints(
  lapNum: number,
  totalLaps: number,
  firstPitLap: number,
  currentCompound: { name: string; color: string; actual: number; visual: number },
  avgWear: number,
  availableSets: TyreSetEntry[],
  tn: TyreNamer,
): StrategyResult {
  const stints: StintInfo[] = []
  const clampedPit = Math.min(firstPitLap, totalLaps)
  stints.push({
    compoundName:   currentCompound.name,
    tyreColor:      currentCompound.color,
    actualCompound: currentCompound.actual,
    visualCompound: currentCompound.visual,
    lapCount:       clampedPit - lapNum,
    startLap:       lapNum,
    pitLap:         clampedPit >= totalLaps ? null : clampedPit,
    isLast:         clampedPit >= totalLaps,
    currentWear:    avgWear,
  })
  if (clampedPit >= totalLaps) return { stops: 0, stints }

  const pool = [...availableSets]
  let pitLap = clampedPit
  let remaining = totalLaps - pitLap

  for (const set of pool) {
    if (remaining <= 0) break
    const stintLen = Math.min(set.usable_life, remaining)
    const nextPit  = pitLap + stintLen
    const isLast   = nextPit >= totalLaps
    stints.push({
      compoundName:   tn('tyre.actual', set.actual_compound),
      tyreColor:      VISUAL_COLORS[set.visual_compound] ?? '#ffffff',
      actualCompound: set.actual_compound,
      visualCompound: set.visual_compound,
      lapCount:       stintLen,
      startLap:       pitLap,
      pitLap:         isLast ? null : nextPit,
      isLast,
    })
    remaining -= stintLen
    pitLap = nextPit
    if (isLast) break
  }

  if (remaining > 0 && stints.length > 0) {
    const last = stints[stints.length - 1]
    last.lapCount += remaining
    last.pitLap = null
    last.isLast = true
  }

  return { stops: stints.length - 1, stints }
}

function forceExtraStop(
  result: StrategyResult,
  availableSets: TyreSetEntry[],
  usedActualCompounds: Set<number>,
  tn: TyreNamer,
): StrategyResult {
  if (result.stints.length < 2) return result

  let longestIdx = 1
  for (let i = 2; i < result.stints.length; i++) {
    if (result.stints[i].lapCount > result.stints[longestIdx].lapCount) longestIdx = i
  }

  const target = result.stints[longestIdx]
  if (target.lapCount < 4) return result

  const splitLap = target.startLap + Math.floor(target.lapCount / 2)
  const nextSet = availableSets.find(s => !usedActualCompounds.has(s.actual_compound))
    ?? availableSets.find(s => s.actual_compound !== target.actualCompound)
    ?? availableSets[0]

  if (!nextSet) return result

  const part1: StintInfo = {
    ...target,
    lapCount: splitLap - target.startLap,
    pitLap:   splitLap,
    isLast:   false,
  }
  const part2: StintInfo = {
    compoundName:   tn('tyre.actual', nextSet.actual_compound),
    tyreColor:      VISUAL_COLORS[nextSet.visual_compound] ?? '#ffffff',
    actualCompound: nextSet.actual_compound,
    visualCompound: nextSet.visual_compound,
    lapCount:       (target.isLast ? (target.startLap + target.lapCount) : (target.pitLap ?? target.startLap + target.lapCount)) - splitLap,
    startLap:       splitLap,
    pitLap:         target.pitLap,
    isLast:         target.isLast,
  }

  const newStints = [...result.stints]
  newStints.splice(longestIdx, 1, part1, part2)
  return { stops: result.stops + 1, stints: newStints }
}

function applyMonacoRule(
  result: StrategyResult,
  availableSets: TyreSetEntry[],
  usedActualCompounds: Set<number>,
  tn: TyreNamer,
): StrategyResult {
  if (result.stops >= 2) return result
  return forceExtraStop(result, availableSets, usedActualCompounds, tn)
}

// ─── Sub-components ───────────────────────────────────────────────────────────

const CompoundChip = memo(function CompoundChip({ name, color }: { name: string; color: string }) {
  return (
    <span
      className="text-[10px] font-bold px-2 py-0.5 rounded border shrink-0"
      style={{ color, borderColor: color, backgroundColor: color + '1a' }}
    >
      {name}
    </span>
  )
})

// One stint: header row (compound · number · range · expected/actual laps) plus
// its per-lap target table. The in-lap (pit) row is tinted green, mirroring the
// Standings fastest-lap highlight; delta cells are red (slower) / green (faster).
const TABLE_HEADS = ['LAP', 'REQ', 'ADJ REQ', 'ACTUAL', 'Δ LAP', 'Δ STINT', 'Δ TOTAL'] as const

const StintGroup = memo(function StintGroup({ stint, withTopSep }: { stint: DisplayStint; withTopSep: boolean }) {
  return (
    <div className={withTopSep ? 'border-t border-[var(--border)]' : ''}>
      {/* Header row */}
      <div className="flex items-center gap-2 px-4 py-2">
        <CompoundChip name={stint.compoundName} color={stint.color} />
        <span className="text-sm font-bold tabular-nums text-[var(--text-primary)]">{stint.stintNumber}</span>
        <div className="flex-1" />
        <span className="text-[10px] text-[var(--text-secondary)] tabular-nums">
          {stint.startLap}–{stint.endLap}
        </span>
        <div className="flex-1" />
        <span className="text-[9px] uppercase tracking-wider text-[var(--text-secondary)]">Expected</span>
        <span className="text-sm font-bold tabular-nums text-[var(--text-primary)]">{stint.lapCount}</span>
        <span className="text-[9px] uppercase tracking-wider text-[var(--text-secondary)]">Actual</span>
        <span className="text-sm font-bold tabular-nums text-[var(--text-primary)]">{stint.actualLaps}</span>
      </div>

      {/* Per-lap target table */}
      {stint.rows.length > 0 && (
        <table className="w-full border-collapse">
          <thead>
            <tr className="border-y border-[var(--border)]">
              {TABLE_HEADS.map(h => (
                <th key={h} className="px-2 py-1 text-center text-[9px] text-[var(--text-secondary)] uppercase tracking-wider font-normal">
                  {h}
                </th>
              ))}
            </tr>
          </thead>
          <tbody>
            {stint.rows.map((r, i) => {
              const isPit = !stint.isLast && r.lapNum === stint.endLap
              const rowCls = isPit
                ? 'bg-[#73BF69]/15'
                : i % 2 === 1 ? 'bg-[var(--bg-input)]/30' : ''
              return (
                <tr key={r.lapNum} className={rowCls}>
                  <td className="px-2 py-1 text-center text-[11px] tabular-nums text-[var(--text-secondary)]">{r.lapNum}</td>
                  <td className="px-2 py-1 text-center text-[11px] tabular-nums text-[var(--text-primary)]">{fmtLapTime(r.requiredMs)}</td>
                  <td className="px-2 py-1 text-center text-[11px] tabular-nums text-[var(--text-secondary)]">{fmtLapTime(r.actualRequiredMs)}</td>
                  <td className="px-2 py-1 text-center text-[11px] tabular-nums text-[var(--text-primary)]">{r.hasActual ? fmtLapTime(r.actualMs) : '—'}</td>
                  <td className="px-2 py-1 text-center text-[11px] tabular-nums" style={{ color: r.hasActual ? (r.deltaLapMs   > 0 ? '#C4162A' : '#73BF69') : 'var(--text-muted)' }}>{r.hasActual ? fmtDelta(r.deltaLapMs)   : '—'}</td>
                  <td className="px-2 py-1 text-center text-[11px] tabular-nums" style={{ color: r.hasActual ? (r.deltaStintMs > 0 ? '#C4162A' : '#73BF69') : 'var(--text-muted)' }}>{r.hasActual ? fmtDelta(r.deltaStintMs) : '—'}</td>
                  <td className="px-2 py-1 text-center text-[11px] tabular-nums" style={{ color: r.hasActual ? (r.deltaTotalMs > 0 ? '#C4162A' : '#73BF69') : 'var(--text-muted)' }}>{r.hasActual ? fmtDelta(r.deltaTotalMs) : '—'}</td>
                </tr>
              )
            })}
          </tbody>
        </table>
      )}
    </div>
  )
})

// One strategy variant column: a fixed (non-scrolling) header bar with the stop
// count + Monaco flag, then the scrollable stack of stint groups.
const StintColumn = memo(function StintColumn({
  display, stops, isMonaco, label, accentColor,
}: {
  display: DisplayStint[]
  stops: number
  isMonaco: boolean
  label: string
  accentColor: string
}) {
  return (
    <div className="flex flex-col h-full min-h-0">
      {/* Fixed header bar */}
      <div className="shrink-0 flex items-center justify-between px-4 py-2.5 border-b border-[var(--border)]">
        <div className="flex items-center gap-2">
          <span className="text-[10px] font-medium uppercase tracking-widest text-[var(--text-secondary)]">{label}</span>
          {isMonaco && (
            <span
              className="text-[9px] font-bold px-1.5 py-0.5 rounded border"
              style={{ color: '#FADE2A', borderColor: '#FADE2A', backgroundColor: 'rgba(250,222,42,0.08)' }}
            >
              Monaco 2-stop
            </span>
          )}
        </div>
        <div className="flex items-baseline gap-1.5">
          <span className="text-2xl font-black tabular-nums leading-none" style={{ color: accentColor }}>{stops}</span>
          <span className="text-xs font-medium text-[var(--text-secondary)]">stop{stops !== 1 ? 's' : ''}</span>
        </div>
      </div>

      {/* Scrollable stint groups */}
      <div className="flex-1 min-h-0 overflow-y-auto">
        {display.map((st, i) => (
          <StintGroup key={st.stintNumber} stint={st} withTopSep={i > 0} />
        ))}
      </div>
    </div>
  )
})

function Placeholder({ children }: { children: React.ReactNode }) {
  return (
    <div className="flex-1 flex items-center justify-center text-[var(--text-secondary)]">
      <div className="flex flex-col items-center gap-3 max-w-xs text-center">
        {children}
      </div>
    </div>
  )
}

// ─── StrategyCallPanel ───────────────────────────────────────────────────────

const StrategyCallPanel = memo(function StrategyCallPanel({
  call, participants,
}: { call: StrategyCall; participants: ParticipantsMsg | null }) {
  const isUndercut = call.type === 'undercut'
  const color      = isUndercut ? '#FADE2A' : '#73BF69'
  const label      = isUndercut ? 'UNDERCUT' : 'OVERCUT'
  const gapStr     = isUndercut
    ? `+${(call.gapMs / 1000).toFixed(1)}s ahead`
    : `−${(call.gapMs / 1000).toFixed(1)}s behind`
  const actionLine = isUndercut
    ? `Pit now — recover time in ${call.crossoverLaps} lap${call.crossoverLaps !== 1 ? 's' : ''}`
    : 'Stay out — build gap while they stop'

  return (
    <div className="border-b border-[var(--border)]">
      <div className="px-4 pt-3 pb-2">
        <span className="text-[10px] font-medium uppercase tracking-widest text-[var(--text-secondary)]">Strategy Call</span>
      </div>
      <div className="flex items-center gap-3 px-4 pb-2">
        <span
          className="text-[9px] font-bold px-1.5 py-0.5 rounded border shrink-0"
          style={{ color, borderColor: color, backgroundColor: color + '1a' }}
        >
          {label}
        </span>
        <span className="text-sm font-bold text-[var(--text-primary)] truncate flex-1">
          {driverName(participants, call.targetIdx)}
        </span>
        <span className="text-xs font-semibold tabular-nums shrink-0" style={{ color }}>
          {gapStr}
        </span>
      </div>
      <div className="px-4 pb-3">
        <span className="text-[10px] text-[var(--text-secondary)]">{actionLine}</span>
      </div>
    </div>
  )
})

// ─── RivalsPanel ─────────────────────────────────────────────────────────────

// Signature of the two rival rows' displayed state (position, retired, 0.1s gap),
// so 20Hz sub-0.1s gap jitter doesn't re-render the panel.
function rivalsSig(rivals: { aheadIdx: number | null; behindIdx: number | null }, timing: TimingMsg): string {
  const player = timing.cars.find(c => c.idx === timing.player_idx)
  if (!player) return ''
  const parts: string[] = []
  for (const [idx, tag, ahead] of [[rivals.aheadIdx, 'a', true], [rivals.behindIdx, 'b', false]] as const) {
    if (idx == null) continue
    const car = timing.cars.find(c => c.idx === idx)
    if (!car) { parts.push(`${tag}${idx}:none`); continue }
    const rel = ahead ? player.gap_ms - car.gap_ms : car.gap_ms - player.gap_ms
    parts.push(`${tag}${idx}:${car.position}:${car.result_status}:${Math.round(rel / 100)}`)
  }
  return parts.join('|')
}

const RivalsPanel = memo(function RivalsPanel({
  rivals, timing, participants,
}: {
  rivals: { aheadIdx: number | null; behindIdx: number | null }
  timing: TimingMsg
  participants: ParticipantsMsg | null
}) {
  const player = timing.cars.find(c => c.idx === timing.player_idx)
  if (!player) return null

  const rows: { idx: number; dir: 'ahead' | 'behind' }[] = []
  if (rivals.aheadIdx  !== null) rows.push({ idx: rivals.aheadIdx,  dir: 'ahead'  })
  if (rivals.behindIdx !== null) rows.push({ idx: rivals.behindIdx, dir: 'behind' })
  if (rows.length === 0) return null

  return (
    <div className="border-b border-[var(--border)]">
      <div className="px-4 pt-3 pb-2">
        <span className="text-[10px] font-medium uppercase tracking-widest text-[var(--text-secondary)]">Rivals</span>
      </div>
      <div className="flex flex-col divide-y divide-[var(--border)]">
        {rows.map(({ idx, dir }) => {
          const car       = timing.cars.find(c => c.idx === idx)
          const livery    = participants?.drivers.find(d => d.idx === idx)?.livery_color ?? '#8e8e8e'
          const isRetired = !car || car.result_status !== 2
          const dirColor  = dir === 'ahead' ? '#5794F2' : '#FADE2A'

          let gapStr: string | null = null
          if (car && !isRetired) {
            if (dir === 'ahead') {
              const g = player.gap_ms - car.gap_ms
              if (g > 0) gapStr = `+${(g / 1000).toFixed(1)}s`
            } else {
              const g = car.gap_ms - player.gap_ms
              if (g > 0) gapStr = `−${(g / 1000).toFixed(1)}s`
            }
          }

          return (
            <div key={idx} className="flex items-center gap-2.5 px-4 py-2.5">
              <span
                className="text-[9px] font-bold w-4 text-center shrink-0"
                style={{ color: dirColor }}
              >
                {dir === 'ahead' ? '▲' : '▼'}
              </span>
              {car && !isRetired ? (
                <span
                  className="text-[10px] font-bold tabular-nums w-7 text-center py-0.5 rounded shrink-0"
                  style={{ backgroundColor: livery + '28', color: livery }}
                >
                  P{car.position}
                </span>
              ) : (
                <span className="text-[10px] w-7 text-center text-[var(--text-secondary)] shrink-0">—</span>
              )}
              <span className={`text-sm font-bold flex-1 truncate ${isRetired ? 'opacity-40 text-[var(--text-secondary)]' : 'text-[var(--text-secondary)]'}`}>
                {driverName(participants, idx)}
              </span>
              {isRetired ? (
                <span className="text-[9px] font-bold opacity-40 text-[var(--text-secondary)] shrink-0">DNF</span>
              ) : gapStr != null ? (
                <span className="text-xs font-semibold tabular-nums shrink-0 text-[var(--text-secondary)]">{gapStr}</span>
              ) : null}
            </div>
          )
        })}
      </div>
    </div>
  )
}, (a, b) =>
  a.rivals === b.rivals && a.participants === b.participants &&
  rivalsSig(a.rivals, a.timing) === rivalsSig(b.rivals, b.timing),
)

// ─── PositionPanel ────────────────────────────────────────────────────────────
// Isolated so its 20Hz gap-trend state updates don't re-render strategy columns.

interface WearWarning { text: string; detail: string; color: string }
const EMPTY_WARNINGS: WearWarning[] = []

// Selects the cars shown in the Position panel: the player, up to 3 ahead and 3
// behind, topped up to keep ~7 rows centred on the player.
function selectPositionCars(timing: TimingMsg) {
  const active = timing.cars.filter(c => c.result_status === 2 && c.position > 0)
  const player = active.find(c => c.idx === timing.player_idx)
  if (!player) return null

  const carsAhead  = active.filter(c => c.position < player.position).length
  const carsBehind = active.filter(c => c.position > player.position).length
  const baseAhead  = Math.min(3, carsAhead)
  const baseBehind = Math.min(3, carsBehind)
  const aheadCount  = Math.min(baseAhead  + Math.max(0, 3 - baseBehind), carsAhead)
  const behindCount = Math.min(baseBehind + Math.max(0, 3 - baseAhead),  carsBehind)

  const aheadCars  = Array.from({ length: aheadCount },  (_, i) => active.find(x => x.position === player.position - (i + 1))).filter((c): c is typeof player => !!c)
  const behindCars = Array.from({ length: behindCount }, (_, i) => active.find(x => x.position === player.position + (i + 1))).filter((c): c is typeof player => !!c)

  return { player, aheadCars, behindCars }
}

// Signature of everything the panel actually displays — positions, pit state,
// stop counts and gaps bucketed to the shown 0.1s resolution. Equal signatures
// mean an identical render, so the 20Hz sub-0.1s gap jitter is skipped.
function positionSig(timing: TimingMsg, pitCounts: Map<number, number>): string {
  const d = selectPositionCars(timing)
  if (!d) return ''
  const { player, aheadCars, behindCars } = d
  const parts = [`p${player.idx}:${player.position}:${player.pit_status}`]
  for (const c of aheadCars) parts.push(`a${c.idx}:${c.position}:${c.pit_status}:${pitCounts.get(c.idx) ?? 0}:${Math.round((player.gap_ms - c.gap_ms) / 100)}`)
  for (const c of behindCars) parts.push(`b${c.idx}:${c.position}:${c.pit_status}:${pitCounts.get(c.idx) ?? 0}:${Math.round((c.gap_ms - player.gap_ms) / 100)}`)
  return parts.join('|')
}

const PositionPanel = memo(function PositionPanel({
  timing, participants, pitCounts,
}: { timing: TimingMsg; participants: ParticipantsMsg | null; pitCounts: Map<number, number> }) {
  const positionData = useMemo(() => selectPositionCars(timing), [timing])

  const prevBehindGapRef = useRef<number | null>(null)
  const prevAheadGapRef  = useRef<number | null>(null)
  const [behindIsGaining,    setBehindIsGaining]    = useState<boolean | null>(null)
  const [playerGainingAhead, setPlayerGainingAhead] = useState<boolean | null>(null)

  useEffect(() => {
    const immediateBehind = positionData?.behindCars[0] ?? null
    const immediateAhead  = positionData?.aheadCars[0]  ?? null

    if (!immediateBehind || !positionData?.player) {
      prevBehindGapRef.current = null
      setBehindIsGaining(null)
    } else {
      const g = immediateBehind.gap_ms - positionData.player.gap_ms
      if (prevBehindGapRef.current !== null) {
        const d = g - prevBehindGapRef.current
        if (Math.abs(d) > 30) setBehindIsGaining(d < 0)
      }
      prevBehindGapRef.current = g
    }

    if (!immediateAhead || !positionData?.player) {
      prevAheadGapRef.current = null
      setPlayerGainingAhead(null)
    } else {
      const g = positionData.player.gap_ms - immediateAhead.gap_ms
      if (prevAheadGapRef.current !== null) {
        const d = g - prevAheadGapRef.current
        if (Math.abs(d) > 30) setPlayerGainingAhead(d < 0)
      }
      prevAheadGapRef.current = g
    }
  }, [positionData])

  return (
    <div>
      <div className="px-4 pt-3 pb-2 border-b border-[var(--border)]">
        <span className="text-[10px] font-medium uppercase tracking-widest text-[var(--text-secondary)]">Position</span>
      </div>
      {positionData ? (
        <div className="flex flex-col divide-y divide-[var(--border)]">
          {[
            ...positionData.aheadCars.slice().reverse().map((car, revI) => ({
              car, role: 'ahead' as const,
              isImmediate: revI === positionData.aheadCars.length - 1,
            })),
            { car: positionData.player, role: 'player' as const, isImmediate: false },
            ...positionData.behindCars.map((car, i) => ({
              car, role: 'behind' as const,
              isImmediate: i === 0,
            })),
          ].map(({ car, role, isImmediate }) => {
            const isPlayer = role === 'player'
            const livery   = participants?.drivers.find(d => d.idx === car.idx)?.livery_color ?? '#8e8e8e'

            let gapMs: number | null = null
            let gapColor = 'var(--text-secondary)'

            if (role === 'ahead') {
              const gap = positionData.player.gap_ms - car.gap_ms
              if (gap > 0) {
                gapMs = gap
                if (gap > 10_000) {
                  gapColor = '#C4162A'
                } else if (isImmediate) {
                  gapColor = playerGainingAhead === true ? '#73BF69' : '#FADE2A'
                }
              }
            } else if (role === 'behind') {
              const gap = car.gap_ms - positionData.player.gap_ms
              if (gap > 0) {
                gapMs = gap
                if (gap < 1_000) {
                  gapColor = '#C4162A'
                } else if (isImmediate) {
                  gapColor = behindIsGaining === true ? '#FADE2A' : '#73BF69'
                }
              }
            }

            const stops = pitCounts.get(car.idx) ?? 0
            const inPit = car.pit_status !== 0

            return (
              <div key={car.idx} className="flex items-center gap-3 px-5 py-3">
                <span
                  className="text-[10px] font-bold tabular-nums w-7 text-center py-0.5 rounded shrink-0"
                  style={{ backgroundColor: livery + '28', color: livery }}
                >
                  P{car.position}
                </span>
                <span className={`text-sm font-bold flex-1 truncate ${isPlayer ? 'text-[var(--text-primary)]' : 'text-[var(--text-secondary)]'}`}>
                  {driverName(participants, car.idx)}
                </span>
                {inPit ? (
                  <span
                    className="text-[9px] font-bold px-1.5 py-0.5 rounded border shrink-0"
                    style={{ color: '#FADE2A', borderColor: '#FADE2A', backgroundColor: 'rgba(250,222,42,0.08)' }}
                  >
                    PIT
                  </span>
                ) : stops > 0 ? (
                  <span className="text-[9px] tabular-nums text-[var(--text-secondary)] shrink-0">
                    {stops} pit{stops !== 1 ? 's' : ''}
                  </span>
                ) : null}
                {gapMs != null && (
                  <span className="text-xs font-semibold tabular-nums shrink-0" style={{ color: gapColor }}>
                    {role === 'ahead' ? `+${(gapMs / 1000).toFixed(1)}s` : `-${(gapMs / 1000).toFixed(1)}s`}
                  </span>
                )}
              </div>
            )
          })}
        </div>
      ) : (
        <p className="text-xs text-[var(--text-secondary)] px-5 py-3">No position data</p>
      )}
    </div>
  )
}, (a, b) =>
  // Skip the render unless the displayed model changed (names/livery come from
  // participants; everything else is captured by the signature).
  a.participants === b.participants &&
  positionSig(a.timing, a.pitCounts) === positionSig(b.timing, b.pitCounts),
)

// ─── TyreWearPanel ────────────────────────────────────────────────────────────
// Isolated so wear data updates don't re-render strategy columns.

function warningsEqual(a: WearWarning[], b: WearWarning[]): boolean {
  if (a.length !== b.length) return false
  for (let i = 0; i < a.length; i++) {
    if (a[i].text !== b[i].text || a[i].detail !== b[i].detail || a[i].color !== b[i].color) return false
  }
  return true
}

// Props are rounded ints (not the raw 20Hz damage row) + a content-compared
// warnings list, so this only repaints when a shown wear % or a warning changes.
const TyreWearPanel = memo(function TyreWearPanel({
  fl, fr, rl, rr, wearWarnings,
}: { fl: number; fr: number; rl: number; rr: number; wearWarnings: WearWarning[] }) {
  return (
    <>
      <div className="px-4 pt-3 pb-2 border-t border-[var(--border)]">
        <span className="text-[10px] font-medium uppercase tracking-widest text-[var(--text-secondary)]">Tyre Wear</span>
      </div>
      <div className="grid grid-cols-2 border-t border-[var(--border)]">
        {([
          { key: 'fl', label: 'FL', value: fl, borderCls: 'border-r border-b' },
          { key: 'fr', label: 'FR', value: fr, borderCls: 'border-b' },
          { key: 'rl', label: 'RL', value: rl, borderCls: 'border-r' },
          { key: 'rr', label: 'RR', value: rr, borderCls: '' },
        ] as const).map(({ key, label, value, borderCls }) => {
          const c = wearColor(value)
          return (
            <div key={key} className={`flex flex-col justify-between px-4 py-3 gap-2 border-[var(--border)] ${borderCls}`}>
              <div className="flex items-baseline justify-between">
                <span className="text-[9px] font-medium uppercase tracking-wider text-[var(--text-secondary)]">{label}</span>
                <span className="text-sm font-black tabular-nums leading-none" style={{ color: c }}>{value}%</span>
              </div>
              <div className="h-1 bg-[var(--border)] rounded-full overflow-hidden">
                <div className="h-full rounded-full" style={{ width: `${Math.min(100, value)}%`, backgroundColor: c }} />
              </div>
            </div>
          )
        })}
      </div>
      {wearWarnings.length > 0 && (
        <div className="flex flex-col gap-2 px-4 py-3 border-t border-[var(--border)]">
          {wearWarnings.map((w, i) => (
            <div key={i} className="flex flex-col gap-0.5">
              <span className="text-[10px] font-bold" style={{ color: w.color }}>⚠ {w.text}</span>
              <span className="text-[10px] text-[var(--text-secondary)] leading-relaxed">{w.detail}</span>
            </div>
          ))}
        </div>
      )}
    </>
  )
}, (a, b) =>
  a.fl === b.fl && a.fr === b.fr && a.rl === b.rl && a.rr === b.rr &&
  warningsEqual(a.wearWarnings, b.wearWarnings),
)

// ─── Header ──────────────────────────────────────────────────────────────────
// Memoised on the *displayed* (rounded) values, so the 20Hz wear float doesn't
// re-render it — it only repaints when a shown number actually changes.

const StrategyHeader = memo(function StrategyHeader({
  lapNum, totalLaps, tyreName, tyreColor, wearPct, tyreAge, wearPerLapStr,
  hasStrategy, cliffLap, lapsUntilCliff, compact,
}: {
  lapNum: number | null
  totalLaps: number
  tyreName: string
  tyreColor: string
  wearPct: number
  tyreAge: number | null
  wearPerLapStr: string
  hasStrategy: boolean
  cliffLap: number
  lapsUntilCliff: number
  compact?: boolean
}) {
  const wearBar = wearColor(wearPct)
  const cellPad = compact ? 'py-1.5' : 'py-3'
  const labelMb = compact ? 'mb-0' : 'mb-1'
  return (
    <div className="shrink-0 flex divide-x divide-[var(--border)] border-b border-[var(--border)]">
      {/* Lap counter */}
      <div className={`shrink-0 flex flex-col justify-center px-6 ${cellPad}`}>
        <span className={`text-[10px] font-medium uppercase tracking-widest text-[var(--text-secondary)] ${labelMb}`}>Lap</span>
        <div className="flex items-baseline gap-1.5">
          <span className={`${compact ? 'text-xl' : 'text-3xl'} font-black tabular-nums leading-none text-[var(--text-primary)]`}>
            {lapNum ?? '—'}
          </span>
          <span className="text-base font-medium text-[var(--text-secondary)]">
            / {totalLaps > 0 ? totalLaps : '—'}
          </span>
        </div>
      </div>

      {/* Compound + wear */}
      <div className={`flex-1 min-w-0 flex items-center gap-4 px-6 ${cellPad}`}>
        <CompoundChip name={tyreName} color={tyreColor} />
        <div className="flex-1 min-w-0">
          <div className={`flex items-baseline justify-between gap-3 ${compact ? 'mb-0.5' : 'mb-1.5'}`}>
            <span className="text-lg font-black tabular-nums leading-none" style={{ color: wearBar }}>
              {wearPct}%
            </span>
            <span className="text-[10px] text-[var(--text-secondary)] tabular-nums shrink-0">
              {tyreAge != null ? `${tyreAge}L · ${wearPerLapStr}%/L` : '—'}
            </span>
          </div>
          <div className="h-1.5 bg-[var(--border)] rounded-full overflow-hidden">
            <div
              className="h-full rounded-full transition-all duration-300"
              style={{ width: `${Math.min(100, wearPct)}%`, backgroundColor: wearBar }}
            />
          </div>
        </div>
      </div>

      {/* Tyre cliff */}
      <div className={`shrink-0 flex flex-col justify-center px-6 ${cellPad}`}>
        <span className={`text-[10px] font-medium uppercase tracking-widest text-[var(--text-secondary)] ${labelMb}`}>Tyre Cliff</span>
        {hasStrategy ? (
          <div className="flex items-baseline gap-1.5">
            <span
              className="text-lg font-black tabular-nums leading-none"
              style={{ color: lapsUntilCliff <= 5 ? '#C4162A' : lapsUntilCliff <= 10 ? '#FADE2A' : 'var(--text-primary)' }}
            >
              Lap {cliffLap}
            </span>
            <span className="text-[10px] text-[var(--text-secondary)]">+{lapsUntilCliff}</span>
          </div>
        ) : (
          <span className="text-sm text-[var(--text-secondary)]">—</span>
        )}
      </div>
    </div>
  )
})

// ─── Main component ───────────────────────────────────────────────────────────

const StrategyPanel = memo(function StrategyPanel({
  lap, session, status, damage, timing, participants, tyreSets, allStatus, lapTimesByNum, isDark, compact,
}: Props) {
  const { tn } = useLabels()
  const pitLoss = useMemo(() => pitLossForTrack(session?.track_id ?? -1), [session?.track_id])

  const isRaceSession = session !== null && [15, 16, 17].includes(session.session_type)
  // A fitted tyre keeps the strategy valid regardless of age, so a fresh tyre on a
  // pit out-lap (age 0) doesn't blank the panel. Matches native StrategyPage.
  const hasTyreData   = !!status && status.tyre_compound > 0

  const strategyData = useMemo(() => {
    // Hold off until lap 2 is complete (lap_num ≥ 3, so last_lap_ms is lap 2's time).
    // Lap 1 is a standing start — its pace is useless as a Required baseline.
    if (!lap || !session || !status || !damage || !isRaceSession || session.total_laps <= 0 || !hasTyreData || lap.lap_num < 3) return null

    const flW = damage.tyre_wear_fl
    const frW = damage.tyre_wear_fr
    const rlW = damage.tyre_wear_rl
    const rrW = damage.tyre_wear_rr
    const avgWear    = (flW + frW + rlW + rrW) / 4
    const wearPerLap = status.tyre_age_laps > 0 ? avgWear / status.tyre_age_laps : 2
    const cliffPct   = [16, 17, 18].includes(status.visual_compound) ? 80 : 70
    const isMonaco   = session.track_id === 5

    // ── Limiting tyre ──────────────────────────────────────────────────────
    // Pit timing must follow the corner that reaches the cliff first, not the
    // four-corner average. Per corner we project laps-to-cliff from its own wear
    // and wear rate; the soonest one governs the strategy.
    const cornersW = [
      { name: 'FL', wear: flW }, { name: 'FR', wear: frW },
      { name: 'RL', wear: rlW }, { name: 'RR', wear: rrW },
    ]
    const cornerRate  = (w: number) => status.tyre_age_laps > 0 ? w / status.tyre_age_laps : 2
    const lapsToCliff = (w: number) => (cliffPct - w) / Math.max(0.01, cornerRate(w))
    let limitIdx = 0
    let limitLapsLeft = lapsToCliff(cornersW[0].wear)
    for (let i = 1; i < 4; i++) {
      const left = lapsToCliff(cornersW[i].wear)
      if (left < limitLapsLeft) { limitLapsLeft = left; limitIdx = i }
    }
    const limitWear       = cornersW[limitIdx].wear
    const limitWearPerLap = Math.max(0.01, cornerRate(limitWear))
    const limitCorner     = cornersW[limitIdx].name

    const WET_COMPOUNDS = new Set([7, 8])
    const isWetWeather  = session.weather >= 3
    const basePool = (tyreSets?.sets ?? [])
      .filter(s => s.available && !s.fitted && s.usable_life > 0)
      .filter(s => isWetWeather ? WET_COMPOUNDS.has(s.actual_compound) : !WET_COMPOUNDS.has(s.actual_compound))

    const conservativePool = [...basePool].sort((a, b) => b.usable_life - a.usable_life)
    const aggressivePool   = [...basePool].sort((a, b) => a.lap_delta_ms - b.lap_delta_ms)

    const currentCompound = {
      name:   tn('tyre.actual', status.tyre_compound),
      color:  VISUAL_COLORS[status.visual_compound] ?? '#ffffff',
      actual: status.tyre_compound,
      visual: status.visual_compound,
    }

    const consLapsLeft = Math.max(0, Math.floor((cliffPct - limitWear) / limitWearPerLap))
    const consCliffLap = lap.lap_num + consLapsLeft

    // Estimate per-lap pace gain from the best available fresh set vs current (worn) set.
    // lap_delta_ms is negative when a set is faster; netting fittedSet - bestConsSet gives
    // the ms/lap benefit of switching now.  Conservative values track position, so we use
    // 30 s as the effective pit-stop cost — only box early when degradation is truly severe.
    const fittedSet   = tyreSets?.sets.find(s => s.fitted)
    const bestConsSet = conservativePool[0]
    const freshGainMs = bestConsSet
      ? Math.max(0, (fittedSet?.lap_delta_ms ?? 0) - bestConsSet.lap_delta_ms)
      : 0
    const consFirstPit = freshGainMs > 500 && freshGainMs * consLapsLeft > pitLoss.totalMs
      ? Math.max(lap.lap_num + 1, lap.lap_num + Math.ceil(pitLoss.totalMs / freshGainMs))
      : consCliffLap

    let conservative = buildStints(lap.lap_num, session.total_laps, consFirstPit, currentCompound, avgWear, conservativePool, tn)
    if (isMonaco) {
      const usedCompounds = new Set(conservative.stints.map(s => s.actualCompound))
      conservative = applyMonacoRule(conservative, conservativePool, usedCompounds, tn)
    }

    const aggWearRate    = limitWearPerLap * 1.2
    const aggLapsLeft    = Math.max(0, Math.floor((cliffPct - limitWear) / aggWearRate))
    const aggCliffLap    = lap.lap_num + aggLapsLeft
    const bestAggSet     = aggressivePool[0]
    const economicOptLap = bestAggSet
      ? Math.max(lap.lap_num + 1, session.total_laps - bestAggSet.usable_life)
      : aggCliffLap
    const aggPitLap = Math.min(aggCliffLap, economicOptLap)
    let aggressive = buildStints(lap.lap_num, session.total_laps, aggPitLap, currentCompound, avgWear, aggressivePool, tn)
    if (aggressive.stops <= conservative.stops) {
      const usedCompounds = new Set(aggressive.stints.map(s => s.actualCompound))
      aggressive = forceExtraStop(aggressive, aggressivePool, usedCompounds, tn)
    }
    if (isMonaco) {
      const usedCompounds = new Set(aggressive.stints.map(s => s.actualCompound))
      aggressive = applyMonacoRule(aggressive, aggressivePool, usedCompounds, tn)
    }

    const leftWear  = (flW + rlW) / 2
    const rightWear = (frW + rrW) / 2
    const frontWear = (flW + frW) / 2
    const rearWear  = (rlW + rrW) / 2
    const avg       = (flW + frW + rlW + rrW) / 4
    const maxWear   = Math.max(flW, frW, rlW, rrW)
    const sideImb   = rightWear - leftWear   // + = right wearing faster
    const axleImb   = frontWear - rearWear   // + = front wearing faster
    const diagImb   = (flW + rrW) / 2 - (frW + rlW) / 2  // + = oversteer pattern
    const rearImb   = rlW - rrW             // + = rear-left working harder

    type WarnPriority = 0 | 1 | 2 | 3
    const candidates: (WearWarning & { priority: WarnPriority })[] = []

    // Structural risk — single corner >22% above average (cliff + fatigue risk)
    if (maxWear > avg + 22) {
      const corner = maxWear === flW ? 'FL' : maxWear === frW ? 'FR' : maxWear === rlW ? 'RL' : 'RR'
      const detail = corner === 'FL'
        ? 'Outside front taking 70–80% of axle load in right-handers — structural fatigue imminent; prioritise your box lap'
        : corner === 'FR'
        ? 'Outside front taking 70–80% of axle load in left-handers — structural fatigue imminent; prioritise your box lap'
        : corner === 'RL'
        ? 'Rear-left near structural limit — wheelspin overheating on exit; close differential or ease early throttle'
        : 'Rear-right near structural limit — peak lateral and drive load; smooth corner entry and mid-corner throttle'
      candidates.push({ text: `${corner} at structural risk`, detail, color: '#C4162A', priority: 0 })
    }

    // Single corner dominance — one tyre >10% ahead of second-worst
    const sorted4 = [flW, frW, rlW, rrW].sort((a, b) => b - a)
    if (maxWear - sorted4[1] > 10) {
      if (maxWear === flW) candidates.push({
        text: 'Front-left is the limit tyre',
        detail: 'Sustained right-handers push 70–80% of axle load onto the outside front — classic Silverstone/Lusail/Suzuka pattern; monitor cliff lap closely',
        color: '#FF9830', priority: 1,
      })
      else if (maxWear === frW) candidates.push({
        text: 'Front-right is the limit tyre',
        detail: 'Sustained left-handers are loading the outside front corner — ease entry speed and let steering unwind earlier',
        color: '#FF9830', priority: 1,
      })
      else if (maxWear === rlW) candidates.push({
        text: 'Rear-left overheating under throttle',
        detail: 'Inside rear spinning on corner exit — differential too open; ease the initial throttle application and smooth the trace',
        color: '#FF9830', priority: 1,
      })
      else candidates.push({
        text: 'Rear-right taking excess mid-corner load',
        detail: 'Outside rear under simultaneous lateral and drive load — ease through fast right-handers; smooth the corner entry',
        color: '#FF9830', priority: 1,
      })
    }

    // Diagonal cross-wear — handling balance signature
    if (Math.abs(diagImb) > 8) {
      candidates.push(diagImb > 0
        ? {
            text: 'Cross-diagonal wear — FL + RR',
            detail: 'Car rotating too aggressively; oversteer tendency loading opposite corners — try stiffening rear ARB or easing initial throttle on exit',
            color: '#FF9830', priority: 2,
          }
        : {
            text: 'Cross-diagonal wear — FR + RL',
            detail: 'Car pushing at apex; understeer tendency loading opposite corners — ease brake bias or add front wing to reduce entry push',
            color: '#FF9830', priority: 2,
          }
      )
    }

    // Rear asymmetry when rears are the dominant axle
    if (rearWear > frontWear && Math.abs(rearImb) > 8) {
      candidates.push(rlW > rrW
        ? {
            text: 'Rear-left overheating under traction',
            detail: 'Inside rear losing grip first on exit — differential too open; reduce wheelspin by easing the initial throttle application',
            color: '#FF9830', priority: 2,
          }
        : {
            text: 'Rear-right absorbing peak lateral load',
            detail: 'Outside rear carrying combined slip through right-handers — ease mid-corner throttle and smooth the steering transition',
            color: '#FF9830', priority: 2,
          }
      )
    }

    // Axle imbalance — front vs rear
    if (Math.abs(axleImb) > 8) {
      candidates.push(axleImb > 0
        ? {
            text: 'Front axle under peak stress',
            detail: 'Braking zones and lateral load wearing fronts faster than rears — shift brake bias rearward or trim front wing to reduce load',
            color: '#FF9830', priority: 3,
          }
        : {
            text: 'Rear tyres absorbing excess torque load',
            detail: 'Combined slip from drive torque and lateral force — reduce wheelspin on corner exit; smooth the throttle trace',
            color: '#FF9830', priority: 3,
          }
      )
    }

    // Side imbalance — lowest priority as it is implied by more specific warnings above
    if (Math.abs(sideImb) > 5) {
      candidates.push(sideImb > 0
        ? {
            text: 'Right tyres degrading faster',
            detail: 'Left-hand corners loading the right-side contact patches harder — ease entry speed and let the car flow more smoothly',
            color: '#FF9830', priority: 3,
          }
        : {
            text: 'Left tyres degrading faster',
            detail: 'Right-hand corners loading the left-side contact patches harder — ease entry speed and let the car flow more smoothly',
            color: '#FF9830', priority: 3,
          }
      )
    }

    const PRIORITY_COLOR: Record<WarnPriority, string> = {
      0: '#C4162A',
      1: '#C4162A',
      2: '#FF9830',
      3: '#FADE2A',
    }
    const wearWarnings: WearWarning[] = candidates
      .sort((a, b) => a.priority - b.priority)
      .slice(0, 2)
      .map(({ text, detail, priority }) => ({ text, detail, color: PRIORITY_COLOR[priority] }))

    return {
      avgWear, wearPerLap,
      limitCorner, limitWear, limitWearPerLap,
      estimatedCliffLap: consCliffLap, lapsUntilCliff: consLapsLeft,
      conservative, aggressive,
      currentCompound, isMonaco, cliffPct, wearWarnings,
      tyreWears: { fl: flW, fr: frW, rl: rlW, rr: rrW },
    }
  }, [lap, session, status, damage, tyreSets, isRaceSession, hasTyreData, tn])

  // ─── Pace targets ─────────────────────────────────────────────────────────
  // stableLapPace returns the SAME object reference when last_lap_ms values
  // haven't changed, so stintTargets won't recompute on every 20Hz timing frame.
  const lapPaceRef = useRef<{ playerLapMs: number; behindLapMs: number } | null>(null)
  const stableLapPace = useMemo(() => {
    if (!timing || !lap) { lapPaceRef.current = null; return null }
    const active = timing.cars.filter(c => c.result_status === 2 && c.position > 0)
    const player = active.find(c => c.idx === timing.player_idx)
    if (!player) { lapPaceRef.current = null; return null }
    const playerLapMs = timing.cars.find(c => c.idx === timing.player_idx)?.last_lap_ms ?? lap.last_lap_ms
    const behindCar   = active.find(c => c.position === player.position + 1)
    const behindLapMs = behindCar?.last_lap_ms ?? 0
    const prev = lapPaceRef.current
    if (prev && prev.playerLapMs === playerLapMs && prev.behindLapMs === behindLapMs) return prev
    const next = { playerLapMs, behindLapMs }
    lapPaceRef.current = next
    return next
  }, [timing, lap])

  const stintTargets = useMemo(() => {
    if (!strategyData || !stableLapPace || !session) return null
    const { playerLapMs, behindLapMs } = stableLapPace
    if (playerLapMs <= 0 || playerLapMs > 600_000) return null

    const fittedSet = tyreSets?.sets.find(s => s.fitted)
    const totalRemainingLaps = Math.max(1, session.total_laps - (lap?.lap_num ?? 0))

    const calcForResult = (result: StrategyResult, isAggressive: boolean): (StintTargetInfo | null)[] => {
      const perLapOffset = isAggressive ? pitLoss.totalMs / totalRemainingLaps : 0
      return result.stints.map((stint, i) => {
        if (i === 0) {
          if (behindLapMs > 0 && behindLapMs < 600_000) {
            const targetMs       = behindLapMs - perLapOffset
            const lastLapDeltaMs = playerLapMs - targetMs
            return { targetMs, isEstimate: false, lastLapDeltaMs }
          }
          return { targetMs: playerLapMs, isEstimate: false, lastLapDeltaMs: 0 }
        }
        const matchingSet = tyreSets?.sets.find(s =>
          !s.fitted && s.available && s.actual_compound === stint.actualCompound
        )
        if (matchingSet) {
          const deltaVsCurrent = matchingSet.lap_delta_ms - (fittedSet?.lap_delta_ms ?? 0)
          const estimatedMs    = Math.max(60_000, playerLapMs + deltaVsCurrent - perLapOffset)
          return { targetMs: estimatedMs, isEstimate: true, lastLapDeltaMs: null }
        }
        return null
      })
    }

    return {
      conservative: calcForResult(strategyData.conservative, false),
      aggressive:   calcForResult(strategyData.aggressive,   true),
    }
  }, [strategyData, stableLapPace, tyreSets, session, lap, pitLoss])

  // ─── Per-lap target tables (ported from native StrategyPage) ───────────────
  // The live plan only carries the current + future stints, so each stint is
  // recorded the moment it begins (compound + frozen Required base). Completed
  // stints are kept on screen instead of vanishing after a pit. Actuals are gated
  // to laps completed as of the playhead (n < lap_num), so this is identical live
  // and under playback (lapTimesByNum is seek-correct in playback).
  const consFrozenReqMs = useRef<Map<number, number>>(new Map())
  const aggFrozenReqMs  = useRef<Map<number, number>>(new Map())
  const consPast        = useRef<PastStint[]>([])
  const aggPast         = useRef<PastStint[]>([])
  const tableLapComputed = useRef<number>(-1)
  const tableSessionKey  = useRef<string | null>(null)
  const [consDisplay, setConsDisplay] = useState<DisplayStint[]>([])
  const [aggDisplay,  setAggDisplay]  = useState<DisplayStint[]>([])

  // Coalesce the recompute to once per animation frame, reading the SETTLED inputs.
  // lap_num (LapData) and tyre_age (CarStatus) arrive in separate packets/renders;
  // recomputing per-render can observe a transient where lap_num advanced but
  // tyre_age hasn't, making openingStart = lapNum - tyreAge drift by a lap and
  // record a phantom stint boundary. Native avoids this by reading both together on
  // a refresh tick — this rAF coalescing replicates that.
  const tableRaf    = useRef<number | null>(null)
  const tableInputs = useRef({ strategyData, stintTargets, lap, session, status, lapTimesByNum, pitLoss })
  tableInputs.current = { strategyData, stintTargets, lap, session, status, lapTimesByNum, pitLoss }
  useEffect(() => () => {
    if (tableRaf.current != null) { cancelAnimationFrame(tableRaf.current); tableRaf.current = null }
  }, [])

  useEffect(() => {
    // Cancel any not-yet-fired recompute and reschedule, so multiple packet renders
    // within one frame coalesce into a single recompute reading the settled inputs.
    if (tableRaf.current != null) cancelAnimationFrame(tableRaf.current)
    tableRaf.current = requestAnimationFrame(() => {
      tableRaf.current = null
      const { strategyData, stintTargets, lap, session, status, lapTimesByNum, pitLoss } = tableInputs.current

    // Reset all cross-lap tracking only on a genuinely new session. Keyed on a
    // STABLE identity (track + session type) — not session.ts, which is a per-packet
    // timestamp that changes ~every second and would wipe the history mid-race.
    const sessionKey = session ? `${session.track_id}/${session.session_type}` : null
    if (sessionKey !== tableSessionKey.current) {
      consFrozenReqMs.current = new Map()
      aggFrozenReqMs.current  = new Map()
      consPast.current = []
      aggPast.current  = []
      tableLapComputed.current = -1
      tableSessionKey.current = sessionKey
      setConsDisplay([])
      setAggDisplay([])
      // fall through: build immediately if this frame already has valid data
    }
    if (!strategyData || !stintTargets || !lap || !session || !status) return

    const lapNum     = lap.lap_num
    const totalLaps  = session.total_laps
    const tyreAge    = status.tyre_age_laps ?? 0
    const openingStart = Math.max(1, lapNum - tyreAge)

    // Record the current opening stint into the per-variant history (idempotent —
    // its true start only jumps forward on a pit). Keeps the open stint's planned
    // length live until the next stint pushes in front of it.
    const recordStint = (
      res: StrategyResult, targets: (StintTargetInfo | null)[],
      frozen: Map<number, number>, past: PastStint[],
    ): boolean => {
      if (!res.stints.length || !targets.length || !targets[0]) return false
      let added = false
      if (past.length === 0 || openingStart > past[past.length - 1].startLap) {
        if (!frozen.has(openingStart)) frozen.set(openingStart, targets[0].targetMs)
        past.push({
          startLap: openingStart, reqBaseMs: frozen.get(openingStart)!,
          compoundName: res.stints[0].compoundName, color: res.stints[0].tyreColor,
          postPit: past.length > 0, expectedLaps: 0,
        })
        added = true
      }
      const s0 = res.stints[0]
      const s0end = s0.isLast ? totalLaps : (s0.pitLap ?? totalLaps)
      past[past.length - 1].expectedLaps = Math.max(1, s0end - openingStart + 1)
      return added
    }

    const changedC = recordStint(strategyData.conservative, stintTargets.conservative, consFrozenReqMs.current, consPast.current)
    const changedA = recordStint(strategyData.aggressive,   stintTargets.aggressive,   aggFrozenReqMs.current,  aggPast.current)
    const stintsChanged = changedC || changedA

    // Only rebuild the (heavy) tables when the lap advances or a stint was recorded
    // — not on every 20Hz frame. Within a lap the table stays frozen even if the
    // underlying plan jitters, matching native (avoids row churn / flicker).
    if (lapNum === tableLapComputed.current && !stintsChanged) return

    // Pitting costs time on two laps: the out-lap (first lap of a post-pit stint)
    // and the in-lap (last lap of a stint that ends in a pit).
    const computeRows = (
      start: number, end: number, base: number, postPit: boolean, prePit: boolean,
      raceCum: { v: number },
    ): TableLapRow[] => {
      const rows: TableLapRow[] = []
      let cum = 0   // cumulative delta over the stint, through the previous lap
      for (let n = start; n > 0 && n <= end; n++) {
        const requiredMs = base
          + ((postPit && n === start) ? pitLoss.outlapMs : 0)
          + ((prePit  && n === end)   ? pitLoss.inlapMs  : 0)
        // Adjusted required = base minus the running buffer: time banked raises the
        // time you may run; time lost lowers it.
        const actualRequiredMs = requiredMs - cum
        const actual = lapTimesByNum[n]
        const row: TableLapRow = {
          lapNum: n, requiredMs, actualRequiredMs,
          actualMs: 0, deltaLapMs: 0, deltaStintMs: 0, deltaTotalMs: 0, hasActual: false,
        }
        if (n < lapNum && actual && actual > 0) {
          row.hasActual    = true
          row.actualMs     = actual
          row.deltaLapMs   = actual - requiredMs
          cum             += row.deltaLapMs
          row.deltaStintMs = cum
          raceCum.v       += row.deltaLapMs
          row.deltaTotalMs = raceCum.v
        }
        rows.push(row)
      }
      return rows
    }

    const buildDisplay = (
      res: StrategyResult, targets: (StintTargetInfo | null)[],
      frozen: Map<number, number>, past: PastStint[],
    ): DisplayStint[] => {
      const out: DisplayStint[] = []
      const raceCum = { v: 0 }   // Δ TOTAL threads across every stint in lap order
      const done = past.filter(ps => ps.startLap <= lapNum)

      // Completed stints — every recorded one but the current (last) opening stint.
      for (let k = 0; k + 1 < done.length; k++) {
        const ps = done[k]
        const start = ps.startLap, end = done[k + 1].startLap - 1
        if (end < start) continue
        out.push({
          compoundName: ps.compoundName, color: ps.color,
          stintNumber: out.length + 1, startLap: start, endLap: end,
          lapCount: ps.expectedLaps > 0 ? ps.expectedLaps : end - start + 1,
          actualLaps: end - start + 1, isLast: false,
          rows: computeRows(start, end, ps.reqBaseMs, ps.postPit, true, raceCum),
        })
      }

      // Current + future stints from the live forward plan.
      const pittedBefore = done.length > 1
      for (let i = 0; i < res.stints.length; i++) {
        const st = res.stints[i]
        const tg = i < targets.length ? targets[i] : null
        const isOpening = i === 0
        const start = isOpening ? openingStart : st.startLap
        const end   = st.isLast ? totalLaps : (st.pitLap ?? totalLaps)
        const d: DisplayStint = {
          compoundName: st.compoundName, color: st.tyreColor,
          stintNumber: out.length + 1, startLap: start, endLap: end,
          lapCount: isOpening ? Math.max(1, end - start + 1) : st.lapCount,
          actualLaps: 0, isLast: st.isLast, rows: [],
        }
        if (tg && tg.targetMs > 0) {
          const postPit = isOpening ? pittedBefore : true
          const base = frozen.has(start) ? frozen.get(start)! : tg.targetMs
          d.rows = computeRows(start, end, base, postPit, !st.isLast, raceCum)
        }
        for (const r of d.rows) if (r.hasActual) d.actualLaps++
        out.push(d)
      }
      return out
    }

    setConsDisplay(buildDisplay(strategyData.conservative, stintTargets.conservative, consFrozenReqMs.current, consPast.current))
    setAggDisplay(buildDisplay(strategyData.aggressive,  stintTargets.aggressive,  aggFrozenReqMs.current,  aggPast.current))
    tableLapComputed.current = lapNum
    })
  }, [strategyData, stintTargets, lap, session, status, lapTimesByNum, pitLoss])

  // ─── Pit count tracking ───────────────────────────────────────────────────
  const pitCountsRef     = useRef<Map<number, number>>(new Map())
  const prevDrvStatusRef = useRef<Map<number, number>>(new Map())
  const sessionTsRef     = useRef<string | null>(null)
  const [pitCounts, setPitCounts] = useState<Map<number, number>>(new Map())

  useEffect(() => {
    if (!timing || !lap) return
    if (session?.ts !== sessionTsRef.current) {
      pitCountsRef.current     = new Map()
      prevDrvStatusRef.current = new Map()
      sessionTsRef.current     = session?.ts ?? null
      setPitCounts(new Map())
      return
    }
    let changed = false
    for (const car of timing.cars) {
      const prev = prevDrvStatusRef.current.get(car.idx) ?? -1
      if (prev === 2 && car.driver_status === 3 && lap.lap_num > 1) {
        const cur = pitCountsRef.current.get(car.idx) ?? 0
        pitCountsRef.current.set(car.idx, cur + 1)
        changed = true
      }
      prevDrvStatusRef.current.set(car.idx, car.driver_status)
    }
    if (changed) setPitCounts(new Map(pitCountsRef.current))
  }, [timing, lap, session])

  // ─── Rivals tracking ──────────────────────────────────────────────────────
  const rivalsRef           = useRef<{ aheadIdx: number | null; behindIdx: number | null } | null>(null)
  const prevLapRef          = useRef<number | null>(null)
  const rivalSessionTsRef   = useRef<string | null>(null)
  const [rivals, setRivals] = useState<{ aheadIdx: number | null; behindIdx: number | null } | null>(null)

  useEffect(() => {
    if (!lap || !timing) return
    if (session?.ts !== rivalSessionTsRef.current) {
      rivalsRef.current         = null
      prevLapRef.current        = null
      rivalSessionTsRef.current = session?.ts ?? null
      setRivals(null)
      return
    }
    if (prevLapRef.current === 1 && lap.lap_num === 2 && rivalsRef.current === null) {
      const active = timing.cars.filter(c => c.result_status === 2 && c.position > 0)
      const player = active.find(c => c.idx === timing.player_idx)
      if (player) {
        const aheadCar  = active.find(c => c.position === player.position - 1)
        const behindCar = active.find(c => c.position === player.position + 1)
        const r = { aheadIdx: aheadCar?.idx ?? null, behindIdx: behindCar?.idx ?? null }
        rivalsRef.current = r
        setRivals(r)
      }
    }
    prevLapRef.current = lap.lap_num
  }, [lap, timing, session])

  // ─── Undercut / Overcut analysis ──────────────────────────────────────────
  const strategyCall = useMemo((): StrategyCall | null => {
    if (!strategyData || !timing || !lap || !isRaceSession) return null
    const active = timing.cars.filter(c => c.result_status === 2 && c.position > 0)
    const player = active.find(c => c.idx === timing.player_idx)
    if (!player) return null

    const playerTyreAge = status?.tyre_age_laps ?? 0
    const freshGainMs   = strategyData.conservative.stints.length > 1
      ? Math.max(0, (() => {
          const fittedSet  = tyreSets?.sets.find(s => s.fitted)
          const WET        = new Set([7, 8])
          const isWet      = (session?.weather ?? 0) >= 3
          const pool       = (tyreSets?.sets ?? [])
            .filter(s => s.available && !s.fitted && s.usable_life > 0)
            .filter(s => isWet ? WET.has(s.actual_compound) : !WET.has(s.actual_compound))
          const best = pool.sort((a, b) => a.lap_delta_ms - b.lap_delta_ms)[0]
          return best ? (fittedSet?.lap_delta_ms ?? 0) - best.lap_delta_ms : 0
        })())
      : 0
    const PIT_COST_MS = 22_000

    // Undercut: car immediately ahead
    const carAhead = active.find(c => c.position === player.position - 1)
    if (carAhead) {
      const aheadGapMs    = player.gap_ms - carAhead.gap_ms
      const rivalTyreAge  = allStatus?.cars.find(c => c.idx === carAhead.idx)?.tyre_age_laps ?? 0
      const crossoverLaps = freshGainMs > 0 ? Math.ceil(PIT_COST_MS / freshGainMs) : 999
      if (
        aheadGapMs > 0 &&
        aheadGapMs < 3_000 &&
        freshGainMs > 200 &&
        rivalTyreAge >= playerTyreAge + 3 &&
        crossoverLaps <= 10
      ) {
        return {
          type: 'undercut',
          targetIdx: carAhead.idx,
          targetName: driverName(participants, carAhead.idx),
          gapMs: aheadGapMs,
          crossoverLaps,
        }
      }
    }

    // Overcut: car immediately behind
    const carBehind = active.find(c => c.position === player.position + 1)
    if (carBehind && strategyData.lapsUntilCliff >= 3) {
      const behindGapMs   = carBehind.gap_ms - player.gap_ms
      const rivalTyreAge  = allStatus?.cars.find(c => c.idx === carBehind.idx)?.tyre_age_laps ?? 0
      const isOnInlap     = carBehind.driver_status === 2
      if (
        behindGapMs >= 0 &&
        behindGapMs < 3_000 &&
        (isOnInlap || rivalTyreAge >= playerTyreAge + 5)
      ) {
        return {
          type: 'overcut',
          targetIdx: carBehind.idx,
          targetName: driverName(participants, carBehind.idx),
          gapMs: behindGapMs,
        }
      }
    }

    return null
  }, [strategyData, timing, allStatus, pitCounts, lap, isRaceSession, status, tyreSets, session, participants])

  const tyreColor = strategyData ? strategyData.currentCompound.color : 'var(--text-secondary)'
  const tyreName  = strategyData ? strategyData.currentCompound.name  : '—'
  const avgWear   = strategyData?.avgWear ?? 0

  const blueAccent  = isDark ? '#5794F2' : '#0B57D0'
  const amberAccent = isDark ? '#FADE2A' : '#B06000'

  return (
    <div className="flex flex-col h-full overflow-hidden">

      {/* ── Header ── */}
      <StrategyHeader
        lapNum={lap?.lap_num ?? null}
        totalLaps={session?.total_laps ?? 0}
        tyreName={tyreName}
        tyreColor={tyreColor}
        wearPct={Math.round(avgWear)}
        tyreAge={status?.tyre_age_laps ?? null}
        wearPerLapStr={strategyData?.wearPerLap.toFixed(1) ?? '—'}
        hasStrategy={!!strategyData}
        cliffLap={strategyData?.estimatedCliffLap ?? 0}
        lapsUntilCliff={strategyData?.lapsUntilCliff ?? 0}
        compact={compact}
      />

      {/* ── Non-race session: full-area placeholder ── */}
      {!isRaceSession && (
        <Placeholder>
          <div className="text-[10px] font-medium uppercase tracking-widest text-[var(--text-secondary)]">Strategy</div>
          <div className="text-sm font-semibold text-[var(--text-primary)]">Race sessions only</div>
          <div className="text-xs text-[var(--text-secondary)]">Strategy suggestions are available during Race, Race 2, and Race 3 sessions.</div>
        </Placeholder>
      )}

      {/* ── Race session: 3-column layout always rendered ── */}
      {isRaceSession && (
        <div className="flex flex-1 min-h-0 divide-x divide-[var(--border)]">

          {/* Conservative */}
          <div className="flex-1 min-w-0 h-full min-h-0">
            {consDisplay.length > 0 ? (
              <StintColumn
                display={consDisplay}
                stops={strategyData?.conservative.stops ?? Math.max(0, consDisplay.length - 1)}
                isMonaco={strategyData?.isMonaco ?? false}
                label="Conservative"
                accentColor={blueAccent}
              />
            ) : (
              <div className="flex items-center justify-center h-full">
                <span className="text-sm font-semibold text-[var(--text-secondary)]">Waiting for tyre data…</span>
              </div>
            )}
          </div>

          {/* Aggressive */}
          <div className="flex-1 min-w-0 h-full min-h-0">
            {aggDisplay.length > 0 && (
              <StintColumn
                display={aggDisplay}
                stops={strategyData?.aggressive.stops ?? Math.max(0, aggDisplay.length - 1)}
                isMonaco={strategyData?.isMonaco ?? false}
                label="Aggressive"
                accentColor={amberAccent}
              />
            )}
          </div>

          {/* Right panel: Strategy call + Rivals + Position + Tyre wear */}
          <div className="w-72 shrink-0 overflow-y-auto flex flex-col divide-y divide-[var(--border)]">

            {strategyCall && (
              <StrategyCallPanel call={strategyCall} participants={participants} />
            )}

            {rivals && timing && (
              <RivalsPanel rivals={rivals} timing={timing} participants={participants} />
            )}

            {timing && (
              <PositionPanel timing={timing} participants={participants} pitCounts={pitCounts} />
            )}

            {damage && (
              <TyreWearPanel
                fl={Math.round(damage.tyre_wear_fl)}
                fr={Math.round(damage.tyre_wear_fr)}
                rl={Math.round(damage.tyre_wear_rl)}
                rr={Math.round(damage.tyre_wear_rr)}
                wearWarnings={strategyData?.wearWarnings ?? EMPTY_WARNINGS}
              />
            )}


          </div>
        </div>
      )}
    </div>
  )
})

export default StrategyPanel
