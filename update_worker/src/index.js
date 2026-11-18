// Track N Race update service.
//
// GET /v1/check?product=qt&os=windows&arch=x64&format=installer&v=1.6.2
//   200 {"manifest": "<manifest.json text>", "signature": "<base64>"} when this
//       Worker's channel offers a release newer than v with a file for this
//       install. The apps verify the signature themselves (tnrp/Updates.h), so
//       this service only finds the right manifest.
//   204 when there is nothing newer.
//
// Releases live in the R2 bucket bound as RELEASES (publish-release.mjs writes them):
//   <version>/<file>                 the release files, served from downloads.track-n-race.com
//   <version>/manifest.json(.sig)    the signed manifest
//   channels/<channel>.json          {"version": "1.7.0"}: what that channel offers
// Downloads are never proxied in production: the manifest points at the bucket's
// own domain. Nothing about the caller is logged or stored.
//
// Settings (wrangler.toml [vars] or the dashboard):
//   CHANNEL           "stable" (production) or "staging"
//   BLOCKED_VERSIONS  comma-separated versions to stop offering, e.g. "1.7.0"
//   LOCAL_DOWNLOADS   "true" serves /files/<key> from the bucket (local testing only)

const POINTER_TTL = 600             // seconds; how quickly a promoted release shows up
const MANIFEST_TTL = 600            // a manifest is re-signed when another machine adds its files
const TOKEN = /^[a-z0-9_]{1,24}$/
const VERSION = /^v?(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?$/
const CHANNEL = /^[a-z]{1,16}$/

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
    if (env.LOCAL_DOWNLOADS === 'true' && url.pathname.startsWith('/files/')) return download(url, env)
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

  const channel = env.CHANNEL || 'stable'
  if (!CHANNEL.test(channel)) throw new Error('CHANNEL is not a channel name')
  const pointer = await cachedObject(url, `channels/${channel}.json`, POINTER_TTL, env, ctx)
  if (pointer === null) return nothing()
  const version = String(JSON.parse(pointer).version || '').replace(/^v/, '')
  if (!parseVersion(version)) throw new Error(`channels/${channel}.json has no valid version`)

  const blocked = new Set((env.BLOCKED_VERSIONS || '').split(',').map(value => value.trim().replace(/^v/, '')).filter(Boolean))
  if (blocked.has(version) || compareVersions(version, current) <= 0) return nothing()

  const manifest = await cachedObject(url, `${version}/manifest.json`, MANIFEST_TTL, env, ctx)
  const signature = await cachedObject(url, `${version}/manifest.json.sig`, MANIFEST_TTL, env, ctx)
  if (manifest === null || signature === null) throw new Error(`release ${version} has no signed manifest`)
  const parsed = JSON.parse(manifest)
  const hasFile = Array.isArray(parsed.files) && parsed.files.some(file =>
    file.product === want.product && file.os === want.os && file.arch === want.arch && file.format === want.format)
  return hasFile ? json({ manifest, signature: signature.trim() }, 200) : nothing()
}

// Local testing only: the release files, as downloads.track-n-race.com serves them in production.
async function download(url, env) {
  const key = decodeURIComponent(url.pathname.slice('/files/'.length))
  if (!key || key.includes('..') || key.startsWith('channels/')) return new Response(null, { status: 404 })
  const object = await env.RELEASES.get(key)
  if (!object) return new Response(null, { status: 404 })
  return new Response(object.body, {
    headers: { 'Content-Type': 'application/octet-stream', 'Content-Length': String(object.size) },
  })
}

// Most checks find nothing newer after reading only the cached pointer, which
// keeps R2 reads far under the free tier's 10 million a month.
// The Cache API works only on a zone (api.track-n-race.com); on workers.dev
// (staging) every call reads R2. Keys use the Worker's own hostname so they
// stay inside its zone.
async function cachedObject(requestUrl, key, ttl, env, ctx) {
  const cache = caches.default
  const cacheKey = new Request(`${requestUrl.origin}/__cache/${encodeURIComponent(key)}`)
  const hit = await cache.match(cacheKey)
  if (hit) return hit.status === 404 ? null : hit.text()

  const object = await env.RELEASES.get(key)
  const body = object ? await object.text() : null
  const cached = body === null
    ? new Response('', { status: 404, headers: { 'Cache-Control': `public, max-age=${ttl}` } })
    : new Response(body, { headers: { 'Cache-Control': `public, max-age=${ttl}` } })
  ctx.waitUntil(cache.put(cacheKey, cached))
  return body
}

function nothing() {
  return new Response(null, { status: 204, headers: { 'Cache-Control': 'no-store' } })
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
