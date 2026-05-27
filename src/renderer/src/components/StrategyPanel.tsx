import { useMemo, memo, useEffect, useRef, useState } from 'react'
import type { LapRow, StatusRow, DamageRow, TimingMsg, ParticipantsMsg, SessionMsg, TyreSetsMsg, TyreSetEntry } from '../types'
import { sessionAccent } from './SessionPanel'

// ─── Lookup tables ────────────────────────────────────────────────────────────

const COMPOUND_NAMES: Record<number, string> = {
  16: 'C5', 17: 'C4', 22: 'C6',
  18: 'C3', 19: 'C2', 20: 'C1', 21: 'C0',
   7: 'INT', 8: 'WET',
}

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
  isDark: boolean
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

// ─── Helpers ─────────────────────────────────────────────────────────────────

function driverName(p: ParticipantsMsg | null, idx: number): string {
  const d = p?.drivers.find(d => d.idx === idx)
  if (!d) return `Car ${idx}`
  const parts = d.name.trim().split(/\s+/)
  return parts[parts.length - 1].toUpperCase()
}

function wearColor(pct: number): string {
  if (pct < 50) return '#73BF69'
  if (pct < 70) return '#FADE2A'
  return '#C4162A'
}

function fmtLapTime(ms: number): string {
  if (ms <= 0 || ms > 600_000) return '—'
  const totalSecs = ms / 1000
  const mins = Math.floor(totalSecs / 60)
  const secs = totalSecs % 60
  return `${mins}:${secs.toFixed(1).padStart(4, '0')}`
}

// ─── Strategy calculation ────────────────────────────────────────────────────

function buildStints(
  lapNum: number,
  totalLaps: number,
  firstPitLap: number,
  currentCompound: { name: string; color: string; actual: number; visual: number },
  avgWear: number,
  availableSets: TyreSetEntry[],
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
      compoundName:   COMPOUND_NAMES[set.actual_compound] ?? String(set.actual_compound),
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
    compoundName:   COMPOUND_NAMES[nextSet.actual_compound] ?? String(nextSet.actual_compound),
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
): StrategyResult {
  if (result.stops >= 2) return result
  return forceExtraStop(result, availableSets, usedActualCompounds)
}

// ─── Sub-components ───────────────────────────────────────────────────────────

function CompoundChip({ name, color }: { name: string; color: string }) {
  return (
    <span
      className="text-[10px] font-bold px-2 py-0.5 rounded border shrink-0"
      style={{ color, borderColor: color, backgroundColor: color + '1a' }}
    >
      {name}
    </span>
  )
}

