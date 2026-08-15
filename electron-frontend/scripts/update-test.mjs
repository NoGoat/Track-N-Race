import { spawn } from 'node:child_process'

const npmCommand = process.platform === 'win32' ? 'npm.cmd' : 'npm'
const childEnvironment = { ...process.env, TRACK_N_RACE_UPDATE_TEST: '1' }
delete childEnvironment.ELECTRON_RUN_AS_NODE

const child = spawn(npmCommand, ['run', 'dev'], {
  cwd: process.cwd(),
  env: childEnvironment,
  stdio: 'inherit',
})

for (const signal of ['SIGINT', 'SIGTERM']) {
  process.on(signal, () => child.kill(signal))
}

child.on('error', error => {
  console.error('[update-test] Could not start the app:', error)
  process.exitCode = 1
})

child.on('exit', (code, signal) => {
  if (signal) process.kill(process.pid, signal)
  else process.exitCode = code ?? 1
})
