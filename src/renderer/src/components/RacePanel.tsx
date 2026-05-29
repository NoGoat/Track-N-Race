import { useRef, memo } from 'react'
import type { LapRow, StatusRow, TimingCar, DriverInfo, CarStatusEntry } from '../types'

interface Props {
  lap: LapRow | null
  status: StatusRow | null
  selectedCar: TimingCar | null
  selectedDriver: DriverInfo | null
  selectedCarStatus: CarStatusEntry | null
  playerIdx: number | null
  isDark: boolean
}

function fmtMs(ms: number): string {
  if (ms <= 0) return '--:--.---'
  const m     = Math.floor(ms / 60_000)
  const s     = Math.floor((ms % 60_000) / 1000)
  const mills = ms % 1000
  return `${m}:${String(s).padStart(2, '0')}.${String(mills).padStart(3, '0')}`
}

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

const ERS_MODES  = ['None', 'Auto', 'Hotlap', 'Overtake']
const ERS_COLORS = ['text-[var(--text-secondary)]', 'text-[#5794F2]', 'text-[var(--compound-medium)]', 'text-[#C4162A]']
const FUEL_MIX   = ['Lean', 'Standard', 'Rich', 'Max power']
const PIT_STATUS = ['', 'Pitting', 'In pit lane']


function SectionLabel({ children }: { children: React.ReactNode }) {
  return (
    <div className="text-[10px] text-[var(--text-secondary)] uppercase tracking-widest mb-2">{children}</div>
  )
}

function Panel({ children, className = '' }: { children: React.ReactNode; className?: string }) {
  return (
    <div className={`p-4 ${className}`}>
      {children}
    </div>
  )
}

