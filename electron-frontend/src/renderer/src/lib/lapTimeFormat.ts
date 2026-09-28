export function fmtMs(ms: number): string {
  if (ms <= 0) return '--:--.---'
  const m     = Math.floor(ms / 60_000)
  const s     = Math.floor((ms % 60_000) / 1000)
  const mills = ms % 1000
  return `${m}:${String(s).padStart(2, '0')}.${String(mills).padStart(3, '0')}`
}

export function fmtSector(ms: number): string {
  if (ms <= 0) return '—'
  const s     = Math.floor(ms / 1000)
  const mills = ms % 1000
  return `${s}.${String(mills).padStart(3, '0')}`
}
