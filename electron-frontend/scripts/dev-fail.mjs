import { spawnSync } from 'node:child_process'

const npm = process.platform === 'win32' ? 'npm.cmd' : 'npm'
const childEnv = {
  ...process.env,
  TNR_SIMULATE_BRIDGE_FAILURE: '1'
}
delete childEnv.ELECTRON_RUN_AS_NODE

const result = spawnSync(npm, ['run', 'dev'], {
  stdio: 'inherit',
  shell: process.platform === 'win32',
  env: childEnv
})

if (result.error) {
  console.error('[dev:fail] Failed to launch the development app:', result.error)
  process.exit(1)
}

if (result.signal) {
  console.error(`[dev:fail] Development app exited from signal ${result.signal}`)
  process.exit(1)
}

process.exit(result.status ?? 1)
