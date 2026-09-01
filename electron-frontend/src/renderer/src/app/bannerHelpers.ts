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

export function buildBanner(
  event: RaceEventMsg,
  participants: ParticipantsMsg | null,
  labels: Readonly<Record<string, string>>,
): BannerItem | null {
  switch (event.code) {
    case 'SCAR': return null
    case 'FTLP': return { label: 'Fastest Lap', sub: `${lastName(participants, event.car_idx ?? 0)}  ·  ${fmtLap(event.lap_time_s ?? 0)}`, color: '#BF5FFF' }
    case 'DRSE': return { label: 'DRS Enabled', color: '#37872D' }
    case 'DRSD': return { label: 'DRS Disabled', color: '#8e8e8e' }
    case 'RDFL': return { label: 'Red Flag', color: '#e10600' }
    case 'PENA': {
      const penaltyType = event.penalty_type ?? 0
      const penaltyLabel = labels[`penalty.${penaltyType}`]
      if (!penaltyLabel) return null
      const color = penaltyType === 5 ? '#ffd700'
        : penaltyType === 2 || penaltyType === 4 ? '#c47d0e'
        : '#e10600'
      const time = (penaltyType === 1 || penaltyType === 4) && event.penalty_time_s ? ` ${event.penalty_time_s}s` : ''
      const driver = lastName(participants, event.car_idx ?? 0)
      const infringement = event.infringement_type != null ? labels[`infringe.${event.infringement_type}`] : undefined
      return { label: penaltyLabel + time, sub: penaltyType === 5 && infringement ? `${driver}  ·  ${infringement}` : driver, color }
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
