// Runs `electron-vite <command>` and handles app switches electron-vite itself
// would reject as unknown options:
//   npm start -- --first-time-setup
//   npm run dev -- --first-time-setup
// --first-time-setup becomes TNR_FIRST_TIME_SETUP=1, which the main process
// reads (src/main/firstTimeSetup.ts). Every other argument goes to electron-vite.
import { spawnSync } from 'node:child_process'
import { createRequire } from 'node:module'
import { dirname, join } from 'node:path'

const require = createRequire(import.meta.url)
const electronViteBin = join(dirname(require.resolve('electron-vite/package.json')), 'bin', 'electron-vite.js')

const env = { ...process.env }
const args = []
for (const arg of process.argv.slice(2)) {
  if (arg.toLowerCase() === '--first-time-setup') env.TNR_FIRST_TIME_SETUP = '1'
  else args.push(arg)
}

const result = spawnSync(process.execPath, [electronViteBin, ...args], { stdio: 'inherit', env })
if (result.error) {
  console.error('[electron-vite] Failed to start:', result.error)
  process.exit(1)
}
process.exit(result.status ?? 1)
