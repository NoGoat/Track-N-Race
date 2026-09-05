import { app, crashReporter } from 'electron'
import * as fs from 'fs'
import * as os from 'os'
import * as path from 'path'
import { formatWithOptions } from 'util'

export interface Diagnostics {
  directory: string
  mainLogPath: string
  chromiumLogPath: string
}

type ConsoleMethod = 'debug' | 'info' | 'log' | 'warn' | 'error'

const originalConsole: Record<ConsoleMethod, (...args: unknown[]) => void> = {
  debug: console.debug.bind(console),
  info: console.info.bind(console),
  log: console.log.bind(console),
  warn: console.warn.bind(console),
  error: console.error.bind(console),
}

let logFd: number | null = null
let fatalFlushHandler: (() => boolean) | null = null

// Registered by application.ts after the native bridge module is available.
// Kept as a callback so diagnostics can still initialize before bridgeManager
// and report native-addon startup failures.
export function setFatalFlushHandler(handler: (() => boolean) | null): void {
  fatalFlushHandler = handler
}

function tryFatalFlush(reason: string): void {
  if (!fatalFlushHandler) return
  try {
    if (fatalFlushHandler())
      write('INFO', [`recording buffer flushed (${reason})`])
  } catch (error) {
    write('ERROR', [`recording buffer flush failed (${reason})`, error])
  }
}
let writing = false

function stringify(args: unknown[]): string {
  try {
    return formatWithOptions({
      colors: false,
      depth: 8,
      maxArrayLength: 200,
      maxStringLength: 20_000,
      breakLength: 160,
    }, ...args)
  } catch (error) {
    return `[diagnostics formatting failed: ${String(error)}]`
  }
}

function write(level: string, args: unknown[]): void {
  if (logFd === null || writing) return
  writing = true
  try {
    const message = stringify(args).replace(/\r?\n/g, '\n    ')
    fs.writeSync(logFd, `${new Date().toISOString()} [${process.pid}] ${level.padEnd(5)} ${message}\n`)
  } catch (error) {
    originalConsole.error('[diagnostics] unable to write log:', error)
  } finally {
    writing = false
  }
}

function installConsoleCapture(): void {
  const levels: Record<ConsoleMethod, string> = {
    debug: 'DEBUG',
    info: 'INFO',
    log: 'INFO',
    warn: 'WARN',
    error: 'ERROR',
  }

  for (const method of Object.keys(levels) as ConsoleMethod[]) {
    console[method] = (...args: unknown[]): void => {
      originalConsole[method](...args)
      write(levels[method], args)
    }
  }
}

function installProcessCapture(): void {
  process.on('uncaughtExceptionMonitor', (error, origin) => {
    write('FATAL', ['uncaughtException', { origin }, error])
    tryFatalFlush('uncaught exception')
  })
  process.on('unhandledRejection', (reason, promise) => {
    write('ERROR', ['unhandledRejection', { reason, promise }])
    tryFatalFlush('unhandled rejection')
  })
  process.on('warning', (warning) => {
    write('WARN', ['process warning', warning])
  })
  process.on('exit', (code) => {
    tryFatalFlush(`process exit ${code}`)
    write('INFO', [`process exit, code=${code}`])
    if (logFd !== null) {
      try {
        fs.closeSync(logFd)
      } catch {
        // The process is already exiting; there is nowhere else to report this.
      }
      logFd = null
    }
  })
}

function logStartupMetadata(diagnostics: Diagnostics, appVersion: string): void {
  console.log('[diagnostics] launch log:', diagnostics.mainLogPath)
  console.log('[diagnostics] previous launch diagnostics deleted')
  console.log('[diagnostics] startup:', {
    appVersion,
    packaged: app.isPackaged,
    electron: process.versions.electron,
    chrome: process.versions.chrome,
    node: process.versions.node,
    platform: process.platform,
    architecture: process.arch,
    osVersion: process.getSystemVersion(),
    executable: process.execPath,
    resources: process.resourcesPath,
    workingDirectory: process.cwd(),
    commandLine: process.argv,
    userData: app.getPath('userData'),
    locale: app.getLocale(),
    networkInterfaces: Object.fromEntries(
      Object.entries(os.networkInterfaces()).map(([name, addresses]) => [
        name,
        (addresses ?? []).map(address => ({
          address: address.address,
          netmask: address.netmask,
          family: address.family,
          internal: address.internal,
          cidr: address.cidr,
          scopeid: address.scopeid,
        })),
      ])
    ),
  })
}

/**
 * Starts diagnostics before the application module (and native addon) loads.
 * The directory is deliberately replaced on every primary launch, leaving one
 * small, self-contained bundle for the user to send after a failed launch.
 */
export function initializeDiagnostics(appVersion: string): Diagnostics {
  const directory = path.join(app.getPath('userData'), 'launch-diagnostics')
  fs.rmSync(directory, { recursive: true, force: true })
  fs.mkdirSync(directory, { recursive: true })

  const diagnostics: Diagnostics = {
    directory,
    mainLogPath: path.join(directory, 'main.log'),
    chromiumLogPath: path.join(directory, 'chromium.log'),
  }

  logFd = fs.openSync(diagnostics.mainLogPath, 'w')
  installConsoleCapture()
  installProcessCapture()

  // Chromium/GPU/network-service diagnostics and local native crash dumps live
  // beside main.log and are swept with it at the beginning of the next launch.
  const crashDumpsPath = path.join(directory, 'crash-dumps')
  fs.mkdirSync(crashDumpsPath, { recursive: true })
  app.setPath('crashDumps', crashDumpsPath)
  app.setAppLogsPath(directory)
  app.commandLine.appendSwitch('enable-logging', 'file')
  app.commandLine.appendSwitch('log-file', diagnostics.chromiumLogPath)
  crashReporter.start({
    productName: 'Track N Race',
    companyName: 'Track N Race',
    uploadToServer: false,
    compress: false,
  })

  logStartupMetadata(diagnostics, appVersion)
  return diagnostics
}
