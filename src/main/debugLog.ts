// TEMPORARY strategy debugging — appends tagged JSON lines to a fixed absolute
// path so it's always findable. Remove once the strategy/seek issue is diagnosed.
import * as fs from 'fs'

const LOG_PATH = '/home/nogoat/code/Track-N-Race/strategy-debug.log'
let cleared = false

export function debugLog(tag: string, data: unknown): void {
  try {
    if (!cleared) {
      try { fs.writeFileSync(LOG_PATH, '') } catch {}
      cleared = true
      console.log('[debugLog] writing strategy logs to:', LOG_PATH)
    }
    fs.appendFileSync(LOG_PATH, `${new Date().toISOString()} ${tag} ${JSON.stringify(data)}\n`)
  } catch (e) {
    console.error('[debugLog] failed:', e)
  }
}

export const DEBUG_LOG_PATH = LOG_PATH
