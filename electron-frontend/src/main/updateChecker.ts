import { app, net } from 'electron'
import { spawn } from 'child_process'
import * as fs from 'fs'
import * as path from 'path'
import { configStore as store } from './configStore'
import { nativeUpdates, type NativeUpdates } from './bridgeManager'

export interface AvailableUpdate {
  currentVersion: string
  latestVersion: string
  releaseUrl: string
  publishedAt: string | null
  /** The app can download and install this update itself. */
  installable: boolean
}

export interface UpdateProgress {
  phase: 'downloading' | 'verifying'
  received: number
  total: number
}

// evaluateUpdateJson (tnrp/Updates.h)
interface UpdateOffer {
  status: 'available' | 'none' | 'invalid'
  error: string
  version: string
  notesUrl: string
  publishedAt: string
  fileName: string
  url: string
  sha512: string
  size: number
}

const RELEASE_API_URL = 'https://api.github.com/repos/NoGoat/Track-N-Race/releases/latest'
export const RELEASE_PAGE_URL = 'https://github.com/NoGoat/Track-N-Race/releases/latest'
const ENABLED_KEY = 'updates.enabled'
const REMIND_LATER_KEY = 'updates.remindLaterAt'
const LAST_GITHUB_CHECK_KEY = 'updates.lastGitHubCheckAt'
const SKIPPED_VERSION_KEY = 'updates.skippedVersion'
// electron-builder's NSIS flags: silent, update mode, relaunch when done.
const INSTALLER_ARGS = ['/S', '--updated', '--force-run']

// The verified offer behind the dialog. The renderer only says "install"; the
// URL and hash never come from it.
let pendingOffer: UpdateOffer | null = null
// Where "Restart to update" reports the update (tnrp::updateInstallPath), on
// the same service that made the offer.
let pendingCountUrl = ''
let installing = false
// The downloaded, verified installer "Restart to update" runs.
let readyInstaller: string | null = null

function platformTarget(): { os: string; arch: string; format: string } {
  const arch = process.arch === 'arm64' ? 'arm64' : 'x64'
  if (process.platform === 'win32') return { os: 'windows', arch, format: 'installer' }
  if (process.platform === 'darwin') return { os: 'macos', arch, format: 'dmg' }
  return { os: 'linux', arch, format: 'appimage' }
}

// An NSIS install (not the portable build or a dev run): only these can run
// the new setup over themselves.
function isInstalledWindowsBuild(): boolean {
  if (process.platform !== 'win32' || !app.isPackaged || process.env.PORTABLE_EXECUTABLE_DIR) return false
  try {
    return fs.readdirSync(path.dirname(process.execPath)).some(name => /^Uninstall .+\.exe$/i.test(name))
  } catch {
    return false
  }
}

function mockedUpdate(currentVersion: string): AvailableUpdate {
  return {
    currentVersion,
    latestVersion: '999.0.0',
    releaseUrl: RELEASE_PAGE_URL,
    publishedAt: new Date().toISOString(),
    installable: false,
  }
}

function isSkipped(native: NativeUpdates, version: string): boolean {
  const skipped = store.get(SKIPPED_VERSION_KEY, '')
  return typeof skipped === 'string' && skipped !== '' && native.updateCompareVersions(skipped, version) === 0
}

// The update service: a signed manifest checked by the native library.
async function checkService(native: NativeUpdates, serviceUrl: string, current: string): Promise<UpdateOffer> {
  const { os, arch, format } = platformTarget()
  const url = serviceUrl + native.updateCheckPath('electron', os, arch, format, current)
  const response = await net.fetch(url, { headers: { 'User-Agent': 'Track-N-Race' }, cache: 'no-store' })
  if (response.status === 204) return { status: 'none' } as UpdateOffer
  if (!response.ok) throw new Error(`update service returned HTTP ${response.status}`)
  const offer = JSON.parse(native.updateEvaluateJson(await response.text(), 'electron', os, arch, format, current)) as UpdateOffer
  if (offer.status === 'invalid') throw new Error(offer.error || 'invalid update response')
  return offer
}

