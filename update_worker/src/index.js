// Track N Race update service.
//
// GET /v1/check?product=qt&os=windows&arch=x64&format=installer&v=1.6.2
//   200 {"manifest": "<manifest.json text>", "signature": "<base64>"} for the
//       newest published release that is newer than v and has a file for this
//       install. The apps verify the signature themselves (tnrp/Updates.h), so
//       this service only finds the right manifest and caches GitHub.
//   204 when there is nothing newer.
//
// Downloads are never proxied: the manifest points straight at GitHub.
// Nothing about the caller is logged or stored.
//
// Settings (wrangler.toml [vars] or the dashboard):
//   BLOCKED_VERSIONS  comma-separated versions to stop offering, e.g. "1.7.0"
//   GITHUB_TOKEN      optional secret; raises GitHub's API rate limit
//   GITHUB_API        repository API base (local testing only)

const DEFAULT_API = 'https://api.github.com/repos/NoGoat/Track-N-Race'
const RELEASES_TTL = 600            // seconds; how quickly a new release shows up
const ASSET_TTL = 600               // a manifest is re-signed when another machine adds its files
const TOKEN = /^[a-z0-9_]{1,24}$/
const VERSION = /^v?(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?$/

export default {
  async fetch(request, env, ctx) {
    if (request.method !== 'GET' && request.method !== 'HEAD') return new Response(null, { status: 405 })
    const url = new URL(request.url)
    if (url.pathname === '/v1/check') {
      try {
        return await check(url, env, ctx)
      } catch (error) {
        return json({ error: String(error?.message || error) }, 502)
      }
    }
    if (url.pathname === '/') return new Response('Track N Race update service\n', { headers: { 'Content-Type': 'text/plain' } })
    return new Response(null, { status: 404 })
  },
}

async function check(url, env, ctx) {
  const want = {}
  for (const name of ['product', 'os', 'arch', 'format']) {
    const value = (url.searchParams.get(name) || '').toLowerCase()
    if (!TOKEN.test(value)) return json({ error: `missing or invalid ${name}` }, 400)
    want[name] = value
  }
  const current = url.searchParams.get('v') || ''
  if (!parseVersion(current)) return json({ error: 'missing or invalid v' }, 400)

  const api = (env.GITHUB_API || DEFAULT_API).replace(/\/+$/, '')
  const blocked = new Set((env.BLOCKED_VERSIONS || '').split(',').map(value => value.trim().replace(/^v/, '')).filter(Boolean))
  const releases = await cachedFetch(`${api}/releases?per_page=30`, RELEASES_TTL, env, ctx, 'json')

  const candidates = releases
    .filter(release => !release.draft && !release.prerelease && parseVersion(release.tag_name))
    .filter(release => !blocked.has(release.tag_name.replace(/^v/, '')))
    .filter(release => compareVersions(release.tag_name, current) > 0)
    .sort((a, b) => compareVersions(b.tag_name, a.tag_name))

  for (const release of candidates) {
    const manifestAsset = release.assets?.find(asset => asset.name === 'manifest.json')
    const signatureAsset = release.assets?.find(asset => asset.name === 'manifest.json.sig')
    if (!manifestAsset || !signatureAsset) continue

    const manifest = await cachedFetch(manifestAsset.browser_download_url, ASSET_TTL, env, ctx, 'text')
    const signature = (await cachedFetch(signatureAsset.browser_download_url, ASSET_TTL, env, ctx, 'text')).trim()
    let parsed
    try {
      parsed = JSON.parse(manifest)
    } catch {
      continue
    }
    const hasFile = Array.isArray(parsed.files) && parsed.files.some(file =>
      file.product === want.product && file.os === want.os && file.arch === want.arch && file.format === want.format)
    if (hasFile) return json({ manifest, signature }, 200)
  }
  return new Response(null, { status: 204, headers: { 'Cache-Control': 'no-store' } })
}

async function cachedFetch(target, ttl, env, ctx, kind) {
  const cache = caches.default
  const key = new Request(`https://cache.tracknrace.internal/${kind}?u=${encodeURIComponent(target)}`)
  const hit = await cache.match(key)
  if (hit) return kind === 'json' ? hit.json() : hit.text()

  const headers = { 'User-Agent': 'Track-N-Race-Update-Service' }
  if (kind === 'json') {
    headers.Accept = 'application/vnd.github+json'
    headers['X-GitHub-Api-Version'] = '2022-11-28'
  }
  if (env.GITHUB_TOKEN && new URL(target).hostname === 'api.github.com') headers.Authorization = `Bearer ${env.GITHUB_TOKEN}`
  const response = await fetch(target, { headers, redirect: 'follow' })
  if (!response.ok) throw new Error(`GitHub returned HTTP ${response.status} for ${new URL(target).pathname}`)
  const body = await response.text()
  ctx.waitUntil(cache.put(key, new Response(body, { headers: { 'Cache-Control': `public, max-age=${ttl}` } })))
  return kind === 'json' ? JSON.parse(body) : body
}

function json(value, status) {
  return new Response(JSON.stringify(value), {
    status,
    headers: { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' },
  })
}

function parseVersion(value) {
  const match = String(value).trim().match(VERSION)
  if (!match) return null
  return {
    core: [Number(match[1]), Number(match[2]), Number(match[3])],
    prerelease: match[4] ? match[4].split('.').map(part => (/^\d+$/.test(part) ? Number(part) : part)) : [],
  }
}

function compareVersions(leftValue, rightValue) {
  const left = parseVersion(leftValue)
  const right = parseVersion(rightValue)
  for (let index = 0; index < 3; index += 1) {
    if (left.core[index] !== right.core[index]) return left.core[index] > right.core[index] ? 1 : -1
  }
  // A release outranks its own prereleases.
  if (!left.prerelease.length && !right.prerelease.length) return 0
  if (!left.prerelease.length) return 1
  if (!right.prerelease.length) return -1
  const count = Math.max(left.prerelease.length, right.prerelease.length)
  for (let index = 0; index < count; index += 1) {
    const a = left.prerelease[index]
    const b = right.prerelease[index]
    if (a === undefined) return -1
    if (b === undefined) return 1
    if (a === b) continue
    if (typeof a !== typeof b) return typeof a === 'number' ? -1 : 1
    return a > b ? 1 : -1
  }
  return 0
}
