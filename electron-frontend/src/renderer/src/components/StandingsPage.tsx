import { memo, useCallback, useEffect, useState } from 'react'
import TimingTower from './TimingTower'
import RacePanel from './RacePanel'
import type { StandingsLayout } from '../app/appConfig'
import type { CompactState } from '../lib/graphSections'
import type { AllStatusMsg, LapRow, ParticipantsMsg, StatusRow, TimingMsg } from '../types'
import { useSecondaryOpenFile } from '../lib/secondaryComparison'
import { useSecondaryState } from '../lib/secondaryStream'
import { fileDateLabels } from '../lib/secondaryFile'

type FileKey = 'a' | 'b'

interface Props {
  standingsLayout: StandingsLayout
  compact: CompactState
  isDark: boolean
  reduceAnimations: boolean
  // File A's rows, from the telemetry store.
  lap: LapRow | null
  status: StatusRow | null
  timing: TimingMsg | null
  participants: ParticipantsMsg | null
  allStatus: AllStatusMsg | null
  fastestLapCarIdx: number | null
  selectedIdx: number | null
  onSelectDriver: (idx: number) => void
}

/** One file's tower and panel inputs. */
interface StandingsFile {
  lap: LapRow | null
  status: StatusRow | null
  timing: TimingMsg | null
  participants: ParticipantsMsg | null
  allStatus: AllStatusMsg | null
  fastestLapCarIdx: number | null
  selectedIdx: number | null
  onSelectDriver: (idx: number) => void
}

/**
 * The Standings page: the timing tower and the side panel (Timing, Energy
 * Recovery, Strategy). With a second file open (Standings 12) a row of file
 * tabs (coloured date and followed driver) sits over the tower, and the tower
 * and panel both show the selected file; the panel's TIMING carries its date.
 */