// Fallback while no update service is reachable: the GitHub release tag. It
// can only point at the download page.
async function checkGitHub(native: NativeUpdates, current: string): Promise<{ version: string; publishedAt: string | null } | null> {
  const response = await net.fetch(RELEASE_API_URL, {
    headers: {
      Accept: 'application/vnd.github+json',
      'User-Agent': 'Track-N-Race',
      'X-GitHub-Api-Version': '2022-11-28',
    },
  })
  if (!response.ok) throw new Error(`GitHub returned HTTP ${response.status}`)
  const body = await response.json() as Record<string, unknown>
  if (typeof body.tag_name !== 'string') throw new Error('latest release did not include a tag name')
  const order = native.updateCompareVersions(body.tag_name, current)
  if (order === null) throw new Error(`could not compare versions ${current} and ${body.tag_name}`)
  if (order <= 0) return null
  return {
    version: body.tag_name.trim().replace(/^v(?=\d)/i, ''),
    publishedAt: typeof body.published_at === 'string' ? body.published_at : null,
  }
}

function downloadDirectory(): string {
  return path.join(app.getPath('temp'), 'TrackNRace-update')
}

// The setup an earlier update ran. While that installer is still finishing
// (it relaunches the app before it exits) its file is locked; the next launch
// removes it instead.
function removeOldDownloads(): void {
  try {
    for (const name of fs.readdirSync(downloadDirectory())) {
      try { fs.rmSync(path.join(downloadDirectory(), name), { force: true }) } catch { /* still in use */ }
    }
  } catch { /* no folder */ }
}

export async function checkForUpdateOnStartup(currentVersion: string): Promise<AvailableUpdate | null> {
  removeOldDownloads()
  if (process.env.TRACK_N_RACE_UPDATE_TEST === '1') return mockedUpdate(currentVersion)
  if (!(store.get(ENABLED_KEY, true) as boolean)) return null
  const native = nativeUpdates()
  if (!native) return null

  // The update service is asked on every start, unless "Remind me later" is
  // still hiding offers (tnrp::updateQuietPeriodOver).
  const overridden = native.updateServiceOverridden()
  if (!overridden && !native.updateQuietPeriodOver(Date.now(), storedTime(REMIND_LATER_KEY))) return null

  const current = currentVersion.trim().replace(/^v(?=\d)/i, '')
  const serviceUrl = native.updateServiceUrl()
  if (serviceUrl) {
    try {
      const offer = await checkService(native, serviceUrl, current)
      if (offer.status !== 'available' && !offer.version) return null
      if (native.updateCompareVersions(offer.version, current) !== 1 || isSkipped(native, offer.version)) return null
      pendingOffer = offer.status === 'available' ? offer : null
      if (pendingOffer) {
        const { os, arch, format } = platformTarget()
        pendingCountUrl = serviceUrl + native.updateInstallPath('electron', os, arch, format, current, offer.version)
      }
      return {
        currentVersion: current,
        latestVersion: offer.version,
        releaseUrl: RELEASE_PAGE_URL,
        publishedAt: offer.publishedAt || null,
        installable: pendingOffer !== null && isInstalledWindowsBuild(),
      }
    } catch (error) {
      console.warn('[updates] update service check failed, falling back to GitHub:', error)
    }
  }

  // Once a day at most: GitHub allows 60 unauthenticated requests an hour per
  // IP. Recorded before the request so an outage can't retry every launch.
  const now = Date.now()
  if (!overridden && !native.updateQuietPeriodOver(now, storedTime(LAST_GITHUB_CHECK_KEY))) return null
  store.set(LAST_GITHUB_CHECK_KEY, now)
  try {
    const latest = await checkGitHub(native, current)
    if (!latest || isSkipped(native, latest.version)) return null
    return {
      currentVersion: current,
      latestVersion: latest.version,
      releaseUrl: RELEASE_PAGE_URL,
      publishedAt: latest.publishedAt,
      installable: false,
    }
  } catch (error) {
    console.warn('[updates] update check failed:', error)
    return null
  }
}

function storedTime(key: string): number {
  const value = store.get(key, 0)
  return typeof value === 'number' ? value : 0
}

/** Remind me later: no offers for a day. */
export function remindUpdateLater(): void {
  store.set(REMIND_LATER_KEY, Date.now())
}

export function skipUpdateVersion(version: string): void {
  const native = nativeUpdates()
  if (native && native.updateCompareVersions(version, version) === 0) {
    store.set(SKIPPED_VERSION_KEY, version.trim().replace(/^v(?=\d)/i, ''))
  }
}

// Tells the update service the user is restarting into the update, so it can
// count it. The app quits right after, so this waits for the reply, but never
// more than a few seconds, and a failure never stops the update.
async function countUpdate(url: string): Promise<void> {
  if (!url) return
  try {
    await net.fetch(url, { method: 'POST', headers: { 'User-Agent': 'Track-N-Race' }, cache: 'no-store', signal: AbortSignal.timeout(3_000) })
  } catch (error) {
    console.warn('[updates] could not count the update:', error)
  }
}

