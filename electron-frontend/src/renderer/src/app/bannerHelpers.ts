import type { ParticipantsMsg, RaceEventMsg } from '../types'

export interface BannerItem { label: string; sub?: string; color: string }

export function lastName(participants: ParticipantsMsg | null, idx: number): string {
  const driver = participants?.drivers.find(item => item.idx === idx)
  if (!driver) return `Car ${idx}`
  const parts = driver.name.trim().split(/\s+/)
  return parts[parts.length - 1]
}

export function fmtLap(seconds: number): string {
  const minutes = Math.floor(seconds / 60)
  const remainder = seconds % 60
  return `${minutes}:${remainder.toFixed(3).padStart(6, '0')}`
}

const INFRINGEMENT_LABELS: Record<number, string> = {
  0: 'Blocking by slowing', 1: 'Blocking wrong way', 2: 'Reversing off start line', 3: 'Severe collision',
  4: 'Collision', 5: 'Collision — failed to hand back', 6: 'Collision — attack from rear',
  7: 'SC delta exceeded', 8: 'SC illegal overtake', 9: 'SC exceeding allowed pace', 10: 'Cornering under SC',
  11: 'SC must pit this lap', 12: 'SC pit lane curfew', 13: 'Pit lane too fast', 14: 'Unsafe release',
  15: 'Pit re-entry too slow', 16: 'In pit too fast', 17: 'Unsafe release', 18: 'Escape from pit',
  19: 'Ignoring blue flags', 20: 'Ignoring yellow flags', 21: 'Ignoring drive through', 22: 'Too many drive throughs',
  23: 'DT — serve this lap', 24: 'DT — serve next lap', 25: 'Pit stop failed to serve', 26: 'Hanging around',
  27: 'Hang around for SC', 28: 'Return to pits', 29: 'Tyre regulations', 30: 'Lap invalidated',
  31: 'This + next lap invalid', 32: 'Lap invalid (no reason)', 33: 'This + next invalid (no reason)',
  34: 'This + prev lap invalid', 35: 'This + prev invalid (no reason)', 36: 'Retired', 37: 'Black flag timer',
  38: 'Unserved stop-go', 39: 'Unserved drive through', 40: 'Engine change', 41: 'Gearbox change',
  42: 'Parc fermé change', 43: 'League grid penalty', 44: 'Retry penalty', 45: 'Illegal time gain',
  46: 'Mandatory pit stop', 47: 'Attribute assigned', 48: 'Corner cutting',
}

export function buildBanner(event: RaceEventMsg, participants: ParticipantsMsg | null): BannerItem | null {
  switch (event.code) {
    case 'SCAR': return null
    case 'FTLP': return { label: 'Fastest Lap', sub: `${lastName(participants, event.car_idx ?? 0)}  ·  ${fmtLap(event.lap_time_s ?? 0)}`, color: '#BF5FFF' }
    case 'DRSE': return { label: 'DRS Enabled', color: '#37872D' }
    case 'DRSD': return { label: 'DRS Disabled', color: '#8e8e8e' }
    case 'RDFL': return { label: 'Red Flag', color: '#e10600' }
    case 'PENA': {
      const penaltyType = event.penalty_type ?? 0
      const labels: Record<number, string> = { 0: 'Drive Through', 1: 'Stop Go', 2: 'Grid Penalty', 4: 'Time Penalty', 5: 'Warning', 6: 'Disqualified' }
      const colors: Record<number, string> = { 0: '#e10600', 1: '#e10600', 2: '#c47d0e', 4: '#c47d0e', 5: '#ffd700', 6: '#e10600' }
      if (!(penaltyType in labels)) return null
      const time = (penaltyType === 1 || penaltyType === 4) && event.penalty_time_s ? ` ${event.penalty_time_s}s` : ''
      const driver = lastName(participants, event.car_idx ?? 0)
      const infringement = event.infringement_type != null ? INFRINGEMENT_LABELS[event.infringement_type] : undefined
      return { label: labels[penaltyType] + time, sub: penaltyType === 5 && infringement ? `${driver}  ·  ${infringement}` : driver, color: colors[penaltyType] }
    }
    case 'DTSV': return { label: 'DT Served', sub: lastName(participants, event.car_idx ?? 0), color: '#a0a8b8' }
    case 'SGSV': return { label: 'SG Served', sub: lastName(participants, event.car_idx ?? 0), color: '#a0a8b8' }
    case 'RTMT': return { label: 'Retired', sub: lastName(participants, event.car_idx ?? 0), color: '#a0a8b8' }
    case 'RCWN': return { label: 'Race Winner', sub: lastName(participants, event.car_idx ?? 0), color: '#FFD700' }
    case 'CHQF': return { label: 'Chequered Flag', color: '#7a7a7a' }
    case 'LGOT': return { label: 'Lights Out', color: '#37872D' }
    case 'SSTA': return { label: 'Session Start', color: '#5794F2' }
    case 'SEND': return { label: 'Session End', color: '#5794F2' }
    default: return null
  }
}
