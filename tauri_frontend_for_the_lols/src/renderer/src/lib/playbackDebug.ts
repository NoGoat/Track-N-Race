import { isAdditionalLoggingEnabled } from './debugSettings'

export function playbackDebug(event: string, details: Record<string, unknown>): void {
  if (!isAdditionalLoggingEnabled()) return
  const payload = JSON.stringify(details, (_key, value) =>
    typeof value === 'number' && !Number.isFinite(value) ? String(value) : value)
  console.info(`[playback-debug] ${new Date().toISOString()} ${event} ${payload}`)
}
