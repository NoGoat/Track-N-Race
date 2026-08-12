import { useMemo, useRef, useCallback, memo, useLayoutEffect } from 'react'
import type { TimingMsg, ParticipantsMsg, TimingCar, DriverInfo, AllStatusMsg } from '../types'
import { useLabels } from '../lib/labels'

interface Props {
  timing: TimingMsg | null
  participants: ParticipantsMsg | null
  allStatus: AllStatusMsg | null
  fastestLapCarIdx: number | null
  selectedIdx: number | null
  onSelectDriver: (idx: number) => void
  isDark: boolean
  animationsEnabled: boolean
}


const VISUAL_COLORS: Record<number, string> = {
  16: 'var(--compound-soft)',
  17: 'var(--compound-medium)',
  18: 'var(--compound-hard)',
   7: 'var(--compound-inter)',
   8: 'var(--compound-wet)',
}


const RESULT_LABELS: Record<number, string> = {
  4: 'DNF',
  5: 'DSQ',
  7: 'RET',
}

function abbrev(name: string): string {
  const parts = name.trim().split(/\s+/)
  return parts[parts.length - 1].slice(0, 3).toUpperCase()
}

function fmtMs(ms: number): string {
  if (ms <= 0) return '--:--.---'
  const m     = Math.floor(ms / 60_000)
  const s     = Math.floor((ms % 60_000) / 1000)
  const mills = ms % 1000
  return `${m}:${String(s).padStart(2, '0')}.${String(mills).padStart(3, '0')}`
}

function fmtSector(ms: number): string {
  if (ms <= 0) return '—'
  const s     = Math.floor(ms / 1000)
  const mills = ms % 1000
  return `${s}.${String(mills).padStart(3, '0')}`
}

function fmtGap(gap_ms: number, position: number): string {
  if (position === 1) return 'LEADER'
  if (gap_ms <= 0) return '—'
  const sec = gap_ms / 1000
  if (sec < 60) return `+${sec.toFixed(3)}`
  const m = Math.floor(sec / 60)
  const s = (sec % 60).toFixed(3).padStart(6, '0')
  return `+${m}:${s}`
}

const PosCell = memo(function PosCell({ pos, isDark }: { pos: number; isDark: boolean }) {
  const color = isDark
    ? (pos === 1 ? '#FFD700' :
       pos === 2 ? '#C0C0C0' :
       pos === 3 ? '#CD7F32' :
       '#8e8e8e')
    : (pos === 1 ? '#765900' :
       pos === 2 ? '#5E6475' :
       pos === 3 ? '#9C5B23' :
       '#565B70')
  return (
    <td className="px-3 py-1 text-sm font-black tabular-nums w-10" style={{ color }}>
      P{pos}
    </td>
  )
})
PosCell.displayName = 'PosCell'

interface RowProps {
  carIdx: number
  position: number
  lapNum: number
  lastLapMs: number
  gapMs: number
  pitStatus: number
  resultStatus: number
  lapInvalid: boolean
  penaltiesS: number
  numDtPens: number
  numSgPens: number
  driver: DriverInfo | undefined
  isPlayer: boolean
  isSelected: boolean
  isFastest: boolean
  onSelect: (idx: number) => void
  s1: number
  s2: number
  s3: number
  tyreLabel: string | null
  tyreColor: string
  isDark: boolean
}

