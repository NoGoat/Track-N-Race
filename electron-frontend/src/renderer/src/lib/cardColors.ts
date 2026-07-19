import type { ColorSpec } from '../types'

// Renderer side of the library-owned card-colour model (tnrp/CardColors.h). The
// per-key specs arrive in the protocol_status row (cardColors); this module maps
// the semantic tokens to theme-aware colours and evaluates a spec against the
// current data. Keeping the rule structure in the library means the native
// recorder and the Electron app use identical thresholds.

// token → colour, theme-aware. `neutral` returns undefined so the card falls back
// to its default text colour.
export function tokenColor(token: string, isDark: boolean): string | undefined {
  switch (token) {
    case 'pos':            return isDark ? '#37872D' : '#137333'
    case 'neg':            return '#C4162A'
    case 'warn':           return isDark ? '#d4ad04' : '#B06000'
    case 'warnAlt':        return isDark ? '#c47d0e' : '#C26400'
    case 'info':           return isDark ? '#5794F2' : '#0B57D0'
    case 'ice':            return isDark ? '#5794F2' : '#0B57D0'
    case 'mguk':           return isDark ? '#FADE2A' : '#B06000'
    case 'fuel':           return isDark ? '#F0A500' : '#C26400'
    case 'off':            return isDark ? 'var(--text-secondary)' : '#565B70'
    case 'wear1':          return isDark ? '#73BF69' : '#137333'
    case 'wear2':          return '#A8D436'
    case 'wear3':          return '#FF9830'
    case 'compoundSoft':   return 'var(--compound-soft)'
    case 'compoundMedium': return 'var(--compound-medium)'
    case 'compoundHard':   return 'var(--compound-hard)'
    case 'compoundInter':  return 'var(--compound-inter)'
    case 'compoundWet':    return 'var(--compound-wet)'
    case 'neutral':
    default:               return undefined
  }
}

function cmp(op: string, lhs: number, rhs: number): boolean {
  switch (op) {
    case 'lt':  return lhs < rhs
    case 'lte': return lhs <= rhs
    case 'gt':  return lhs > rhs
    case 'gte': return lhs >= rhs
    case 'eq':  return lhs === rhs
    default:    return false
  }
}

// First satisfied rule wins; `self` reads the card's own value, otherwise the
// named field from `fields`. Missing inputs (NaN/undefined) skip the rule.
export function evalColorToken(
  spec: ColorSpec | undefined,
  fields: Record<string, number>,
  self: number,
): string {
  if (!spec) return 'neutral'
  for (const r of spec.rules) {
    const lhs = r.on === 'self' ? self : fields[r.on]
    if (lhs === undefined || Number.isNaN(lhs)) continue
    if (cmp(r.op, lhs, r.value)) return r.color
  }
  return spec.default
}
