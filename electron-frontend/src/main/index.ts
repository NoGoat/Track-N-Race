import { app, dialog } from 'electron'
import { initializeDiagnostics } from './diagnostics'

declare const __APP_VERSION__: string

// Do not let a file-association activation or shortcut double-click erase the
// diagnostics of the already-running primary process.
const gotSingleInstanceLock = app.requestSingleInstanceLock()
if (!gotSingleInstanceLock) {
  app.quit()
  process.exit(0)
}

let logPath = ''

function failStartup(error: unknown): void {
  console.error('[bootstrap] fatal startup failure:', error)
  const location = logPath ? `\n\nDiagnostic log:\n${logPath}` : ''
  dialog.showErrorBox(
    'Track N Race Failed to Start',
    `The application encountered a fatal error during startup.${location}\n\n${String(error)}`
  )
  app.exit(1)
}

try {
  const diagnostics = initializeDiagnostics(app.isPackaged ? __APP_VERSION__ : 'Next')
  logPath = diagnostics.mainLogPath
  console.log('[bootstrap] single-instance lock acquired')

  // Deliberately dynamic: diagnostics must be live before application.ts and
  // bridgeManager.ts evaluate, since native-module import failures can happen
  // during module loading. import() also makes the module visible to Rollup.
  void import('./application').catch(failStartup)
} catch (error) {
  failStartup(error)
}
