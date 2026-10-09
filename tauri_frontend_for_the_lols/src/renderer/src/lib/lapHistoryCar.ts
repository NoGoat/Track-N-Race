import { useTelemetryStore } from '../stores/telemetryStore'

// The engine pushes driver_lap_history for one car at a time. Each consumer
// claims it while mounted; the newest claim wins, and releasing it hands the
// car back to the previous claim (the laps dialog opened over the Stint page).
const claims: Array<{ carIdx: number }> = []

function apply(): void {
  window.playerBridge.setLapHistoryCar(claims.at(-1)?.carIdx ?? -1)
}

export function claimLapHistoryCar(carIdx: number): () => void {
  const claim = { carIdx }
  claims.push(claim)
  // A row left from an earlier claim may describe another car or cursor.
  useTelemetryStore.setState({ driverLapHistory: null })
  apply()
  return () => {
    const index = claims.indexOf(claim)
    if (index === -1) return
    claims.splice(index, 1)
    if (index === claims.length) apply()
  }
}