const TowerRow = memo(function TowerRow({
  carIdx,
  position,
  lapNum,
  lastLapMs,
  gapMs,
  pitStatus,
  resultStatus,
  lapInvalid,
  penaltiesS,
  numDtPens,
  numSgPens,
  driver,
  isPlayer,
  isSelected,
  isFastest,
  onSelect,
  s1,
  s2,
  s3,
  tyreLabel,
  tyreColor,
  isDark
}: RowProps) {
  const teamColor = driver?.livery_color ?? '#8e8e8e'
  const driverCode = driver ? abbrev(driver.name) : `C${carIdx}`
  const raceNum = driver?.race_number ?? ''
  const retired = RESULT_LABELS[resultStatus]
  const isInactive = resultStatus <= 1 || position === 0

  const handleClick = useCallback(() => {
    onSelect(carIdx)
  }, [onSelect, carIdx])

  if (isInactive) return null

  return (
    <tr
      data-car-idx={carIdx}
      onClick={handleClick}
      className={`border-b border-[var(--border)] transition-colors cursor-pointer ${
        isSelected
          ? 'bg-[var(--bg-selected)]'
          : isFastest
          ? 'bg-[#BF5FFF]/10 hover:bg-[#BF5FFF]/15'
          : 'hover:bg-[var(--bg-hover)]'
      }`}
    >
      <PosCell pos={position} isDark={isDark} />

      {/* Driver */}
      <td className="px-3 py-1">
        <div className="flex items-center gap-2">
          <div className="w-1 h-5 rounded-full shrink-0" style={{ background: teamColor }} />
          <span className="text-[10px] text-[var(--text-secondary)] w-5 tabular-nums">{raceNum}</span>
          <span className={`text-sm font-bold ${isPlayer ? 'text-[var(--text-primary)] font-extrabold' : 'text-[var(--text-primary)]'}`}>
            {driverCode}
          </span>
          {driver && (
            <span className="text-[10px] text-[var(--text-secondary)] hidden lg:inline truncate max-w-[100px]">
              {driver.name}
            </span>
          )}
          {isPlayer && (
            <span className="text-[9px] bg-[var(--border-focus)] text-[#8e9ee8] px-1.5 py-0.5 rounded font-bold">YOU</span>
          )}
        </div>
      </td>

      {/* Lap */}
      <td className="px-3 py-1 text-sm tabular-nums text-[var(--text-secondary)] w-12">
        {lapNum}
      </td>

      {/* Last lap */}
      <td className="px-3 py-1 text-sm font-bold tabular-nums text-[var(--text-primary)] w-28">
        {fmtMs(lastLapMs)}
      </td>

      {/* Gap */}
      <td className="px-3 py-1 text-sm tabular-nums text-[var(--text-secondary)] w-24">
        {retired
          ? <span className="text-[#C4162A] font-bold text-xs">{retired}</span>
          : fmtGap(gapMs, position)
        }
      </td>

      {/* S1 */}
      <td className="px-3 py-1 text-xs tabular-nums text-[var(--text-secondary)] w-20">{fmtSector(s1)}</td>
      {/* S2 */}
      <td className="px-3 py-1 text-xs tabular-nums text-[var(--text-secondary)] w-20">{fmtSector(s2)}</td>
      {/* S3 */}
      <td className="px-3 py-1 text-xs tabular-nums text-[var(--text-secondary)] w-20">{fmtSector(s3)}</td>

      {/* Tyre */}
      <td className="px-3 py-1 w-12">
        {tyreLabel
          ? <span className="text-xs font-bold tabular-nums" style={{ color: tyreColor }}>{tyreLabel}</span>
          : <span className="text-xs text-[var(--text-muted)]">—</span>
        }
      </td>

      {/* Badges */}
      <td className="px-3 py-1">
        <div className="flex gap-1 flex-wrap">
          {pitStatus > 0 && (
            <span
              className="text-[9px] font-bold px-1.5 py-0.5 rounded"
              style={{
                color: isDark ? 'var(--compound-medium)' : '#765900',
                backgroundColor: isDark ? 'rgba(255, 215, 0, 0.1)' : 'rgba(118, 89, 0, 0.1)',
                border: `1px solid ${isDark ? 'var(--compound-medium)' : 'rgba(118, 89, 0, 0.45)'}`
              }}
            >
              {pitStatus === 1 ? 'PIT' : 'PIT LANE'}
            </span>
          )}
          {lapInvalid && (
            <span className="text-[9px] font-bold text-[#C4162A] bg-[#C4162A]/10 border border-[#C4162A] px-1.5 py-0.5 rounded">
              INV
            </span>
          )}
          {penaltiesS > 0 && (
            <span
              className="text-[9px] font-bold px-1.5 py-0.5 rounded border"
              style={{
                color: isDark ? '#c47d0e' : '#8B5200',
                borderColor: isDark ? '#c47d0e' : '#8B5200',
                backgroundColor: isDark ? 'rgba(196, 125, 14, 0.1)' : 'rgba(139, 82, 0, 0.1)'
              }}
            >
              +{penaltiesS}s
            </span>
          )}
          {numDtPens > 0 && (
            <span className="text-[9px] font-bold text-[#e10600] bg-[#e10600]/10 border border-[#e10600] px-1.5 py-0.5 rounded">
              {numDtPens > 1 ? `${numDtPens}× ` : ''}DT
            </span>
          )}
          {numSgPens > 0 && (
            <span className="text-[9px] font-bold text-[#e10600] bg-[#e10600]/10 border border-[#e10600] px-1.5 py-0.5 rounded">
              {numSgPens > 1 ? `${numSgPens}× ` : ''}SG
            </span>
          )}
        </div>
      </td>
    </tr>
  )
})
TowerRow.displayName = 'TowerRow'