function StintTimeline({
  result, totalLaps, avgWear, isMonaco, label, accentColor, targets,
}: {
  result: StrategyResult
  totalLaps: number
  avgWear: number
  isMonaco: boolean
  label: string
  accentColor: string
  targets?: (StintTargetInfo | null)[]
}) {
  return (
    <div className="flex flex-col h-full overflow-y-auto">
      {/* Column headline */}
      <div className="px-5 pt-5 pb-4 shrink-0 border-b border-[var(--border)]">
        <div className="flex items-center justify-between mb-2">
          <span className="text-[10px] font-medium uppercase tracking-widest text-[var(--text-secondary)]">
            {label}
          </span>
          {isMonaco && (
            <span
              className="text-[9px] font-bold px-1.5 py-0.5 rounded border"
              style={{ color: '#FADE2A', borderColor: '#FADE2A', backgroundColor: 'rgba(250,222,42,0.08)' }}
            >
              Monaco 2-stop
            </span>
          )}
        </div>
        <div className="flex items-baseline gap-2">
          <span className="text-5xl font-black tabular-nums leading-none" style={{ color: accentColor }}>
            {result.stops}
          </span>
          <span className="text-sm font-medium text-[var(--text-secondary)]">
            stop{result.stops !== 1 ? 's' : ''}
          </span>
        </div>
      </div>

      {/* Stint timeline */}
      <div className="flex flex-col p-5 gap-0">
        {result.stints.map((stint, i) => {
          const target = targets?.[i] ?? null
          return (
            <div key={i}>
              <div className="rounded-md border border-[var(--border)] p-3.5 bg-[var(--bg-input)]/20">
                {/* Compound + wear state */}
                <div className="flex items-center justify-between mb-2">
                  <CompoundChip name={stint.compoundName} color={stint.tyreColor} />
                  <span className="text-[10px] text-[var(--text-secondary)] tabular-nums">
                    {i === 0 ? `${avgWear.toFixed(0)}% worn` : 'Fresh'}
                  </span>
                </div>

                {/* Lap count + range */}
                <div className="flex items-baseline gap-2">
                  <span className="text-xl font-black tabular-nums leading-none text-[var(--text-primary)]">
                    ~{stint.lapCount}L
                  </span>
                  <span className="text-[10px] text-[var(--text-secondary)] tabular-nums">
                    Lap {stint.startLap}–{stint.isLast ? totalLaps : stint.pitLap}
                  </span>
                </div>

                {/* Pace target row */}
                {target !== null && (
                  <div className="mt-2.5 pt-2.5 border-t border-[var(--border)]/50 flex items-center justify-between gap-2">
                    <span className="text-[9px] font-medium uppercase tracking-wider text-[var(--text-secondary)] shrink-0">
                      {target.isEstimate ? 'Est. pace' : 'Target'}
                    </span>
                    <div className="flex items-center gap-2 min-w-0">
                      <span
                        className="text-sm font-black tabular-nums leading-none"
                        style={{ color: target.isEstimate ? 'var(--text-secondary)' : 'var(--text-primary)' }}
                      >
                        {fmtLapTime(target.targetMs)}
                      </span>
                      {target.lastLapDeltaMs !== null && Math.abs(target.lastLapDeltaMs) > 50 && (
                        <span
                          className="text-[10px] font-bold tabular-nums shrink-0"
                          style={{ color: target.lastLapDeltaMs > 0 ? '#C4162A' : '#73BF69' }}
                        >
                          {target.lastLapDeltaMs > 0 ? '+' : '−'}{Math.abs(target.lastLapDeltaMs / 1000).toFixed(1)}s
                        </span>
                      )}
                      {target.lastLapDeltaMs !== null && Math.abs(target.lastLapDeltaMs) <= 50 && (
                        <span className="text-[10px] text-[var(--text-secondary)] shrink-0">on target</span>
                      )}
                    </div>
                  </div>
                )}
              </div>

              {/* Connector */}
              <div className="flex items-center gap-3 py-2 pl-5">
                <div className="w-px bg-[var(--border)] self-stretch" style={{ minHeight: 10 }} />
                {stint.isLast ? (
                  <span className="text-[10px] font-bold uppercase tracking-wider" style={{ color: '#73BF69' }}>
                    Finish · Lap {totalLaps}
                  </span>
                ) : (
                  <span className="text-[10px] font-bold uppercase tracking-wider" style={{ color: '#FADE2A' }}>
                    Pit · Lap {stint.pitLap}
                  </span>
                )}
              </div>
            </div>
          )
        })}
      </div>
    </div>
  )
}

function Placeholder({ children }: { children: React.ReactNode }) {
  return (
    <div className="flex-1 flex items-center justify-center text-[var(--text-secondary)]">
      <div className="flex flex-col items-center gap-3 max-w-xs text-center">
        {children}
      </div>
    </div>
  )
}

// ─── Main component ───────────────────────────────────────────────────────────

