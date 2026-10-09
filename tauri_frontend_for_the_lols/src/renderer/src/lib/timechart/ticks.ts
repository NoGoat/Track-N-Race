// "Nice" tick generation for auto-ranged y-axes (tyre temps / wear), matching
// the round tick values uPlot produces for an unconstrained scale. Standard
// nice-number algorithm; returns ascending tick values that fall within
// [min, max].

function niceNum(range: number, round: boolean): number {
  const exp = Math.floor(Math.log10(range))
  const frac = range / Math.pow(10, exp)
  let nf: number
  if (round) nf = frac < 1.5 ? 1 : frac < 3 ? 2 : frac < 7 ? 5 : 10
  else nf = frac <= 1 ? 1 : frac <= 2 ? 2 : frac <= 5 ? 5 : 10
  return nf * Math.pow(10, exp)
}

export function niceTicks(min: number, max: number, count = 5): number[] {
  if (!Number.isFinite(min) || !Number.isFinite(max)) return []
  if (min === max) return [min]
  const range = niceNum(max - min, false)
  const step = niceNum(range / Math.max(1, count - 1), true)
  const niceMin = Math.ceil(min / step) * step
  const niceMax = Math.floor(max / step) * step
  const ticks: number[] = []
  // guard against fp drift producing a runaway loop
  for (let v = niceMin, i = 0; v <= niceMax + step * 0.5 && i < 100; v += step, i++) {
    ticks.push(Math.round(v * 1e6) / 1e6)
  }
  return ticks
}