async function download(offer: UpdateOffer, destination: string, onProgress: (progress: UpdateProgress) => void): Promise<void> {
  const response = await net.fetch(offer.url, { headers: { 'User-Agent': 'Track-N-Race' }, cache: 'no-store' })
  if (!response.ok || !response.body) throw new Error(`download failed with HTTP ${response.status}`)

  const partial = `${destination}.part`
  const file = fs.createWriteStream(partial)
  const fileDone = new Promise<void>((resolve, reject) => {
    file.on('finish', () => resolve())
    file.on('error', reject)
  })
  let received = 0
  let lastReport = 0
  try {
    const reader = response.body.getReader()
    for (;;) {
      const { done, value } = await reader.read()
      if (done) break
      received += value.byteLength
      if (received > offer.size) throw new Error('the download is larger than the signed size')
      if (!file.write(value)) await new Promise<void>(resolve => file.once('drain', () => resolve()))
      const now = Date.now()
      if (now - lastReport > 100) {
        lastReport = now
        onProgress({ phase: 'downloading', received, total: offer.size })
      }
    }
    file.end()
    await fileDone
  } catch (error) {
    file.destroy()
    fs.rmSync(partial, { force: true })
    throw error
  }
  onProgress({ phase: 'downloading', received, total: offer.size })
  fs.rmSync(destination, { force: true })
  fs.renameSync(partial, destination)
}

// Starts the installer detached so it outlives this process. ShellExecute
// (through PowerShell's Start-Process) is the fallback when the setup asks
// for elevation, which a plain CreateProcess refuses.
function launchInstaller(installer: string): Promise<void> {
  const start = (command: string, args: string[]) => new Promise<void>((resolve, reject) => {
    const child = spawn(command, args, { detached: true, stdio: 'ignore', windowsHide: command !== installer })
    child.once('error', reject)
    child.once('spawn', () => {
      child.unref()
      resolve()
    })
  })
  return start(installer, INSTALLER_ARGS).catch(error => {
    console.warn('[updates] direct installer launch failed, retrying through the shell:', error)
    const quoted = (value: string) => `'${value.replace(/'/g, "''")}'`
    const command = `Start-Process -FilePath ${quoted(installer)} -ArgumentList ${INSTALLER_ARGS.map(quoted).join(',')}`
    return start('powershell.exe', ['-NoProfile', '-NonInteractive', '-Command', command])
  })
}

/**
 * Update now: downloads the pending update and verifies it against the signed
 * manifest. The installer then waits for "Restart to update"
 * ({@link restartToUpdate}). Resolves with "" when it's ready, otherwise with
 * what went wrong.
 */
export async function downloadPendingUpdate(onProgress: (progress: UpdateProgress) => void): Promise<string> {
  const offer = pendingOffer
  if (!offer || !isInstalledWindowsBuild()) return 'This update has to be installed from the download page.'
  if (readyInstaller) return ''
  const native = nativeUpdates()
  if (!native) return 'The update could not be verified: the native library is unavailable.'
  if (installing) return 'The update is already being downloaded.'
  installing = true

  const directory = downloadDirectory()
  const installer = path.join(directory, offer.fileName)
  try {
    fs.mkdirSync(directory, { recursive: true })
    await download(offer, installer, onProgress)

    onProgress({ phase: 'verifying', received: offer.size, total: offer.size })
    const mismatch = await native.updateVerifyFile(installer, offer.sha512, offer.size)
    if (mismatch) {
      fs.rmSync(installer, { force: true })
      throw new Error(mismatch)
    }
    readyInstaller = installer
    return ''
  } catch (error) {
    console.error('[updates] update failed:', error)
    return error instanceof Error ? error.message : String(error)
  } finally {
    installing = false
  }
}

/**
 * Restart to update: counts the update, starts the verified installer and
 * quits so it can replace the app; the installer relaunches it. Resolves with
 * "" on success, otherwise with what went wrong.
 */
export async function restartToUpdate(): Promise<string> {
  const installer = readyInstaller
  if (!installer) return 'The update has not been downloaded yet.'
  try {
    await countUpdate(pendingCountUrl)
    await launchInstaller(installer)
    console.log(`[updates] started the ${pendingOffer?.version ?? ''} installer; quitting`)
    setTimeout(() => app.quit(), 250)
    return ''
  } catch (error) {
    console.error('[updates] could not start the installer:', error)
    return error instanceof Error ? error.message : String(error)
  }
}