const RacePanel = memo(function RacePanel({ lap, status, selectedCar, selectedDriver, selectedCarStatus, playerIdx, isDark }: Props) {
  // Hooks must come before any early return
  const prevLapTsRef    = useRef<string | null>(null)
  const prevLapRef      = useRef<LapRow | null>(null)
  const frozenRef       = useRef<{ s1: number; s2: number; s3: number; exp: number } | null>(null)
  const lastS3Ref       = useRef<number>(0)
  const s3SnapshotRef   = useRef<{ s1: number; s2: number; lap: number } | null>(null)

  // Process new lap data once per message (ts-guard is safe under StrictMode double-invoke)
  if (lap && lap.ts !== prevLapTsRef.current) {
    prevLapTsRef.current = lap.ts
    const prev = prevLapRef.current
    const now  = Date.now()

    // Capture s1+s2 when entering sector 3 so we can compute s3 on lap completion
    if (lap.sector === 2 && lap.s1_ms > 0 && lap.s2_ms > 0) {
      if (!s3SnapshotRef.current || s3SnapshotRef.current.lap !== lap.lap_num)
        s3SnapshotRef.current = { s1: lap.s1_ms, s2: lap.s2_ms, lap: lap.lap_num }
    }

    // Lap just completed: compute s3, freeze previous sector times for 7 s
    if (prev && lap.lap_num > prev.lap_num) {
      const snap = s3SnapshotRef.current
      const s3   = (snap && snap.lap === prev.lap_num && lap.last_lap_ms > 0)
        ? Math.max(0, lap.last_lap_ms - snap.s1 - snap.s2) : 0
      lastS3Ref.current  = s3
      frozenRef.current  = { s1: prev.s1_ms, s2: prev.s2_ms, s3, exp: now + 7_000 }
    }

    prevLapRef.current = lap
  }

  // Determine what sector times to display (player's own data)
  const now    = Date.now()
  const frozen = frozenRef.current
  const useFrozen = !!lap && !!frozen && now < frozen.exp

  const displayS1  = useFrozen ? frozen!.s1 : (lap?.s1_ms ?? 0)
  const displayS2  = useFrozen ? frozen!.s2 : (lap?.s2_ms ?? 0)
  const displayS3  = useFrozen ? frozen!.s3 : 0
  const s1Done     = useFrozen ? frozen!.s1 > 0 : (!!lap && lap.sector >= 1)
  const s2Done     = useFrozen ? frozen!.s2 > 0 : (!!lap && lap.sector >= 2)
  const s3Done     = displayS3 > 0

  // When a different driver is selected, show their data instead of the player's
  const viewingOther = selectedCar !== null && selectedCar.idx !== playerIdx
  const teamColor    = selectedDriver?.livery_color ?? '#8e8e8e'

  // Active status: selected driver's when viewing other, player's otherwise
  const activeStatus = viewingOther ? selectedCarStatus : status


  const ersPct = activeStatus?.ers_pct ?? 0
  const ersBarWidth = Math.min(100, Math.max(0, ersPct))
  const ersBarColor =
    ersPct > 60 ? (isDark ? '#5794F2' : '#0B57D0') :
    ersPct > 30 ? (isDark ? '#d4ad04' : '#B7950B') :
    '#C4162A'

  const tyreName  = activeStatus ? (COMPOUND_NAMES[activeStatus.tyre_compound] ?? String(activeStatus.tyre_compound)) : null
  const tyreColor = activeStatus ? (VISUAL_COLORS[activeStatus.visual_compound] ?? '#ffffff') : '#ffffff'

  return (
    <div className="flex flex-col divide-y divide-[var(--border)] border-b border-[var(--border)]">

      {/* ── Lap timing ── */}
      <Panel>
        <div className="flex items-center justify-between mb-2">
          <SectionLabel>Timing</SectionLabel>
          {selectedDriver && (
            <div className="flex items-center gap-1.5 mb-2">
              <div className="w-1.5 h-4 rounded-full shrink-0" style={{ background: teamColor }} />
              <span className="text-xs font-bold text-[var(--text-primary)] truncate max-w-[120px]">{selectedDriver.name}</span>
            </div>
          )}
        </div>

        {viewingOther && selectedCar ? (
          <>
            <div className="flex items-center justify-between mb-3">
              <div>
                <div className="text-[10px] text-[var(--text-secondary)]">Lap {selectedCar.lap_num}</div>
                <div className="text-2xl font-black text-[var(--text-primary)] tabular-nums">
                  P{selectedCar.position}
                </div>
              </div>
              <div className="flex gap-1 flex-wrap justify-end">
                {selectedCar.pit_status > 0 && (
                  <div className="text-xs font-bold text-[var(--compound-medium)] bg-[var(--compound-medium)]/10 border border-[var(--compound-medium)] px-2 py-0.5 rounded">
                    {PIT_STATUS[selectedCar.pit_status]}
                  </div>
                )}
                {selectedCar.lap_invalid && (
                  <div className="text-xs font-bold text-[#C4162A] bg-[#C4162A]/10 border border-[#C4162A] px-2 py-0.5 rounded">
                    INVALID
                  </div>
                )}
              </div>
            </div>

            <div className="space-y-1.5">
              <div>
                <div className="text-[9px] text-[var(--text-secondary)] uppercase tracking-wider">Current</div>
                <div className="text-xl font-bold text-[var(--text-primary)] tabular-nums">
                  {fmtMs(selectedCar.current_lap_ms)}
                </div>
              </div>
              <div>
                <div className="text-[9px] text-[var(--text-secondary)] uppercase tracking-wider">Last lap</div>
                <div className="text-lg font-bold text-[var(--text-secondary)] tabular-nums">
                  {fmtMs(selectedCar.last_lap_ms)}
                </div>
              </div>
            </div>

            <div className="mt-3 pt-3 -mx-4 px-4 border-t border-[var(--border)] grid grid-cols-3 gap-2">
              {[
                { label: 'S1', ms: selectedCar.s1_ms, done: selectedCar.sector >= 1 && selectedCar.s1_ms > 0 },
                { label: 'S2', ms: selectedCar.s2_ms, done: selectedCar.sector >= 2 && selectedCar.s2_ms > 0 },
                { label: 'S3', ms: 0, done: false },
              ].map(s => (
                <div key={s.label}>
                  <div className="text-[9px] text-[var(--text-secondary)]">{s.label}</div>
                  <div className={`text-xs font-bold tabular-nums ${s.done && s.ms > 0 ? 'text-[var(--text-primary)]' : 'text-[var(--text-muted)]'}`}>
                    {s.done && s.ms > 0 ? fmtMs(s.ms) : '–:––.–––'}
                  </div>
                </div>
              ))}
            </div>

            {selectedCar.penalties_s > 0 && (
              <div className="mt-2 text-xs text-[#C4162A]">+{selectedCar.penalties_s}s penalty</div>
            )}
          </>
        ) : lap ? (
          <>
            <div className="flex items-center justify-between mb-3">
              <div>
                <div className="text-[10px] text-[var(--text-secondary)]">Lap {lap.lap_num}</div>
                <div className="text-2xl font-black text-[var(--text-primary)] tabular-nums">
                  P{lap.position}
                </div>
              </div>
              {lap.pit_status > 0 && (
                <div className="text-xs font-bold text-[var(--compound-medium)] bg-[var(--compound-medium)]/10 border border-[var(--compound-medium)] px-2 py-0.5 rounded">
                  {PIT_STATUS[lap.pit_status]}
                </div>
              )}
              {lap.lap_invalid && (
                <div className="text-xs font-bold text-[#C4162A] bg-[#C4162A]/10 border border-[#C4162A] px-2 py-0.5 rounded">
                  INVALID
                </div>
              )}
            </div>

            <div className="space-y-1.5">
              <div>
                <div className="text-[9px] text-[var(--text-secondary)] uppercase tracking-wider">Current</div>
                <div className="text-xl font-bold text-[var(--text-primary)] tabular-nums">
                  {fmtMs(lap.current_lap_ms)}
                </div>
              </div>
              <div>
                <div className="text-[9px] text-[var(--text-secondary)] uppercase tracking-wider">Last lap</div>
                <div className="text-lg font-bold text-[var(--text-secondary)] tabular-nums">
                  {fmtMs(lap.last_lap_ms)}
                </div>
              </div>
            </div>

            {/* Sectors */}
            <div className="mt-3 pt-3 -mx-4 px-4 border-t border-[var(--border)] grid grid-cols-3 gap-2">
              {[
                { label: 'S1', ms: displayS1, done: s1Done },
                { label: 'S2', ms: displayS2, done: s2Done },
                { label: 'S3', ms: displayS3, done: s3Done },
              ].map(s => (
                <div key={s.label}>
                  <div className="text-[9px] text-[var(--text-secondary)]">{s.label}</div>
                  <div className={`text-xs font-bold tabular-nums ${s.done && s.ms > 0 ? 'text-[var(--text-primary)]' : 'text-[var(--text-muted)]'}`}>
                    {s.done && s.ms > 0 ? fmtMs(s.ms) : '–:––.–––'}
                  </div>
                </div>
              ))}
            </div>

            {lap.penalties_s > 0 && (
              <div className="mt-2 text-xs text-[#C4162A]">+{lap.penalties_s}s penalty</div>
            )}
          </>
        ) : (
          <>
            <div className="flex items-center justify-between mb-3">
              <div>
                <div className="text-[10px] text-[var(--text-muted)]">Lap —</div>
                <div className="text-2xl font-black tabular-nums text-[var(--text-muted)]">P—</div>
              </div>
            </div>
            <div className="space-y-1.5">
              <div>
                <div className="text-[9px] text-[var(--text-secondary)] uppercase tracking-wider">Current</div>
                <div className="text-xl font-bold tabular-nums text-[var(--text-muted)]">--:--.---</div>
              </div>
              <div>
                <div className="text-[9px] text-[var(--text-secondary)] uppercase tracking-wider">Last lap</div>
                <div className="text-lg font-bold tabular-nums text-[var(--text-muted)]">--:--.---</div>
              </div>
            </div>
            <div className="mt-3 pt-3 -mx-4 px-4 border-t border-[var(--border)] grid grid-cols-3 gap-2">
              {['S1', 'S2', 'S3'].map(s => (
                <div key={s}>
                  <div className="text-[9px] text-[var(--text-secondary)]">{s}</div>
                  <div className="text-xs font-bold tabular-nums text-[var(--text-muted)]">–:––.–––</div>
                </div>
              ))}
            </div>
          </>
        )}
      </Panel>

      {/* ── ERS ── */}
      <Panel>
        <SectionLabel>Energy Recovery</SectionLabel>

        {activeStatus ? (
          <div className="space-y-4">
            {/* Energy bar */}
            <div>
              <div className="flex justify-between items-baseline mb-1.5">
                <span className="text-3xl font-black tabular-nums"
                      style={{ color: ersBarColor }}>
                  {ersPct.toFixed(1)}%
                </span>
                <span
                  className="text-sm font-bold"
                  style={{
                    color: activeStatus.ers_mode === 0 ? 'var(--text-secondary)' :
                           activeStatus.ers_mode === 1 ? (isDark ? '#5794F2' : '#0B57D0') :
                           activeStatus.ers_mode === 2 ? (isDark ? 'var(--compound-medium)' : '#B7950B') :
                           '#C4162A'
                  }}
                >
                  {ERS_MODES[activeStatus.ers_mode] ?? ''}
                </span>
              </div>
              <div className="w-full h-3 bg-[var(--border)] rounded-full overflow-hidden">
                <div
                  className="h-full rounded-full transition-all duration-300"
                  style={{ width: `${ersBarWidth}%`, background: ersBarColor }}
                />
              </div>
              <div className="text-[9px] text-[var(--text-secondary)] mt-1">
                {(activeStatus.ers_j / 1_000_000).toFixed(2)} MJ / 4.00 MJ
              </div>
            </div>

            <div className="grid grid-cols-2 gap-3 text-sm">
              <div>
                <div className="text-[9px] text-[var(--text-secondary)] uppercase tracking-wider">Deployed</div>
                <div className="font-bold text-[var(--text-primary)]">
                  {(activeStatus.ers_deployed_j / 1_000_000).toFixed(2)} MJ
                </div>
              </div>
              <div>
                <div className="text-[9px] text-[var(--text-secondary)] uppercase tracking-wider">DRS</div>
                <div className={`font-bold ${activeStatus.drs_allowed ? 'text-[#37872D]' : 'text-[var(--text-secondary)]'}`}>
                  {activeStatus.drs_allowed ? 'AVAILABLE' : 'LOCKED'}
                </div>
              </div>
            </div>
          </div>
        ) : (
          <div className="space-y-4">
            <div>
              <div className="flex justify-between items-baseline mb-1.5">
                <span className="text-3xl font-black tabular-nums text-[var(--text-muted)]">—%</span>
                <span className="text-sm font-bold text-[var(--text-muted)]">—</span>
              </div>
              <div className="w-full h-3 bg-[var(--border)] rounded-full overflow-hidden" />
              <div className="text-[9px] text-[var(--text-muted)] mt-1">— MJ / 4.00 MJ</div>
            </div>
            <div className="grid grid-cols-2 gap-3 text-sm">
              <div>
                <div className="text-[9px] text-[var(--text-secondary)] uppercase tracking-wider">Deployed</div>
                <div className="font-bold text-[var(--text-muted)]">— MJ</div>
              </div>
              <div>
                <div className="text-[9px] text-[var(--text-secondary)] uppercase tracking-wider">DRS</div>
                <div className="font-bold text-[var(--text-muted)]">—</div>
              </div>
            </div>
          </div>
        )}
      </Panel>

      {/* ── Fuel & Tyre ── */}
      <Panel>
        <SectionLabel>Strategy</SectionLabel>

        {activeStatus ? (
          <div className="space-y-4">
            {/* Fuel */}
            <div>
              <div className="text-[9px] text-[var(--text-secondary)] uppercase tracking-wider mb-1">Fuel</div>
              <div className="flex items-baseline gap-2">
                <span
                  className="text-3xl font-black tabular-nums"
                  style={{
                    color: activeStatus.fuel_laps > 1 ? (isDark ? '#37872D' : '#137333') :
                           activeStatus.fuel_laps >= 0 ? (isDark ? '#d4ad04' : '#B06000') :
                           '#C4162A'
                  }}
                >
                  {activeStatus.fuel_kg.toFixed(1)}
                </span>
                <span className="text-[var(--text-secondary)] text-sm">kg</span>
              </div>
              <div className="text-sm text-[var(--text-secondary)]">
                {activeStatus.fuel_laps >= 0 ? '+' : ''}{activeStatus.fuel_laps.toFixed(1)} laps vs finish
              </div>
              <div className="text-xs text-[var(--text-secondary)] mt-1">Mix: {FUEL_MIX[activeStatus.fuel_mix] ?? ''}</div>
            </div>

            {/* Tyre */}
            <div className="pt-3 -mx-4 px-4 border-t border-[var(--border)]">
              <div className="text-[9px] text-[var(--text-secondary)] uppercase tracking-wider mb-1">Tyre</div>
              <div className="flex items-center gap-2">
                {tyreName && (
                  <span className="text-2xl font-black" style={{ color: tyreColor }}>
                    {tyreName}
                  </span>
                )}
                <div className="text-sm text-[var(--text-secondary)]">
                  Age: {activeStatus.tyre_age_laps} laps
                </div>
              </div>
              <div className="text-xs text-[var(--text-secondary)] mt-0.5">Brake bias: {activeStatus.front_brake_bias}% front</div>
            </div>
          </div>
        ) : (
          <div className="space-y-4">
            <div>
              <div className="text-[9px] text-[var(--text-secondary)] uppercase tracking-wider mb-1">Fuel</div>
              <div className="flex items-baseline gap-2">
                <span className="text-3xl font-black tabular-nums text-[var(--text-muted)]">—</span>
                <span className="text-[var(--text-muted)] text-sm">kg</span>
              </div>
              <div className="text-sm text-[var(--text-muted)]">— laps vs finish</div>
              <div className="text-xs text-[var(--text-muted)] mt-1">Mix: —</div>
            </div>
            <div className="pt-3 -mx-4 px-4 border-t border-[var(--border)]">
              <div className="text-[9px] text-[var(--text-secondary)] uppercase tracking-wider mb-1">Tyre</div>
              <div className="flex items-center gap-2">
                <span className="text-2xl font-black text-[var(--text-muted)]">—</span>
                <div className="text-sm text-[var(--text-muted)]">Age: — laps</div>
              </div>
              <div className="text-xs text-[var(--text-muted)] mt-0.5">Brake bias: —% front</div>
            </div>
          </div>
        )}
      </Panel>

    </div>
  )
})
export default RacePanel
