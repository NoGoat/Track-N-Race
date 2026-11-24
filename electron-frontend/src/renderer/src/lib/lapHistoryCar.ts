import { useTelemetryStore } from '../stores/telemetryStore'
import { clearSecondaryLapHistory } from './secondaryStream'

// The engine (file A) and file B's stream each push driver_lap_history for one
// car at a time. Each consumer claims it while mounted; the newest claim wins,
// and releasing it hands the car back to the previous claim (the laps dialog
// opened over the Stint page).
function lapHistoryClaims(send: (carIdx: number) => void, forget: () => void): (carIdx: number) => () => void {
  const claims: Array<{ carIdx: number }> = []
  const apply = () => send(claims.at(-1)?.carIdx ?? -1)
  return carIdx => {
    const claim = { carIdx }
    claims.push(claim)
    // A row left from an earlier claim may describe another car or cursor.
    forget()
    apply()
    return () => {
      const index = claims.indexOf(claim)
      if (index === -1) return
      claims.splice(index, 1)
      if (index === claims.length) apply()
    }
  }
}

/** File A's lap history for a car. A claim restates the lap measures too. */
export const claimLapHistoryCar = lapHistoryClaims(
  carIdx => window.playerBridge.setLapHistoryCar(carIdx),
  () => useTelemetryStore.setState({ driverLapHistory: null, lapMeasures: null }),
)

/** File B's lap history for a car, from B's stream (useSecondaryState().driverLapHistory). */
export const claimSecondaryLapHistoryCar = lapHistoryClaims(
  carIdx => window.analysisBridge.setLapHistoryCar(carIdx),
  clearSecondaryLapHistory,
)
