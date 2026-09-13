import { app, crashReporter, ipcMain } from 'electron'
import * as fs from 'fs'
import * as os from 'os'
import * as path from 'path'
import { formatWithOptions } from 'util'
import { configStore } from './configStore'

export interface Diagnostics {
  directory: string
  mainLogPath: string
  chromiumLogPath: string
  ramUsageLogPath: string
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
let ramUsageLogFd: number | null = null
let ramUsageTimer: NodeJS.Timeout | null = null
let ramUsageStartedAt = 0
let writingRamUsage = false
let memoryLogEnabled = false
let ramUsageLogPath: string | null = null
let ramUsageLogOpened = false
let ramUsageReadyListener: (() => void) | null = null
let memoryLogSubscriptionInstalled = false
let fatalFlushHandler: (() => boolean) | null = null
let telemetryRetentionProvider: (() => Record<string, unknown>) | null = null
let latestRendererTelemetryRetention: Record<string, unknown> | null = null
let telemetryRetentionCaptureInstalled = false

export function setTelemetryRetentionProvider(
  provider: (() => Record<string, unknown>) | null,
): void {
  telemetryRetentionProvider = provider
}

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

function installTelemetryRetentionCapture(): void {
  if (telemetryRetentionCaptureInstalled) return
  telemetryRetentionCaptureInstalled = true
  ipcMain.on('diagnostics:telemetry-retention', (_event, value: unknown) => {
    if (!memoryLogEnabled) return
    if (!value || typeof value !== 'object' || Array.isArray(value)) return
    try {
      // Clone the small diagnostic document so the sampler never retains an
      // Electron IPC wrapper or an unexpectedly large renderer-owned object.
      const json = JSON.stringify(value)
      if (json.length > 256 * 1024) return
      latestRendererTelemetryRetention = JSON.parse(json) as Record<string, unknown>
    } catch {
      // Diagnostics input is best-effort and must never affect the application.
    }
  })
}

function finiteNumber(value: unknown): number {
  return typeof value === 'number' && Number.isFinite(value) ? value : 0
}

function writeRamUsageSample(): void {
  if (ramUsageLogFd === null || writingRamUsage) return
  writingRamUsage = true

  try {
    const metrics = app.getAppMetrics()
    const processes = metrics
      .map(metric => ({
        pid: metric.pid,
        type: metric.type,
        name: metric.name ?? metric.serviceName ?? null,
        working_set_kb: metric.memory.workingSetSize,
        private_kb: metric.memory.privateBytes ?? null,
      }))
      .sort((left, right) => right.working_set_kb - left.working_set_kb)

    const hasCompletePrivateMemory = processes.length > 0 &&
      processes.every(metric => metric.private_kb !== null)
    let mainTelemetryRetention: Record<string, unknown> | null = null
    try {
      mainTelemetryRetention = telemetryRetentionProvider?.() ?? null
    } catch (error) {
      originalConsole.error('[diagnostics] unable to collect main telemetry retention:', error)
    }
    const rendererTelemetryRetention = latestRendererTelemetryRetention
    const estimatedRetainedBytes =
      finiteNumber(mainTelemetryRetention?.retained_bytes) +
      finiteNumber(rendererTelemetryRetention?.estimated_retained_bytes)
    const rendererSampledAt = typeof rendererTelemetryRetention?.sampled_at === 'string'
      ? Date.parse(rendererTelemetryRetention.sampled_at)
      : NaN
    const telemetryData = {
      type: 'TelemetryData',
      name: 'Application-held telemetry',
      mode: rendererTelemetryRetention?.mode ?? mainTelemetryRetention?.mode ?? 'unknown',
      estimated_retained_bytes: estimatedRetainedBytes,
      estimated_retained_kb: estimatedRetainedBytes / 1024,
      already_included_in_process_totals: true,
      attribution_scope: 'Electron telemetry stores, published views, chart CPU/GPU pages, ' +
        'seek/resume buffers, native transit queues, and native live-history allocation capacity; ' +
        'remaining protocol-engine cache/container overhead remains only in process totals',
      renderer_sample_age_ms: Number.isFinite(rendererSampledAt)
        ? Math.max(0, Date.now() - rendererSampledAt)
        : null,
      main: mainTelemetryRetention,
      renderer: rendererTelemetryRetention,
    }
    const sample = {
      timestamp: new Date().toISOString(),
      elapsed_ms: Date.now() - ramUsageStartedAt,
      total_working_set_kb: processes.reduce((total, metric) => total + metric.working_set_kb, 0),
      total_private_kb: hasCompletePrivateMemory
        ? processes.reduce((total, metric) => total + (metric.private_kb ?? 0), 0)
        : null,
      process_count: processes.length,
      processes,
      category_count: processes.length + 1,
      categories: [
        ...processes.map(processMetric => ({ category: 'process', ...processMetric })),
        { category: 'telemetry_data', ...telemetryData },
      ],
      telemetry_data: telemetryData,
    }

    fs.writeSync(ramUsageLogFd, `${JSON.stringify(sample)}\n`)
  } catch (error) {
    originalConsole.error('[diagnostics] unable to write RAM usage log:', error)
  } finally {
    writingRamUsage = false
  }
}

function startRamUsageProfiler(logPath: string): void {
  ramUsageLogPath = logPath
  if (ramUsageLogFd === null) {
    ramUsageLogFd = fs.openSync(logPath, ramUsageLogOpened ? 'a' : 'w')
    ramUsageLogOpened = true
  }

  const startSampling = (): void => {
    ramUsageReadyListener = null
    if (!memoryLogEnabled) return
    if (ramUsageLogFd === null || ramUsageTimer !== null) return
    ramUsageStartedAt = Date.now()
    writeRamUsageSample()
    ramUsageTimer = setInterval(writeRamUsageSample, 1000)
    ramUsageTimer.unref()
  }

  if (app.isReady()) startSampling()
  else {
    ramUsageReadyListener = startSampling
    app.once('ready', startSampling)
  }
}

function stopRamUsageProfiler(): void {
  if (ramUsageReadyListener !== null) {
    app.removeListener('ready', ramUsageReadyListener)
    ramUsageReadyListener = null
  }
  if (ramUsageTimer !== null) {
    clearInterval(ramUsageTimer)
    ramUsageTimer = null
  }
  if (ramUsageLogFd !== null) {
    try {
      fs.closeSync(ramUsageLogFd)
    } catch {
      // The process is already exiting; there is nowhere else to report this.
    }
    ramUsageLogFd = null
  }
  latestRendererTelemetryRetention = null
}

function configureMemoryLog(enabled: boolean): void {
  memoryLogEnabled = enabled
  if (enabled) {
    if (ramUsageLogPath !== null) startRamUsageProfiler(ramUsageLogPath)
  } else {
    stopRamUsageProfiler()
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
    stopRamUsageProfiler()
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
    ramUsageLogPath: path.join(directory, 'ram_usage.log'),
  }

  logFd = fs.openSync(diagnostics.mainLogPath, 'w')
  installConsoleCapture()
  installProcessCapture()
  installTelemetryRetentionCapture()
  ramUsageLogPath = diagnostics.ramUsageLogPath
  configureMemoryLog(configStore.get('debug.memoryLog', false) === true)
  if (!memoryLogSubscriptionInstalled) {
    memoryLogSubscriptionInstalled = true
    configStore.onDidChange('debug.memoryLog', value => configureMemoryLog(value === true))
  }

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

  console.log('[diagnostics] RAM usage log:', diagnostics.ramUsageLogPath)
  logStartupMetadata(diagnostics, appVersion)
  return diagnostics
}
