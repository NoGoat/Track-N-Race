// A team override is a fixed hex color, or "livery" for the F1 25/26 packet color.
export type TeamColorOverrides = Record<string, Record<string, string>>

export function normalizeTeamColorOverrides(value: unknown): TeamColorOverrides {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return {}
  const result: TeamColorOverrides = {}
  for (const format of ['2024', '2025', '2026']) {
    const teams = (value as Record<string, unknown>)[format]
    if (!teams || typeof teams !== 'object' || Array.isArray(teams)) continue
    for (const [id, color] of Object.entries(teams as Record<string, unknown>)) {
      if (!/^\d+$/.test(id) || typeof color !== 'string') continue
      const normalized = color === 'livery' && format !== '2024'
        ? 'livery'
        : /^#[0-9a-f]{6}$/i.test(color) ? color.toUpperCase() : null
      if (normalized !== null) (result[format] ??= {})[id] = normalized
    }
  }
  return result
}
