import type { DriverInfo } from '../types'

export interface DriverOption {
  value: number
  label: string
  raceNumber: number
  teamId: number
  teamColor: string
  restricted: boolean
  isDisabled: boolean
}

const FALLBACK_TEAM_COLOR = '#8e8e8e'

/**
 * Roster entries as dropdown options, ordered by race number. Works for every
 * TNRD version: older recordings carry no telemetry-access field and may leave
 * the livery color or name blank for unused car slots.
 */
export function buildDriverOptions(drivers: DriverInfo[], {
  playerIdx = null,
  markRestricted = false,
  disableOthers = false,
}: { playerIdx?: number | null; markRestricted?: boolean; disableOthers?: boolean } = {}): DriverOption[] {
  // Older recordings replay their stored JSON rows, so any field may be absent.
  return drivers
    .map(driver => ({ ...driver, name: (driver.name ?? '').trim(), race_number: driver.race_number ?? 0 }))
    .filter(driver => driver.name !== '' || driver.race_number > 0)
    .map(driver => ({
      value: driver.idx,
      label: driver.name || `Car ${driver.idx + 1}`,
      raceNumber: driver.race_number,
      teamId: driver.team_id ?? -1,
      teamColor: driver.livery_color || FALLBACK_TEAM_COLOR,
      restricted: markRestricted && driver.idx !== playerIdx && driver.your_telemetry !== 1,
      isDisabled: disableOthers && driver.idx !== playerIdx,
    }))
    .sort((a, b) => a.raceNumber - b.raceNumber || a.value - b.value)
}
