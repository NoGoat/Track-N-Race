import type { SessionMsg, WeatherForecastSample } from '../types'

/**
 * The packet carries forecasts for several sessions (e.g. qualifying and the
 * race), each a block starting again at offset 0. Returns the block for the
 * session being driven — matched by session_type, otherwise (older
 * recordings) the first block. Mirrors libtnrp's currentSessionForecast, which
 * Strategy plans with.
 */
export function currentSessionForecast(session: SessionMsg): WeatherForecastSample[] {
  const samples = session.weather_forecast_samples
  const matched = samples.filter(s => s.session_type === session.session_type)
  if (matched.length > 0) return matched
  const out: WeatherForecastSample[] = []
  for (let i = 0; i < samples.length; i++) {
    if (i > 0 && samples[i].time_offset <= samples[i - 1].time_offset) break
    out.push(samples[i])
  }
  return out
}
