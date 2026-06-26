import { spawnSync } from 'node:child_process'
import { existsSync } from 'node:fs'
import os from 'node:os'
import { fileURLToPath } from 'node:url'
import { dirname, join } from 'node:path'

const __dirname = dirname(fileURLToPath(import.meta.url))
const root = join(__dirname, '..')

if (process.env.TNR_SKIP_BRIDGE === '1') {
  console.log('[build-bridge] TNR_SKIP_BRIDGE=1 — skipping bridge build')
  process.exit(0)
}

function have(cmd) {
  const probe = spawnSync(cmd, ['--version'], { stdio: 'ignore', shell: false })
  return probe.status === 0
}

if (!have('cmake')) {
  console.error(
    '[build-bridge] cmake not found on PATH.\n' +
    '  Install CMake + a C++ toolchain to build the telemetry bridge,\n' +
    '  or set TNR_SKIP_BRIDGE=1 to skip (live telemetry will not work).'
  )
  process.exit(1)
}

const jobs = Math.max(1, os.cpus().length - 2)
process.env.CMAKE_BUILD_PARALLEL_LEVEL = String(jobs)

console.log(`[build-bridge] Compiling N-API addon via cmake-js (Threads: ${jobs})...`)
const cmd = process.platform === 'win32' ? 'npx.cmd' : 'npx'
const r = spawnSync(cmd, ['cmake-js', 'compile', '-d', 'node_addon'], { 
  stdio: 'inherit', 
  cwd: root, 
  shell: false 
})

if (r.status !== 0) {
  console.error('[build-bridge] cmake-js failed — see output above.')
  process.exit(r.status ?? 1)
}

console.log('[build-bridge] protocol_parser.node built successfully.')
