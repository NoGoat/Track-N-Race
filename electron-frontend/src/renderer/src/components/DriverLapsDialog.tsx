import { useEffect } from 'react'
import { X } from 'lucide-react'
import type { DriverInfo, DriverLapHistoryLap } from '../types'
import { useModalPresenceValue } from '../lib/useModalPresence'
import { useTelemetryStore } from '../stores/telemetryStore'
import { fmtMs, fmtSector } from '../lib/lapTimeFormat'

interface Target {
  carIdx: number
  driver: DriverInfo | undefined
}

interface Props {
  target: Target | null
  // The Standings fastest-lap holder, so the modal agrees with its tinted row.
  fastestLapCarIdx: number | null
  onClose: () => void
}

export default function DriverLapsDialog({ target, fastestLapCarIdx, onClose }: Props) {
  const { mounted, visible, transitionTargetRef, value: shown } = useModalPresenceValue(target)
  const carIdx = target?.carIdx ?? null
  // Tell the engine which car's laps are wanted while open. It pushes them
  // now and again only when they change; nothing here polls.
  useEffect(() => {
    if (carIdx === null) return
    // A row left from an earlier opening may describe another cursor.
    useTelemetryStore.setState({ driverLapHistory: null })
    window.playerBridge.setLapHistoryCar(carIdx)
    return () => window.playerBridge.setLapHistoryCar(-1)
  }, [carIdx])
  const pushed = useTelemetryStore(s => s.driverLapHistory)
  const shownCar = shown?.carIdx ?? null
  const history = pushed && pushed.car_idx === shownCar ? pushed : null
  const loaded = history !== null

  useEffect(() => {
    if (carIdx === null) return
    const onKey = (event: KeyboardEvent) => { if (event.key === 'Escape') onClose() }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [carIdx, onClose])

  if (!mounted || !shown) return null

  const laps = history?.laps ?? []
  const bestLapNum = history?.best_lap_num ?? null
  // Fastest valid time in each sector across this driver's laps.
  const bestSector = (sector: (lap: DriverLapHistoryLap) => [number, boolean]) => {
    let best = 0
    for (const lap of laps) {
      const [ms, valid] = sector(lap)
      if (ms > 0 && valid && (best === 0 || ms < best)) best = ms
    }
    return best
  }
  const bestS1 = bestSector(lap => [lap.s1_ms, lap.s1_valid])
  const bestS2 = bestSector(lap => [lap.s2_ms, lap.s2_valid])
  const bestS3 = bestSector(lap => [lap.s3_ms, lap.s3_valid])
  // Purple for the session's fastest sector across all drivers, green for
  // this driver's own fastest.
  const sectorClass = (ms: number, best: number, overall: number | undefined) =>
    ms > 0 && ms === overall ? 'text-[var(--color-fastest)] font-bold'
      : ms > 0 && ms === best ? 'text-[#37872D] font-bold'
      : 'text-[var(--text-secondary)]'
  const title = shown.driver?.name ?? `Car ${shown.carIdx + 1}`
  const cell = 'px-4 py-1.5 text-right tabular-nums'

  return (
    <div
      ref={transitionTargetRef}
      data-state={visible ? 'open' : 'closed'}
      className="modal-backdrop fixed inset-0 z-[120] flex items-center justify-center bg-[var(--bg-modal)] backdrop-blur-[2px]"
      role="dialog"
      aria-modal="true"
      aria-labelledby="driver-laps-title"
      onMouseDown={event => { if (event.target === event.currentTarget) onClose() }}
    >
      <div className="modal-panel bg-[var(--bg-panel)] border border-[var(--border)] rounded-xl shadow-[0_0_60px_rgba(0,0,0,0.85)] w-[min(50vw,1100px)] max-w-[calc(100vw-2rem)] max-h-[75vh] flex flex-col overflow-hidden">
        <div className="flex items-center justify-between px-3 py-4 border-b border-[var(--border)] shrink-0 select-none">
          <div
            id="driver-laps-title"
            className="text-xs font-mono font-bold text-[var(--text-primary)] uppercase tracking-widest flex items-center gap-2 min-w-0"
          >
            <span className="truncate">{title} · Lap Times</span>
          </div>
          <button
            onClick={onClose}
            aria-label="Close lap times"
            className="w-9 h-9 flex items-center justify-center rounded-lg text-[var(--text-secondary)] hover:text-[#e10600] transition-colors shrink-0"
          >
            <X size={18} />
          </button>
        </div>

        <div className="flex-1 min-h-0 overflow-auto">
          {!loaded ? (
            // The Strategy page's loading spinner.
            <div className="flex items-center justify-center py-10" role="status" aria-live="polite" aria-label="Loading lap times">
              <div className="w-6 h-6 rounded-full border-2 border-[var(--border)] border-t-[#5794F2] animate-spin"/>
            </div>
          ) : laps.length === 0 ? (
            <p className="px-6 py-10 text-center text-sm text-[var(--text-secondary)]">
              No completed laps for this driver yet.
            </p>
          ) : (
            <table className="w-full border-collapse text-xs font-mono">
              <thead className="sticky top-0 bg-[var(--bg-panel)]">
                <tr className="text-[10px] uppercase tracking-wider text-[var(--text-secondary)]">
                  <th className="px-4 py-2 text-left font-bold border-b border-[var(--border)]">Lap</th>
                  <th className="px-4 py-2 text-right font-bold border-b border-[var(--border)]">Time</th>
                  <th className="px-4 py-2 text-right font-bold border-b border-[var(--border)]">S1</th>
                  <th className="px-4 py-2 text-right font-bold border-b border-[var(--border)]">S2</th>
                  <th className="px-4 py-2 text-right font-bold border-b border-[var(--border)]">S3</th>
                </tr>
              </thead>
              <tbody>
                {laps.map(lap => {
                  // Purple when this driver holds the session's fastest lap (the
                  // same holder Standings tints), green for their own best.
                  const isBest = lap.lap_num === bestLapNum
                  const isOverallBest = isBest && shown.carIdx === fastestLapCarIdx
                  const timeClass = !lap.lap_valid ? 'line-through text-[var(--text-muted)]'
                    : isOverallBest ? 'text-[var(--color-fastest)]'
                    : isBest ? 'text-[#37872D]' : ''
                  return (
                    <tr key={lap.lap_num} className="border-b border-[var(--border)]">
                      <td className="px-4 py-1.5 tabular-nums text-[var(--text-secondary)]">{lap.lap_num}</td>
                      <td className={`${cell} font-bold`}>
                        <span className="inline-flex items-center gap-2 justify-end">
                          {!lap.lap_valid && (
                            <span className="text-[9px] px-1.5 py-0.5 font-bold text-[#C4162A] bg-[#C4162A]/10 border border-[#C4162A] rounded">
                              INV
                            </span>
                          )}
                          <span className={timeClass}>
                            {fmtMs(lap.lap_time_ms)}
                          </span>
                        </span>
                      </td>
                      <td className={`${cell} ${sectorClass(lap.s1_ms, bestS1, history?.overall_best_s1_ms)}`}>{fmtSector(lap.s1_ms)}</td>
                      <td className={`${cell} ${sectorClass(lap.s2_ms, bestS2, history?.overall_best_s2_ms)}`}>{fmtSector(lap.s2_ms)}</td>
                      <td className={`${cell} ${sectorClass(lap.s3_ms, bestS3, history?.overall_best_s3_ms)}`}>{fmtSector(lap.s3_ms)}</td>
                    </tr>
                  )
                })}
              </tbody>
            </table>
          )}
        </div>
      </div>
    </div>
  )
}
