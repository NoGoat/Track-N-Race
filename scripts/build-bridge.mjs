// Builds the native protocol_parser bridge (and libtnrp) via CMake.
// Hooked into predev/prebuild/predist/prepack so the bridge binary always exists
// before Electron tries to spawn it. Incremental: re-running is cheap when nothing
// changed. Skip with TNR_SKIP_BRIDGE=1 (e.g. for renderer-only UI work without a
// C++ toolchain).

import { spawnSync } from 'node:child_process'
import { existsSync } from 'node:fs'
import os from 'node:os'
import { fileURLToPath } from 'node:url'
import { dirname, join } from 'node:path'

const __dirname = dirname(fileURLToPath(import.meta.url))
const root = join(__dirname, '..')
const srcDir = join(root, 'protocol_parser')
const buildDir = join(srcDir, 'build')

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

// Leave headroom so the build never pins the machine (per project convention).
const jobs = Math.max(1, os.cpus().length - 2)

function run(args) {
  console.log(`[build-bridge] cmake ${args.join(' ')}`)
  const r = spawnSync('cmake', args, { stdio: 'inherit', cwd: root, shell: false })
  if (r.status !== 0) {
    console.error('[build-bridge] cmake failed — see output above.')
    process.exit(r.status ?? 1)
  }
}

// Configure (idempotent; cheap after the first run). FetchContent pulls json+zlib
// on the first configure only.
if (!existsSync(join(buildDir, 'CMakeCache.txt'))) {
  run(['-S', srcDir, '-B', buildDir, '-DCMAKE_BUILD_TYPE=Release'])
}

// Build. --config Release covers multi-config generators (Visual Studio);
// it's a harmless no-op on single-config ones (Make/Ninja).
run(['--build', buildDir, '--config', 'Release', '--parallel', String(jobs)])

console.log('[build-bridge] protocol_parser built.')