const StrategyPanel = memo(function StrategyPanel({
  lap, session, status, damage, timing, participants, tyreSets, isDark,
}: Props) {
  const accent = sessionAccent(session?.session_type ?? -1, isDark)

  const isRaceSession = session !== null && [15, 16, 17].includes(session.session_type)
  const hasEnoughLaps = true
  const hasTyreData   = !!status && status.tyre_age_laps >= 1

  const strategyData = useMemo(() => {
    if (!lap || !session || !status || !damage || !isRaceSession || session.total_laps <= 0 || !hasTyreData) return null

    const avgWear    = (damage.tyre_wear_fl + damage.tyre_wear_fr + damage.tyre_wear_rl + damage.tyre_wear_rr) / 4
    const wearPerLap = status.tyre_age_laps > 0 ? avgWear / status.tyre_age_laps : 2
    const cliffPct   = [16, 17, 18].includes(status.visual_compound) ? 80 : 70
    const isMonaco   = session.track_id === 5

    const WET_COMPOUNDS = new Set([7, 8])
    const isWetWeather  = session.weather >= 3
    const basePool = (tyreSets?.sets ?? [])
      .filter(s => s.available && !s.fitted && s.usable_life > 0)
      .filter(s => isWetWeather ? WET_COMPOUNDS.has(s.actual_compound) : !WET_COMPOUNDS.has(s.actual_compound))

    const conservativePool = [...basePool].sort((a, b) => b.usable_life - a.usable_life)
    const aggressivePool   = [...basePool].sort((a, b) => a.lap_delta_ms - b.lap_delta_ms)

    const currentCompound = {
      name:   COMPOUND_NAMES[status.tyre_compound] ?? String(status.tyre_compound),
      color:  VISUAL_COLORS[status.visual_compound] ?? '#ffffff',
      actual: status.tyre_compound,
      visual: status.visual_compound,
    }

    const consLapsLeft = Math.max(0, Math.floor((cliffPct - avgWear) / wearPerLap))
    const consCliffLap = lap.lap_num + consLapsLeft
    let conservative = buildStints(lap.lap_num, session.total_laps, consCliffLap, currentCompound, avgWear, conservativePool)
    if (isMonaco) {
      const usedCompounds = new Set(conservative.stints.map(s => s.actualCompound))
      conservative = applyMonacoRule(conservative, conservativePool, usedCompounds)
    }

    const aggWearRate    = wearPerLap * 1.2
    const aggLapsLeft    = Math.max(0, Math.floor((cliffPct - avgWear) / aggWearRate))
    const aggCliffLap    = lap.lap_num + aggLapsLeft
    const bestAggSet     = aggressivePool[0]
    const economicOptLap = bestAggSet
      ? Math.max(lap.lap_num + 1, session.total_laps - bestAggSet.usable_life)
      : aggCliffLap
    const aggPitLap = Math.min(aggCliffLap, economicOptLap)
    let aggressive = buildStints(lap.lap_num, session.total_laps, aggPitLap, currentCompound, avgWear, aggressivePool)
    if (aggressive.stops <= conservative.stops) {
      const usedCompounds = new Set(aggressive.stints.map(s => s.actualCompound))
      aggressive = forceExtraStop(aggressive, aggressivePool, usedCompounds)
    }
    if (isMonaco) {
      const usedCompounds = new Set(aggressive.stints.map(s => s.actualCompound))
      aggressive = applyMonacoRule(aggressive, aggressivePool, usedCompounds)
    }

    const flW = damage.tyre_wear_fl
    const frW = damage.tyre_wear_fr
    const rlW = damage.tyre_wear_rl
    const rrW = damage.tyre_wear_rr
    const leftWear  = (flW + rlW) / 2
    const rightWear = (frW + rrW) / 2
    const frontWear = (flW + frW) / 2
    const rearWear  = (rlW + rrW) / 2
    const sideImb   = rightWear - leftWear
    const axleImb   = frontWear - rearWear

    interface WearWarning { text: string; detail: string; color: string }
    const wearWarnings: WearWarning[] = []
    if (Math.abs(sideImb) > 5) {
      wearWarnings.push(sideImb > 0
        ? { text: 'Right tyres degrading faster', detail: 'Ease through left-hand corners — let the car flow more smoothly', color: '#FF9830' }
        : { text: 'Left tyres degrading faster',  detail: 'Ease through right-hand corners — let the car flow more smoothly', color: '#FF9830' }
      )
    }
    if (Math.abs(axleImb) > 8) {
      wearWarnings.push(axleImb > 0
        ? { text: 'Front tyres overloaded', detail: 'Ease brake pressure on corner entry',     color: '#FF9830' }
        : { text: 'Rear tyres overloaded',  detail: 'Ease throttle application on exit', color: '#FF9830' }
      )
    }

    return {
      avgWear, wearPerLap,
      estimatedCliffLap: consCliffLap, lapsUntilCliff: consLapsLeft,
      conservative, aggressive,
      currentCompound, isMonaco, cliffPct, wearWarnings,
      tyreWears: { fl: flW, fr: frW, rl: rlW, rr: rrW },
    }
  }, [lap, session, status, damage, tyreSets, isRaceSession, hasTyreData])

  // ─── Pace targets ─────────────────────────────────────────────────────────
  // For the current stint: target = car-behind's pace + gap spread over remaining stint laps.
  // This naturally recalculates every lap — if the player banks 2s, the gap widens and the
  // target automatically relaxes without any explicit "bank" state needed.
  // For future stints: estimate from the set's lap_delta_ms vs the currently fitted set.
  const stintTargets = useMemo(() => {
    if (!strategyData || !timing || !lap || !session) return null

    const active = timing.cars.filter(c => c.result_status === 2 && c.position > 0)
    const player  = active.find(c => c.idx === timing.player_idx)
    if (!player) return null

    const playerCar  = timing.cars.find(c => c.idx === timing.player_idx)
    const playerLapMs = playerCar?.last_lap_ms ?? lap.last_lap_ms
    if (!playerLapMs || playerLapMs <= 0 || playerLapMs > 600_000) return null

    const behindCar   = active.find(c => c.position === player.position + 1)
    const behindLapMs = behindCar?.last_lap_ms ?? 0
    const gapBehindMs = behindCar ? behindCar.gap_ms - player.gap_ms : null

    const fittedSet = tyreSets?.sets.find(s => s.fitted)

    const calcForResult = (result: StrategyResult): (StintTargetInfo | null)[] =>
      result.stints.map((stint, i) => {
        const stintEndLap       = stint.pitLap ?? session.total_laps
        const remainingStintLaps = stintEndLap - lap.lap_num

        if (i === 0) {
          // Current stint: hold-position target vs car behind
          if (
            behindLapMs > 0 && behindLapMs < 600_000 &&
            gapBehindMs !== null && gapBehindMs > 0 &&
            remainingStintLaps > 0
          ) {
            const targetMs       = behindLapMs + gapBehindMs / remainingStintLaps
            const lastLapDeltaMs = playerLapMs - targetMs
            return { targetMs, isEstimate: false, lastLapDeltaMs }
          }
          // No car behind threat: show own pace with no delta
          return { targetMs: playerLapMs, isEstimate: false, lastLapDeltaMs: 0 }
        }

        // Future stint: estimate via lap_delta_ms offset from current set
        const matchingSet = tyreSets?.sets.find(s =>
          !s.fitted && s.available && s.actual_compound === stint.actualCompound
        )
        if (matchingSet) {
          const deltaVsCurrent = matchingSet.lap_delta_ms - (fittedSet?.lap_delta_ms ?? 0)
          const estimatedMs    = Math.max(60_000, playerLapMs + deltaVsCurrent)
          return { targetMs: estimatedMs, isEstimate: true, lastLapDeltaMs: null }
        }
        return null
      })

    return {
      conservative: calcForResult(strategyData.conservative),
      aggressive:   calcForResult(strategyData.aggressive),
    }
  }, [strategyData, timing, lap, tyreSets, session])

  const positionData = useMemo(() => {
    if (!timing) return null
    const active = timing.cars.filter(c => c.result_status === 2 && c.position > 0)
    const player = active.find(c => c.idx === timing.player_idx)
    if (!player) return null
    const ahead  = active.find(c => c.position === player.position - 1)
    const behind = active.find(c => c.position === player.position + 1)
    return { player, ahead, behind }
  }, [timing])

  // ─── Gap trend for car behind ───────────────────────────────────────────────
  // Compare successive gap_ms deltas — no lap time involved, pure gap delta.
  // null = no car behind or not enough data yet.
  const prevBehindGapRef = useRef<number | null>(null)
  const [behindIsGaining, setBehindIsGaining] = useState<boolean | null>(null)

  useEffect(() => {
    if (!positionData?.behind || !positionData?.player) {
      prevBehindGapRef.current = null
      setBehindIsGaining(null)
      return
    }
    const currentGap = positionData.behind.gap_ms - positionData.player.gap_ms
    if (prevBehindGapRef.current !== null) {
      const delta = currentGap - prevBehindGapRef.current // negative = gap shrinking = they gaining
      if (Math.abs(delta) > 30) {
        setBehindIsGaining(delta < 0)
      }
    }
    prevBehindGapRef.current = currentGap
  }, [positionData])

  const tyreColor = strategyData ? strategyData.currentCompound.color : 'var(--text-secondary)'
  const tyreName  = strategyData ? strategyData.currentCompound.name  : '—'
  const avgWear   = strategyData?.avgWear ?? 0
  const wearBar   = wearColor(avgWear)

  const blueAccent  = isDark ? '#5794F2' : '#0B57D0'
  const amberAccent = isDark ? '#FADE2A' : '#B06000'

  return (
    <div className="flex flex-col h-full overflow-hidden">

      {/* ── Header ── */}
      <div
        className="shrink-0 flex divide-x divide-[var(--border)] border-b border-[var(--border)]"
        style={{ borderLeft: `4px solid ${accent}` }}
      >
        {/* Lap counter */}
        <div className="shrink-0 flex flex-col justify-center px-6 py-3">
          <span className="text-[10px] font-medium uppercase tracking-widest text-[var(--text-secondary)] mb-1">Lap</span>
          <div className="flex items-baseline gap-1.5">
            <span className="text-3xl font-black tabular-nums leading-none" style={{ color: accent }}>
              {lap?.lap_num ?? '—'}
            </span>
            <span className="text-base font-medium text-[var(--text-secondary)]">
              / {session?.total_laps && session.total_laps > 0 ? session.total_laps : '—'}
            </span>
          </div>
        </div>

        {/* Compound + wear */}
        <div className="flex-1 min-w-0 flex items-center gap-4 px-6 py-3">
          <CompoundChip name={tyreName} color={tyreColor} />
          <div className="flex-1 min-w-0">
            <div className="flex items-baseline justify-between gap-3 mb-1.5">
              <span className="text-lg font-black tabular-nums leading-none" style={{ color: wearBar }}>
                {avgWear.toFixed(0)}%
              </span>
              <span className="text-[10px] text-[var(--text-secondary)] tabular-nums shrink-0">
                {status ? `${status.tyre_age_laps}L · ${strategyData?.wearPerLap.toFixed(1) ?? '—'}%/L` : '—'}
              </span>
            </div>
            <div className="h-1.5 bg-[var(--border)] rounded-full overflow-hidden">
              <div
                className="h-full rounded-full transition-all duration-300"
                style={{ width: `${Math.min(100, avgWear)}%`, backgroundColor: wearBar }}
              />
            </div>
          </div>
        </div>

        {/* Tyre cliff */}
        <div className="shrink-0 flex flex-col justify-center px-6 py-3">
          <span className="text-[10px] font-medium uppercase tracking-widest text-[var(--text-secondary)] mb-1">Tyre Cliff</span>
          {strategyData ? (
            <div className="flex items-baseline gap-1.5">
              <span
                className="text-lg font-black tabular-nums leading-none"
                style={{ color: strategyData.lapsUntilCliff <= 5 ? '#C4162A' : strategyData.lapsUntilCliff <= 10 ? '#FADE2A' : 'var(--text-primary)' }}
              >
                Lap {strategyData.estimatedCliffLap}
              </span>
              <span className="text-[10px] text-[var(--text-secondary)]">+{strategyData.lapsUntilCliff}</span>
            </div>
          ) : (
            <span className="text-sm text-[var(--text-secondary)]">—</span>
          )}
        </div>
      </div>

      {/* ── Guard: non-race session ── */}
      {!isRaceSession && (
        <Placeholder>
          <div className="text-[10px] font-medium uppercase tracking-widest text-[var(--text-secondary)]">Strategy</div>
          <div className="text-sm font-semibold text-[var(--text-primary)]">Race sessions only</div>
          <div className="text-xs text-[var(--text-secondary)]">Strategy suggestions are available during Race, Race 2, and Race 3 sessions.</div>
        </Placeholder>
      )}

      {/* ── Guard: fewer than 5 laps ── */}
      {isRaceSession && !hasEnoughLaps && (
        <Placeholder>
          <div className="text-[10px] font-medium uppercase tracking-widest text-[var(--text-secondary)]">Strategy Locked</div>
          <div className="text-sm font-semibold text-[var(--text-primary)]">
            {5 - (lap?.lap_num ?? 0)} more lap{5 - (lap?.lap_num ?? 0) !== 1 ? 's' : ''} to unlock
          </div>
          <div className="w-48">
            <div className="h-1.5 bg-[var(--border)] rounded-full overflow-hidden">
              <div
                className="h-full rounded-full transition-all"
                style={{ width: `${((lap?.lap_num ?? 0) / 5) * 100}%`, backgroundColor: accent }}
              />
            </div>
            <div className="mt-1">
              <span className="text-[10px] text-[var(--text-secondary)]">{lap?.lap_num ?? 0} / 5 laps</span>
            </div>
          </div>
        </Placeholder>
      )}

      {/* ── Guard: waiting for tyre data ── */}
      {isRaceSession && hasEnoughLaps && (!hasTyreData || !strategyData) && (
        <Placeholder>
          <div className="text-sm font-semibold text-[var(--text-secondary)]">Waiting for tyre data…</div>
        </Placeholder>
      )}

      {/* ── Main content ── */}
      {isRaceSession && hasEnoughLaps && strategyData && (
        <div className="flex flex-1 min-h-0 divide-x divide-[var(--border)]">

          {/* Conservative */}
          <div className="flex-1 min-w-0 overflow-y-auto">
            <StintTimeline
              result={strategyData.conservative}
              totalLaps={session!.total_laps}
              avgWear={strategyData.avgWear}
              isMonaco={strategyData.isMonaco}
              label="Conservative"
              accentColor={blueAccent}
              targets={stintTargets?.conservative}
            />
          </div>

          {/* Aggressive */}
          <div className="flex-1 min-w-0 overflow-y-auto">
            <StintTimeline
              result={strategyData.aggressive}
              totalLaps={session!.total_laps}
              avgWear={strategyData.avgWear}
              isMonaco={strategyData.isMonaco}
              label="Aggressive"
              accentColor={amberAccent}
              targets={stintTargets?.aggressive}
            />
          </div>

          {/* Right panel: Position + Tyre wear */}
          <div className="w-72 shrink-0 overflow-y-auto flex flex-col divide-y divide-[var(--border)]">

            {/* Position */}
            <div>
              <div className="px-5 pt-5 pb-2">
                <span className="text-[10px] font-medium uppercase tracking-widest text-[var(--text-secondary)]">Position</span>
              </div>
              {positionData ? (
                <div className="flex flex-col divide-y divide-[var(--border)]">
                  {(([
                    positionData.ahead  ? { car: positionData.ahead,  role: 'ahead'  as const } : null,
                    { car: positionData.player, role: 'player' as const },
                    positionData.behind ? { car: positionData.behind, role: 'behind' as const } : null,
                  ].filter(Boolean)) as Array<{ car: typeof positionData.player; role: 'ahead' | 'player' | 'behind' }>).map(({ car, role }) => {
                    const isPlayer  = role === 'player'
                    const livery    = participants?.drivers.find(d => d.idx === car.idx)?.livery_color ?? '#8e8e8e'
                    const aheadGap  = positionData.ahead  ? positionData.player.gap_ms - positionData.ahead.gap_ms  : null
                    const behindGap = positionData.behind ? positionData.behind.gap_ms - positionData.player.gap_ms : null

                    let gapMs: number | null = null
                    let gapColor = 'var(--text-secondary)'
                    let statusChip: { label: string; color: string } | null = null

                    if (role === 'ahead' && aheadGap != null && aheadGap > 0) {
                      gapMs = aheadGap
                      gapColor = aheadGap > 2000 ? '#73BF69' : '#FADE2A'
                      statusChip = aheadGap > 2000
                        ? { label: 'SAFE',  color: '#73BF69' }
                        : { label: 'CLOSE', color: '#FADE2A' }
                    } else if (role === 'behind' && behindGap != null && behindGap > 0) {
                      gapMs = behindGap
                      if (behindGap < 1000) {
                        // Within 1s — imminent threat regardless of trend
                        gapColor = '#C4162A'
                        statusChip = { label: 'THREAT', color: '#C4162A' }
                      } else if (behindIsGaining === true) {
                        // Gap is shrinking — they're closing
                        gapColor = '#FADE2A'
                      } else {
                        // Gap is stable or growing — player pulling away
                        gapColor = '#73BF69'
                      }
                    }

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
                        {gapMs != null && (
                          <span
                            className="text-xs font-semibold tabular-nums shrink-0"
                            style={{ color: gapColor }}
                          >
                            {role === 'ahead' ? `+${(gapMs / 1000).toFixed(1)}s` : `-${(gapMs / 1000).toFixed(1)}s`}
                          </span>
                        )}
                        {statusChip && (
                          <span
                            className="text-[9px] font-bold px-1.5 py-0.5 rounded border shrink-0"
                            style={{ color: statusChip.color, borderColor: statusChip.color, backgroundColor: statusChip.color + '18' }}
                          >
                            {statusChip.label}
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

            {/* Tyre wear */}
            <div className="p-5 flex flex-col gap-3">
              <span className="text-[10px] font-medium uppercase tracking-widest text-[var(--text-secondary)]">Tyre Wear</span>

              <div className="grid grid-cols-2 gap-2">
                {([
                  { key: 'fl' as const, label: 'FL' },
                  { key: 'fr' as const, label: 'FR' },
                  { key: 'rl' as const, label: 'RL' },
                  { key: 'rr' as const, label: 'RR' },
                ]).map(({ key, label }) => {
                  const w = strategyData.tyreWears[key]
                  const c = wearColor(w)
                  return (
                    <div key={key} className="flex flex-col gap-1.5 p-2.5 rounded-md border border-[var(--border)]">
                      <div className="flex items-baseline justify-between">
                        <span className="text-[9px] text-[var(--text-secondary)] uppercase tracking-wider">{label}</span>
                        <span className="text-sm font-black tabular-nums leading-none" style={{ color: c }}>{w.toFixed(0)}%</span>
                      </div>
                      <div className="h-1 bg-[var(--border)] rounded-full overflow-hidden">
                        <div className="h-full rounded-full" style={{ width: `${Math.min(100, w)}%`, backgroundColor: c }} />
                      </div>
                    </div>
                  )
                })}
              </div>

              {strategyData.wearWarnings.length > 0 && (
                <div className="flex flex-col gap-2 pt-2 border-t border-[var(--border)]">
                  {strategyData.wearWarnings.map((w, i) => (
                    <div key={i} className="flex flex-col gap-0.5">
                      <span className="text-[10px] font-bold" style={{ color: w.color }}>⚠ {w.text}</span>
                      <span className="text-[10px] text-[var(--text-secondary)] leading-relaxed">{w.detail}</span>
                    </div>
                  ))}
                </div>
              )}
            </div>

          </div>
        </div>
      )}
    </div>
  )
})

export default StrategyPanel
