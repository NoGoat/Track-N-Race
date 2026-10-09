// F2 actual compound IDs agree in the local F1 24, F1 25 and 2026 Season Pack
// UDP specifications (CarStatusData.m_actualTyreCompound). F2 colours are fixed
// by actual compound, regardless of the reported visual compound or theme.
const F2_COLORS: Record<number, string> = {
  11: '#a855f7', // Supersoft — purple
  12: '#e8002d', // Soft — red
  13: '#ffd700', // Medium — yellow
  14: '#ffffff', // Hard — white
  15: '#4488ff', // Wet — blue
}

const F1_VISUAL_COLORS: Record<number, string> = {
  16: 'var(--compound-soft)',
  17: 'var(--compound-medium)',
  18: 'var(--compound-hard)',
  7: 'var(--compound-inter)',
  8: 'var(--compound-wet)',
}

export function tyreCompoundColor(actual: number, visual: number): string | undefined {
  return F2_COLORS[actual] ?? F1_VISUAL_COLORS[visual]
}

export function dryTyreCompoundOrder(actual: number, visual: number): number {
  if (actual >= 11 && actual <= 14) return actual - 11
  return ({ 16: 0, 17: 1, 18: 2 } as Record<number, number>)[visual] ?? 3
}