const StandingsPage = memo(function StandingsPage({
  standingsLayout, compact, isDark, reduceAnimations,
  lap, status, timing, participants, allStatus, fastestLapCarIdx, selectedIdx, onSelectDriver,
}: Props) {
  const secondary = useSecondaryOpenFile()
  const secondaryState = useSecondaryState()
  const [selectedFile, setSelectedFile] = useState<FileKey>('a')
  // The car picked in B's tower for the panel, as A's selectedIdx (null: B's followed car).
  const [selectedIdxB, setSelectedIdxB] = useState<number | null>(null)
  const [selectionFile, setSelectionFile] = useState<string | null>(null)
  const secondaryPath = secondary?.file.filePath ?? null
  if (selectionFile !== secondaryPath) {
    setSelectionFile(secondaryPath)
    setSelectedIdxB(null)
  }
  const onSelectDriverB = useCallback((idx: number) => setSelectedIdxB(prev => prev === idx ? null : idx), [])
  // V6 playback reads a car's private status (ERS, fuel, DRS, brake bias) only
  // for the focus car, so B's stream focuses the car selected in B's tower, as
  // AppShell focuses A's on this page. Re-sent per second file.
  useEffect(() => {
    if (!secondaryPath) return
    window.analysisBridge.setFocusDriver(selectedIdxB ?? -1)
    return () => window.analysisBridge.setFocusDriver(-1)
  }, [secondaryPath, selectedIdxB])
  const shownFile: FileKey = secondary ? selectedFile : 'a'

  // B's rows from its stream, at its moment on the playhead. Its reader plays
  // the car followed in B, so B's rows name that car as their player, as A's do.
  const fileA: StandingsFile = { lap, status, timing, participants, allStatus, fastestLapCarIdx, selectedIdx, onSelectDriver }
  const fileB: StandingsFile = {
    lap: secondary ? secondaryState.lapRow : null,
    status: secondary ? secondaryState.status : null,
    timing: secondary ? secondaryState.timing : null,
    participants: secondary ? secondaryState.participants : null,
    allStatus: secondary ? secondaryState.allStatus : null,
    fastestLapCarIdx: secondary ? secondaryState.fastestLapCarIdx : null,
    selectedIdx: selectedIdxB, onSelectDriver: onSelectDriverB,
  }
  const shown = shownFile === 'b' ? fileB : fileA

  const showTower = standingsLayout?.showTimingTower ?? true
  const showTiming = standingsLayout?.cards?.timing ?? true
  const showErs = standingsLayout?.cards?.energyRecovery ?? true
  const showStrategy = standingsLayout?.cards?.strategy ?? true
  const anySidebar = showTiming || showErs || showStrategy
  const sidebarPct = standingsLayout?.sidebarPct ?? 28

  if (!showTower && !anySidebar) {
    return null
  }

  const selectedCar       = shown.timing?.cars.find(c => c.idx === shown.selectedIdx) ?? null
  const playerDriver      = shown.participants?.drivers.find(d => d.idx === (shown.timing?.player_idx ?? -1)) ?? null
  const selectedDriver    = shown.participants?.drivers.find(d => d.idx === shown.selectedIdx) ?? playerDriver
  const selectedCarStatus = shown.allStatus?.cars.find(c => c.idx === shown.selectedIdx) ?? null

  const [dateA, dateB] = secondary ? fileDateLabels(secondary.primaryFilename, secondary.file.filename) : [null, null]
  const tabs = secondary ? [
    { key: 'a' as const, date: dateA ?? 'File A', driver: driverName(timing, participants) },
    { key: 'b' as const, date: dateB ?? 'File B', driver: driverName(fileB.timing, fileB.participants) },
  ] : null

  const fileTabs = tabs && (
    <div role="tablist" aria-label="Standings file" className="shrink-0 h-[38px] flex items-stretch gap-1 px-2.5 border-b border-[var(--border)]">
      {tabs.map(entry => {
        const selected = entry.key === shownFile
        return (
          <button
            key={entry.key}
            type="button"
            role="tab"
            aria-selected={selected}
            onClick={() => setSelectedFile(entry.key)}
            className={`flex items-center gap-2 px-3 transition-opacity ${selected ? '' : 'opacity-60 hover:opacity-100'}`}
          >
            <span className="text-[12px] font-bold whitespace-nowrap" style={{ color: `var(--file-${entry.key})` }}>{entry.date}</span>
            {entry.driver && <span className="text-[12px] font-bold text-[var(--text-primary)] whitespace-nowrap">{entry.driver}</span>}
          </button>
        )
      })}
    </div>
  )

  const tower = (
    <TimingTower
      key={shownFile}
      timing={shown.timing}
      participants={shown.participants}
      allStatus={shown.allStatus}
      fastestLapCarIdx={shown.fastestLapCarIdx}
      selectedIdx={shown.selectedIdx}
      onSelectDriver={shown.onSelectDriver}
      isDark={isDark}
      animationsEnabled={!reduceAnimations}
      compact={compact.standingsTable}
      file={shownFile}
    />
  )
  const panel = (
    <RacePanel
      key={shownFile}
      lap={shown.lap}
      status={shown.status}
      selectedCar={selectedCar}
      selectedDriver={selectedDriver}
      selectedCarStatus={selectedCarStatus}
      playerIdx={shown.timing?.player_idx ?? null}
      isDark={isDark}
      cards={standingsLayout?.cards}
      compactTiming={compact.standingsTiming}
      compactErs={compact.standingsErs}
      compactStrategy={compact.standingsStrategy}
    />
  )

  return (
    <div className="h-full flex flex-col overflow-hidden">
      <div className="flex-1 min-h-0 flex bg-[var(--bg-panel)] border-t border-[var(--border)] overflow-hidden divide-x divide-[var(--border)]">
        {showTower && (fileTabs ? (
          <div
            style={anySidebar ? { width: `${100 - sidebarPct}%` } : undefined}
            className={`${anySidebar ? 'shrink-0' : 'flex-1'} min-w-0 flex flex-col overflow-hidden`}
          >
            {fileTabs}
            <div className="flex-1 min-h-0 overflow-auto">{tower}</div>
          </div>
        ) : (
          <div
            style={anySidebar ? { width: `${100 - sidebarPct}%` } : undefined}
            className={`${anySidebar ? 'shrink-0' : 'flex-1'} min-w-0 overflow-auto`}
          >
            {tower}
          </div>
        ))}
        {anySidebar && (
          <div
            style={showTower ? { width: `${sidebarPct}%` } : undefined}
            className={`${showTower ? 'shrink-0' : 'flex-1'} overflow-y-auto`}
          >
            {/* The tower hidden: the file tabs sit over the panel instead. */}
            {!showTower && fileTabs}
            {panel}
          </div>
        )}
      </div>
    </div>
  )
})

/** The followed car's name: the tower's player row. */
function driverName(timing: TimingMsg | null, participants: ParticipantsMsg | null): string | null {
  if (!timing) return null
  return participants?.drivers.find(d => d.idx === timing.player_idx)?.name ?? null
}

export default StandingsPage
