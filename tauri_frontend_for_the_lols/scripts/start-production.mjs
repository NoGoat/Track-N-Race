import { spawnSync } from 'node:child_process'
import { existsSync, readFileSync } from 'node:fs'
import { join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const root = fileURLToPath(new URL('../', import.meta.url))
const nativeRoot = join(root, 'src-tauri')

// Invoke executables directly so paths and recording filenames with spaces
// work on Windows as well as macOS/Linux.
function run(command, args, capture = false) {
  const result = spawnSync(command, args, {
    cwd: nativeRoot,
    stdio: capture ? ['ignore', 'pipe', 'inherit'] : 'inherit',
    encoding: 'utf8',
    windowsHide: true,
  })
  if (result.error) throw result.error
  if (result.status !== 0) {
    if (result.signal) console.error(command + ' stopped by ' + result.signal)
    process.exit(result.status ?? 1)
  }
  return result.stdout?.trim()
}

try {
  // Use the current machine's target even if Cargo has a cross-build default.
  const target = run('rustc', ['--print', 'host-tuple'], true)
  const metadata = JSON.parse(run('cargo', [
    'metadata', '--format-version', '1', '--no-deps',
    '--manifest-path', join(nativeRoot, 'Cargo.toml'),
  ], true))
  const pkg = metadata.packages.find(pkg =>
    resolve(pkg.manifest_path) === join(nativeRoot, 'Cargo.toml'))
  const binary = pkg?.targets.find(target =>
    target.kind.includes('bin') && (!pkg.default_run || target.name === pkg.default_run))
  if (!binary) throw new Error('Could not determine the Tauri application executable')
  const config = JSON.parse(readFileSync(join(nativeRoot, 'tauri.conf.json'), 'utf8'))
  const name = config.mainBinaryName ?? binary.name

  console.log('Building Track N Race in production mode...')
  run(process.execPath, [
    join(root, 'node_modules', '@tauri-apps', 'cli', 'tauri.js'),
    'build', '--no-bundle', '--target', target,
  ])

  const executable = join(metadata.target_directory, target, 'release',
    name + (process.platform === 'win32' ? '.exe' : ''))
  if (!existsSync(executable)) throw new Error('Build completed but the executable is missing: ' + executable)
  console.log('Starting ' + executable)
  // Forward arguments from npm start -- <recording.tnrd>.
  run(executable, process.argv.slice(2))
} catch (error) {
  console.error('Production start failed:', error.message)
  process.exitCode = 1
}
