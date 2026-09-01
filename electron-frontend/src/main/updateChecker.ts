import { net } from 'electron'
import { configStore as store } from './configStore'

export interface AvailableUpdate {
  currentVersion: string
  latestVersion: string
  releaseUrl: string
  publishedAt: string | null
}

const RELEASE_API_URL = 'https://api.github.com/repos/NoGoat/Track-N-Race/releases/latest'
export const RELEASE_PAGE_URL = 'https://github.com/NoGoat/Track-N-Race/releases/latest'
const CHECK_INTERVAL_MS = 24 * 60 * 60 * 1000
const ENABLED_KEY = 'updates.enabled'
const LAST_CHECK_KEY = 'updates.lastCheckAt'
const SKIPPED_VERSION_KEY = 'updates.skippedVersion'

type ParsedVersion = {
  core: [number, number, number]
  prerelease: Array<number | string>
}

function parseVersion(value: string): ParsedVersion | null {
  const match = value.trim().match(/^v?(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?$/)
  if (!match) return null

  return {
    core: [Number(match[1]), Number(match[2]), Number(match[3])],
    prerelease: match[4]
      ? match[4].split('.').map(part => /^\d+$/.test(part) ? Number(part) : part)
      : [],
  }
}

function compareVersions(leftValue: string, rightValue: string): number | null {
  const left = parseVersion(leftValue)
  const right = parseVersion(rightValue)
  if (!left || !right) return null

  for (let index = 0; index < left.core.length; index += 1) {
    if (left.core[index] !== right.core[index]) return left.core[index] > right.core[index] ? 1 : -1
  }

  if (left.prerelease.length === 0 && right.prerelease.length === 0) return 0
  if (left.prerelease.length === 0) return 1
  if (right.prerelease.length === 0) return -1

  const partCount = Math.max(left.prerelease.length, right.prerelease.length)
  for (let index = 0; index < partCount; index += 1) {
    const leftPart = left.prerelease[index]
    const rightPart = right.prerelease[index]
    if (leftPart === undefined) return -1
    if (rightPart === undefined) return 1
    if (leftPart === rightPart) continue
    if (typeof leftPart === 'number' && typeof rightPart === 'string') return -1
    if (typeof leftPart === 'string' && typeof rightPart === 'number') return 1
    return leftPart > rightPart ? 1 : -1
  }

  return 0
}

function normalizedVersion(value: string): string {
  return value.trim().replace(/^v(?=\d)/i, '')
}

function mockedUpdate(currentVersion: string): AvailableUpdate {
  return {
    currentVersion,
    latestVersion: '999.0.0',
    releaseUrl: RELEASE_PAGE_URL,
    publishedAt: new Date().toISOString(),
  }
}

export async function checkForUpdateOnStartup(currentVersion: string): Promise<AvailableUpdate | null> {
  if (process.env.TRACK_N_RACE_UPDATE_TEST === '1') return mockedUpdate(currentVersion)
  if (!(store.get(ENABLED_KEY, true) as boolean)) return null

  const now = Date.now()
  const lastCheckAt = store.get(LAST_CHECK_KEY, 0)
  if (typeof lastCheckAt === 'number' && now - lastCheckAt < CHECK_INTERVAL_MS) return null

  // Record the attempt before making the request so an outage cannot cause a
  // network request on every app launch during the next 24 hours.
  store.set(LAST_CHECK_KEY, now)

  try {
    const response = await net.fetch(RELEASE_API_URL, {
      headers: {
        Accept: 'application/vnd.github+json',
        'User-Agent': 'Track-N-Race',
        'X-GitHub-Api-Version': '2022-11-28',
      },
    })
    if (!response.ok) {
      console.warn(`[updates] GitHub returned HTTP ${response.status}`)
      return null
    }

    const body = await response.json() as Record<string, unknown>
    if (typeof body.tag_name !== 'string') {
      console.warn('[updates] latest release did not include a tag name')
      return null
    }

    const latestVersion = normalizedVersion(body.tag_name)
    const current = normalizedVersion(currentVersion)
    const comparison = compareVersions(latestVersion, current)
    if (comparison === null) {
      console.warn(`[updates] could not compare versions ${current} and ${latestVersion}`)
      return null
    }
    if (comparison <= 0) return null

    const skippedVersion = store.get(SKIPPED_VERSION_KEY, '')
    if (typeof skippedVersion === 'string' && normalizedVersion(skippedVersion) === latestVersion) return null

    return {
      currentVersion: current,
      latestVersion,
      releaseUrl: RELEASE_PAGE_URL,
      publishedAt: typeof body.published_at === 'string' ? body.published_at : null,
    }
  } catch (error) {
    console.warn('[updates] update check failed:', error)
    return null
  }
}

export function skipUpdateVersion(version: string): void {
  if (parseVersion(version)) store.set(SKIPPED_VERSION_KEY, normalizedVersion(version))
}
