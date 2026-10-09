import { memo, useEffect, useMemo, useState } from 'react'
import type { AnalyzeLapData, LapRow } from '../../types'
import type { TitlebarUpdateInterval } from '../appConfig'
import { DEFAULT_DELTA_NEGATIVE_COLOR, DEFAULT_DELTA_POSITIVE_COLOR } from '../../lib/analyzeMetrics'
import { buildLapProgressMap, interpolateLapElapsed } from '../../lib/lapDelta'
import { useTelemetryStore } from '../../stores/telemetryStore'

interface Props {
  comparisonMode: 'PL' | 'FL' | 'RL' | null
  referenceLapNum: number | null
  updateInterval: TitlebarUpdateInterval
}

interface TitlebarSnapshot {
  sessionTime: number | undefined
  lap: LapRow | null
  currentLapData: AnalyzeLapData | null
  comparisonLap: AnalyzeLapData | null
}

function selectComparisonLap(state: ReturnType<typeof useTelemetryStore.getState>, mode: Props['comparisonMode'], referenceLapNum: number | null): AnalyzeLapData | null {
  if (mode === null) return null
  const currentLapNum = state.lap?.lap_num
  const comparisonLapNum = mode === 'PL'
    ? currentLapNum !== undefined && currentLapNum > 1 ? currentLapNum - 1 : null
    : mode === 'FL' ? state.fastestLapNum : referenceLapNum
  if (comparisonLapNum === null) return null
  return state.speedRpmBlocks !== null
    ? state.playbackLapDataCache[comparisonLapNum] ?? null
    : mode === 'PL' ? state.livePreviousLapData : mode === 'FL' ? state.liveFastestLapData : null
}

function readTitlebarSnapshot(comparisonMode: Props['comparisonMode'], referenceLapNum: number | null): TitlebarSnapshot {
  const state = useTelemetryStore.getState()
  return {
    sessionTime: state.latest?.session_time,
    lap: state.lap,
    currentLapData: comparisonMode !== null && state.speedRpmBlocks !== null && state.lap
      ? state.playbackLapDataCache[state.lap.lap_num] ?? null
      : null,
    comparisonLap: selectComparisonLap(state, comparisonMode, referenceLapNum),
  }
}

function RealtimeSessionTimer({ comparisonMode, referenceLapNum }: Pick<Props, 'comparisonMode' | 'referenceLapNum'>) {
  const sessionTime = useTelemetryStore(state => state.latest?.session_time)
  const lap = useTelemetryStore(state => state.lap)
  const currentLapData = useTelemetryStore(state => comparisonMode !== null && state.speedRpmBlocks !== null && state.lap
    ? state.playbackLapDataCache[state.lap.lap_num] ?? null
    : null)
  const comparisonLap = useTelemetryStore(state => selectComparisonLap(state, comparisonMode, referenceLapNum))
  return <SessionTimerDisplay comparisonMode={comparisonMode} sessionTime={sessionTime} lap={lap} currentLapData={currentLapData} comparisonLap={comparisonLap} />
}

function ThrottledSessionTimer({ comparisonMode, referenceLapNum, updateInterval }: Props) {
  const [snapshot, setSnapshot] = useState(() => readTitlebarSnapshot(comparisonMode, referenceLapNum))
  // Settings changes refresh the snapshot at once instead of on the next tick.
  const [settings, setSettings] = useState({ comparisonMode, referenceLapNum, updateInterval })
  if (settings.comparisonMode !== comparisonMode || settings.referenceLapNum !== referenceLapNum ||
      settings.updateInterval !== updateInterval) {
    setSettings({ comparisonMode, referenceLapNum, updateInterval })
    setSnapshot(readTitlebarSnapshot(comparisonMode, referenceLapNum))
  }
  useEffect(() => {
    const timer = window.setInterval(() => setSnapshot(readTitlebarSnapshot(comparisonMode, referenceLapNum)), updateInterval)
    return () => window.clearInterval(timer)
  }, [comparisonMode, referenceLapNum, updateInterval])
  return <SessionTimerDisplay comparisonMode={comparisonMode} {...snapshot} />
}

function SessionTimerDisplay({ comparisonMode, sessionTime, lap, currentLapData, comparisonLap }: Pick<Props, 'comparisonMode'> & TitlebarSnapshot) {
  const currentProgress = useMemo(() => buildLapProgressMap(currentLapData), [currentLapData])
  const comparisonProgress = useMemo(() => buildLapProgressMap(comparisonLap), [comparisonLap])

  const delta = useMemo(() => {
    if (comparisonMode === null || !lap || !comparisonProgress ||
        !Number.isFinite(lap.current_lap_ms) || lap.current_lap_ms < 0 ||
        !Number.isFinite(lap.lap_distance_m)) return null
    const currentElapsed = currentProgress
      ? interpolateLapElapsed(currentProgress, lap.lap_distance_m)
      : lap.current_lap_ms / 1000
    const comparisonElapsed = interpolateLapElapsed(comparisonProgress, lap.lap_distance_m)
    const value = currentElapsed - comparisonElapsed
    return Number.isFinite(value) ? value : null
  }, [comparisonMode, comparisonProgress, currentProgress, lap])

  const formattedTime = sessionTime === undefined
    ? null
    : `${Math.floor(sessionTime / 60)}:${String(Math.floor(sessionTime % 60)).padStart(2, '0')}`
  const formattedDelta = delta === null ? '---' : `${delta >= 0 ? '+' : '−'}${Math.abs(delta).toFixed(3)}`
  const deltaColor = delta === null
    ? 'var(--text-secondary)'
    : delta > 0
      ? DEFAULT_DELTA_POSITIVE_COLOR
      : delta < 0
        ? DEFAULT_DELTA_NEGATIVE_COLOR
        : 'var(--text-primary)'
  // The slot fades out on a null delta, so it keeps showing the last value.
  const [lastDelta, setLastDelta] = useState({ text: '+0.000', color: 'var(--text-primary)' })
  if (delta !== null && (lastDelta.text !== formattedDelta || lastDelta.color !== deltaColor))
    setLastDelta({ text: formattedDelta, color: deltaColor })
  const shownDelta = delta !== null ? { text: formattedDelta, color: deltaColor } : lastDelta

  return (
    <div className="flex items-center text-sm font-black tabular-nums shrink-0">
      {formattedTime && <span className="text-[var(--text-primary)]">{formattedTime}</span>}
      <span className={`titlebar-delta-slot ${delta !== null ? 'titlebar-delta-slot--visible' : ''}`}>
        <span>
          <span className="ml-[13px] whitespace-nowrap" style={{ color: shownDelta.color }}>
            {shownDelta.text}
          </span>
        </span>
      </span>
    </div>
  )
}

export default memo(function SessionTimer(props: Props) {
  return props.updateInterval === 0
    ? <RealtimeSessionTimer comparisonMode={props.comparisonMode} referenceLapNum={props.referenceLapNum} />
    : <ThrottledSessionTimer {...props} />
})
