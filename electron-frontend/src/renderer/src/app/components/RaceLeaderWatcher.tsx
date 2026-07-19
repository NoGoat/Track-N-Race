import { memo, useEffect, useRef } from 'react'
import { useTelemetryStore } from '../../stores/telemetryStore'

export default memo(function RaceLeaderWatcher({ enabled, onLeaderChange }: { enabled: boolean; onLeaderChange: (idx: number) => void }) {
  const timing = useTelemetryStore(state => state.timing)
  const leaderRef = useRef<number | null>(null)
  useEffect(() => {
    if (!timing || !enabled) return
    const leader = timing.cars.find(car => car.position === 1 && car.result_status === 2)
    if (!leader) return
    if (leaderRef.current === null) { leaderRef.current = leader.idx; return }
    if (leader.idx !== leaderRef.current) {
      leaderRef.current = leader.idx
      onLeaderChange(leader.idx)
    }
  }, [timing, enabled, onLeaderChange])
  return null
})
