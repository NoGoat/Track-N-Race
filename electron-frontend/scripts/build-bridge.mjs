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
// Invoke cmake-js through Node directly. Spawning npx.cmd required shell:true
// on Windows, which concatenates arguments into a command string and triggers
// Node's DEP0190 command-injection warning.
const cmakeJsCli = join(root, 'node_modules', 'cmake-js', 'bin', 'cmake-js')
const r = spawnSync(process.execPath, [cmakeJsCli, 'compile', '-d', 'node_addon'], {
  stdio: 'inherit',
  cwd: root,
  shell: false
})

if (r.status !== 0) {
  if (r.error) {
    console.error('[build-bridge] failed to launch cmake-js:', r.error)
  } else if (r.signal) {
    console.error(`[build-bridge] cmake-js was killed by signal ${r.signal}`)
  } else {
    console.error('[build-bridge] cmake-js failed — see output above.')
  }
  process.exit(r.status ?? 1)
}

console.log('[build-bridge] protocol_parser.node built successfully.')