const TimingTower = memo(function TimingTower({ timing, participants, allStatus, fastestLapCarIdx, selectedIdx, onSelectDriver, isDark, animationsEnabled }: Props) {
  const { tn } = useLabels()
  // Per-car tracking refs for freeze + S3 computation
  const prevCarsRef     = useRef<Map<number, TimingCar>>(new Map())
  const frozenRef       = useRef<Map<number, { s1: number; s2: number; s3: number; exp: number }>>(new Map())
  const lastS3Ref       = useRef<Map<number, number>>(new Map())
  const s3SnapshotRef   = useRef<Map<number, { s1: number; s2: number; lap: number }>>(new Map())
  // Guard: process each timing message only once (safe under StrictMode double-invoke)
  const lastTsRef       = useRef<string | null>(null)

  // FLIP animation refs
  const tbodyRef          = useRef<HTMLTableSectionElement>(null)
  const prevOrderRef      = useRef<number[]>([])
  const savedPositionsRef = useRef<Map<number, number>>(new Map())

  const rows = useMemo(() => {
    if (!timing) return []
    const now = Date.now()
    const driverMap = new Map<number, DriverInfo>(
      (participants?.drivers ?? []).map(d => [d.idx, d])
    )
    const statusMap = new Map(
      (allStatus?.cars ?? []).map(s => [s.idx, s])
    )

    // Update freeze/S3 tracking once per unique timing message
    if (timing.ts !== lastTsRef.current) {
      lastTsRef.current = timing.ts

      for (const car of timing.cars) {
        const prev = prevCarsRef.current.get(car.idx)

        // Capture s1+s2 when car enters sector 3 (sector===2, 0-indexed)
        if (car.sector === 2 && car.s1_ms > 0 && car.s2_ms > 0) {
          const snap = s3SnapshotRef.current.get(car.idx)
          if (!snap || snap.lap !== car.lap_num)
            s3SnapshotRef.current.set(car.idx, { s1: car.s1_ms, s2: car.s2_ms, lap: car.lap_num })
        }

        // Lap just completed — compute s3, freeze previous sector times for 7 s
        if (prev && car.lap_num > prev.lap_num) {
          const snap = s3SnapshotRef.current.get(car.idx)
          const s3   = (snap && snap.lap === prev.lap_num && car.last_lap_ms > 0)
            ? Math.max(0, car.last_lap_ms - snap.s1 - snap.s2) : 0
          lastS3Ref.current.set(car.idx, s3)
          frozenRef.current.set(car.idx, { s1: prev.s1_ms, s2: prev.s2_ms, s3, exp: now + 7_000 })
        }

        prevCarsRef.current.set(car.idx, car)
      }
    }

    const activeCars = timing.cars.filter(c => c.position > 0 && c.result_status >= 1)

    return activeCars
      .sort((a, b) => a.position - b.position)
      .map(car => {
        const frozen    = frozenRef.current.get(car.idx)
        const useFrozen = !!frozen && now < frozen.exp
        const carStatus = statusMap.get(car.idx)
        return {
          car,
          s1: useFrozen ? frozen!.s1 : car.s1_ms,
          s2: useFrozen ? frozen!.s2 : car.s2_ms,
          s3: useFrozen ? frozen!.s3 : 0,
          driver: driverMap.get(car.idx),
          isPlayer: car.idx === timing.player_idx,
          isFastest: car.idx === fastestLapCarIdx,
          tyreLabel: carStatus ? tn('tyre.actual', carStatus.tyre_compound) : null,
          tyreColor: carStatus ? (VISUAL_COLORS[carStatus.visual_compound] ?? '#ffffff') : '#ffffff',
        }
      })
  }, [timing, participants, fastestLapCarIdx, tn])

  useLayoutEffect(() => {
    const tbody = tbodyRef.current
    if (!tbody) return

    const trElements = tbody.querySelectorAll<HTMLTableRowElement>('tr[data-car-idx]')
    const currentOrder = rows.map(r => r.car.idx)
    const prevOrder = prevOrderRef.current

    const orderChanged =
      prevOrder.length > 0 &&
      (currentOrder.length !== prevOrder.length ||
        currentOrder.some((idx, i) => prevOrder[i] !== idx))

    if (orderChanged && animationsEnabled) {
      const prevRankOf = new Map(prevOrder.map((idx, rank) => [idx, rank]))
      const currRankOf = new Map(currentOrder.map((idx, rank) => [idx, rank]))

      trElements.forEach(row => {
        const carIdx = Number(row.dataset.carIdx)
        const savedY  = savedPositionsRef.current.get(carIdx)
        const delta   = savedY !== undefined ? savedY - row.offsetTop : 0

        const prevRank   = prevRankOf.get(carIdx)
        const currRank   = currRankOf.get(carIdx)
        const flashDir   =
          prevRank !== undefined && currRank !== undefined && prevRank !== currRank
            ? (currRank < prevRank ? 'gain' : 'lose')
            : null

        const hasMove = Math.abs(delta) > 1
        if (!hasMove && !flashDir) return

        // Set starting state
        if (hasMove) {
          row.style.transition = 'none'
          row.style.transform  = `translateY(${delta}px)`
        }
        if (flashDir) row.style.animation = 'none'

        // Commit starting state before triggering animations
        void row.offsetHeight

        if (hasMove) {
          row.style.transition = 'transform 0.4s cubic-bezier(0.25, 0.46, 0.45, 0.94)'
          row.style.transform  = ''
        }
        if (flashDir) {
          row.style.animation = flashDir === 'gain'
            ? 'posGainFlash 1s ease-out'
            : 'posLoseFlash 1s ease-out'
        }
      })
    }

    // Save layout positions for next render (offsetTop is unaffected by CSS transforms)
    trElements.forEach(row => {
      savedPositionsRef.current.set(Number(row.dataset.carIdx), row.offsetTop)
    })
    prevOrderRef.current = currentOrder
  }, [rows, animationsEnabled])

  const HEADERS = ['Pos', 'Driver', 'Lap', 'Last Lap', 'Gap', 'S1', 'S2', 'S3', 'Tyre', '']

  if (!timing) {
    return (
      <div>
        <div className="overflow-x-auto">
          <table className="w-full border-collapse">
            <thead>
              <tr className="border-b border-[var(--border)]">
                {HEADERS.map(h => (
                  <th key={h} className="px-3 py-1 text-left text-[9px] text-[var(--text-secondary)] uppercase tracking-widest font-normal">{h}</th>
                ))}
              </tr>
            </thead>
            <tbody>
              {Array.from({ length: 20 }, (_, i) => (
                <tr key={i} className="border-b border-[var(--border)]">
                  <td className="px-3 py-1 text-sm font-black tabular-nums w-10 text-[var(--text-muted)]">P{i + 1}</td>
                  <td className="px-3 py-1">
                    <div className="flex items-center gap-2">
                      <div className="w-1 h-5 rounded-full shrink-0 bg-[var(--border)]" />
                      <span className="text-[10px] text-[var(--text-muted)] w-5 tabular-nums">—</span>
                      <span className="text-sm font-bold text-[var(--text-muted)]">—</span>
                    </div>
                  </td>
                  <td className="px-3 py-1 text-sm tabular-nums text-[var(--text-muted)] w-12">—</td>
                  <td className="px-3 py-1 text-sm font-bold tabular-nums text-[var(--text-muted)] w-28">--:--.---</td>
                  <td className="px-3 py-1 text-sm tabular-nums text-[var(--text-muted)] w-24">—</td>
                  <td className="px-3 py-1 text-xs tabular-nums text-[var(--text-muted)] w-20">—</td>
                  <td className="px-3 py-1 text-xs tabular-nums text-[var(--text-muted)] w-20">—</td>
                  <td className="px-3 py-1 text-xs tabular-nums text-[var(--text-muted)] w-20">—</td>
                  <td className="px-3 py-1 w-12"><span className="text-xs text-[var(--text-muted)]">—</span></td>
                  <td className="px-3 py-1" />
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      </div>
    )
  }

  return (
    <div>
      <div className="overflow-x-auto">
        <table className="w-full border-collapse">
          <thead>
            <tr className="border-b border-[var(--border)]">
              {HEADERS.map(h => (
                <th key={h} className="px-3 py-1 text-left text-[9px] text-[var(--text-secondary)] uppercase tracking-widest font-normal">
                  {h}
                </th>
              ))}
            </tr>
          </thead>
          <tbody ref={tbodyRef}>
            {rows.map(({ car, driver, isPlayer, isFastest, s1, s2, s3, tyreLabel, tyreColor }) => (
              <TowerRow
                key={car.idx}
                carIdx={car.idx}
                position={car.position}
                lapNum={car.lap_num}
                lastLapMs={car.last_lap_ms}
                gapMs={car.gap_ms}
                pitStatus={car.pit_status}
                resultStatus={car.result_status}
                lapInvalid={car.lap_invalid}
                penaltiesS={car.penalties_s}
                numDtPens={car.num_dt_pens}
                numSgPens={car.num_sg_pens}
                driver={driver}
                isPlayer={isPlayer}
                isSelected={car.idx === selectedIdx}
                isFastest={isFastest}
                onSelect={onSelectDriver}
                s1={s1}
                s2={s2}
                s3={s3}
                tyreLabel={tyreLabel}
                tyreColor={tyreColor}
                isDark={isDark}
              />
            ))}
          </tbody>
        </table>
      </div>
    </div>
  )
})

export default TimingTower
