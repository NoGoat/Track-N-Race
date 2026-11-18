#!/usr/bin/env node
// Signs and publishes the self-update manifest for a release.
//
//   node publish-release.mjs keygen
//       Creates the release signing key (once) and prints its public key,
//       which goes in protocol_parser_library/src/Updates.cpp.
//
//   node publish-release.mjs manifest <version> [--dir <dir>] [--base-url <url>]
//       Writes manifest.json and manifest.json.sig into the release folder
//       (default release/<version>), describing every file in it.
//       --base-url replaces the download URL base (default
//       https://downloads.track-n-race.com/<version>).
//
//   node publish-release.mjs publish <version> [--dir <dir>] [--no-github] [--local]
//       Uploads this machine's files to the R2 bucket under <version>/, merges
//       in the manifest entries other machines already uploaded, re-signs, and
//       uploads the manifest last, so it never names a file that isn't there.
//       Then mirrors the files to the GitHub release <version> (a draft until
//       you publish it), unless --no-github. Nothing is offered to installed
//       apps until the version is promoted.
//
//   node publish-release.mjs promote <version> <staging|stable> [--local]
//       Points channels/<channel>.json at <version>. The staging Worker offers
//       the staging channel, production offers stable. Promoting an older
//       version to stable is how a bad release is rolled back.
//
// R2 access goes through wrangler (update_worker/), so `npx wrangler login`
// once on each machine is the only setup. --local uses wrangler's local bucket,
// the one `wrangler dev` serves, for testing.
//
// The private key is read from TNR_UPDATE_SIGNING_KEY (a PEM file path) or
// ~/.track-n-race/update-signing-key.pem. Keep a backup: without it, installed
// apps can never accept another update.

import { createHash, createPrivateKey, createPublicKey, generateKeyPairSync, sign } from 'node:crypto'
import { execFileSync } from 'node:child_process'
import { existsSync, mkdirSync, mkdtempSync, readdirSync, readFileSync, rmSync, statSync, writeFileSync } from 'node:fs'
import { homedir, tmpdir } from 'node:os'
import { basename, dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const REPO = 'NoGoat/Track-N-Race'
const ROOT = dirname(fileURLToPath(import.meta.url))
const KEY_PATH = process.env.TNR_UPDATE_SIGNING_KEY || join(homedir(), '.track-n-race', 'update-signing-key.pem')
const MANIFEST = 'manifest.json'
const SIGNATURE = 'manifest.json.sig'
const WORKER_DIR = join(ROOT, 'update_worker')
const BUCKET = process.env.TNR_R2_BUCKET || 'track-n-race-releases'
const DOWNLOAD_BASE = 'https://downloads.track-n-race.com'
// wrangler dev serves the local bucket's files here (LOCAL_DOWNLOADS in update_worker/src/index.js).
const LOCAL_DOWNLOAD_BASE = 'http://127.0.0.1:8790/files'
const CHANNELS = ['staging', 'stable']
const CONTENT_TYPES = {
  '.apk': 'application/vnd.android.package-archive',
  '.exe': 'application/vnd.microsoft.portable-executable',
  '.zip': 'application/zip',
  '.dmg': 'application/x-apple-diskimage',
  '.AppImage': 'application/octet-stream',
  '.json': 'application/json',
  '.sig': 'text/plain',
}

function fail(message) {
  console.error(`Error: ${message}`)
  process.exit(1)
}

function publicKeyBase64(privateKey) {
  // The raw Ed25519 key is the last 32 bytes of its SPKI encoding.
  const spki = createPublicKey(privateKey).export({ type: 'spki', format: 'der' })
  return spki.subarray(spki.length - 32).toString('base64')
}

function loadKey() {
  if (!existsSync(KEY_PATH)) fail(`no signing key at ${KEY_PATH}. Run "node publish-release.mjs keygen" first.`)
  return createPrivateKey(readFileSync(KEY_PATH))
}

function keygen() {
  if (existsSync(KEY_PATH)) {
    console.log(`A signing key already exists at ${KEY_PATH}.`)
  } else {
    const { privateKey } = generateKeyPairSync('ed25519')
    mkdirSync(dirname(KEY_PATH), { recursive: true })
    writeFileSync(KEY_PATH, privateKey.export({ type: 'pkcs8', format: 'pem' }), { mode: 0o600, flag: 'wx' })
    console.log(`Created ${KEY_PATH}. Back it up somewhere safe.`)
  }
  console.log(`Public key: ${publicKeyBase64(loadKey())}`)
}

// Track-N-Race-<Product>-<OS>[-<arch>].<ext>
function describe(name) {
  // build-signed-apk names the single universal APK without an OS segment.
  if (name === 'Track-N-Race-Android.apk') return { product: 'android', os: 'android', arch: 'universal', format: 'apk' }
  const match = name.match(/^Track-N-Race-([A-Za-z]+)-([A-Za-z]+)(?:-([A-Za-z0-9_]+))?\.(exe|zip|AppImage|dmg|apk)$/)
  if (!match) return null
  const [, product, os, arch, extension] = match
  const format = { exe: 'installer', zip: 'zip', AppImage: 'appimage', dmg: 'dmg', apk: 'apk' }[extension]
  const osName = os.toLowerCase() === 'macos' ? 'macos' : os.toLowerCase()
  return { product: product.toLowerCase(), os: osName, arch: (arch || 'x64').toLowerCase(), format }
}

function sha512(path) {
  const hash = createHash('sha512')
  hash.update(readFileSync(path))
  return hash.digest('hex')
}

function compareVersions(left, right) {
  const parse = value => value.replace(/^v/, '').split(/[-+]/)[0].split('.').map(Number)
  const a = parse(left)
  const b = parse(right)
  for (let index = 0; index < 3; index += 1) {
    if (a[index] !== b[index]) return a[index] > b[index] ? 1 : -1
  }
  return 0
}

function writeManifest(version, dir, baseUrl, previous) {
  if (!/^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$/.test(version)) fail(`"${version}" is not a version (x.y.z).`)
  if (!existsSync(dir)) fail(`release folder ${dir} does not exist.`)
  const key = loadKey()
  const urlBase = (baseUrl || `${DOWNLOAD_BASE}/${version}`).replace(/\/+$/, '')

  const local = []
  for (const name of readdirSync(dir).sort()) {
    const path = join(dir, name)
    if (!statSync(path).isFile()) continue
    const target = describe(name)
    if (!target) continue
    local.push({ ...target, name, url: `${urlBase}/${encodeURIComponent(name)}`, size: statSync(path).size, sha512: sha512(path) })
  }
  if (local.length === 0) fail(`no release files (Track-N-Race-*) in ${dir}.`)

  if (previous && previous.version !== version) fail(`the uploaded manifest is for ${previous.version}, not ${version}.`)
  const localNames = new Set(local.map(file => file.name))
  const kept = (previous?.files ?? []).filter(file => !localNames.has(file.name))
  const files = [...kept, ...local].sort((a, b) => a.name.localeCompare(b.name))

  const manifest = {
    schema: 1,
    version,
    publishedAt: new Date().toISOString(),
    notesUrl: `https://github.com/${REPO}/releases/tag/${version}`,
    files,
  }
  const text = `${JSON.stringify(manifest, null, 2)}\n`
  const signature = sign(null, Buffer.from(text, 'utf8'), key).toString('base64')
  writeFileSync(join(dir, MANIFEST), text)
  writeFileSync(join(dir, SIGNATURE), `${signature}\n`)
  console.log(`Signed ${join(dir, MANIFEST)} (${files.length} files, ${kept.length} kept from the uploaded manifest).`)
  return [...local.map(file => join(dir, file.name)), join(dir, MANIFEST), join(dir, SIGNATURE)]
}

function gh(args, options = {}) {
  return execFileSync('gh', args, { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'], ...options })
}

// wrangler, run from update_worker/ so it uses that folder's install and login.
function wrangler(args, options = {}) {
  return execFileSync('npx', ['wrangler', ...args], {
    cwd: WORKER_DIR, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'], ...options,
  })
}

function r2Put(key, path, local) {
  const extension = Object.keys(CONTENT_TYPES).find(ext => path.endsWith(ext))
  const args = ['r2', 'object', 'put', `${BUCKET}/${key}`, '--file', path, local ? '--local' : '--remote']
  if (extension) args.push('--content-type', CONTENT_TYPES[extension])
  // Files are per version; a re-upload while preparing a release must not stay cached for long.
  if (!key.startsWith('channels/')) args.push('--cache-control', 'public, max-age=3600')
  wrangler(args, { stdio: ['ignore', 'ignore', 'inherit'] })
}

// The object's text, or null when it doesn't exist.
function r2Get(key, local) {
  const scratch = mkdtempSync(join(tmpdir(), 'tnr-r2-'))
  const file = join(scratch, 'object')
  try {
    wrangler(['r2', 'object', 'get', `${BUCKET}/${key}`, '--file', file, local ? '--local' : '--remote'])
    return existsSync(file) ? readFileSync(file, 'utf8') : null
  } catch {
    return null
  } finally {
    rmSync(scratch, { recursive: true, force: true })
  }
}

function publishGitHub(version, uploads) {
  let exists = true
  try {
    gh(['release', 'view', version, '--repo', REPO, '--json', 'tagName'])
  } catch {
    exists = false
  }
  if (!exists) {
    const latest = (() => {
      try { return JSON.parse(gh(['release', 'view', '--repo', REPO, '--json', 'tagName'])).tagName } catch { return null }
    })()
    if (latest && compareVersions(version, latest) <= 0) fail(`${version} is not newer than the latest release ${latest}.`)
    gh(['release', 'create', version, '--repo', REPO, '--draft', '--title', version, '--notes', ''])
    console.log(`Created draft GitHub release ${version}.`)
  }
  gh(['release', 'upload', version, '--repo', REPO, '--clobber', ...uploads], { stdio: 'inherit' })
  console.log(`Mirrored ${uploads.map(path => basename(path)).join(', ')} to GitHub.`)
}

function publish(version, dir, { github, local }) {
  const previousText = r2Get(`${version}/${MANIFEST}`, local)
  const previous = previousText ? JSON.parse(previousText) : null
  const uploads = writeManifest(version, dir, local ? `${LOCAL_DOWNLOAD_BASE}/${version}` : null, previous)

  // Files first, the signed manifest last.
  for (const path of uploads) {
    console.log(`Uploading ${basename(path)} to ${BUCKET}/${version}/...`)
    r2Put(`${version}/${basename(path)}`, path, local)
  }
  console.log(`Uploaded ${uploads.length} files to ${local ? 'the local' : 'the R2'} bucket.`)
  if (github && !local) publishGitHub(version, uploads)
  console.log(`When every platform is uploaded: node publish-release.mjs promote ${version} staging${local ? ' --local' : ''}`)
}

function promote(version, channel, { local }) {
  if (!CHANNELS.includes(channel)) fail(`channel must be one of ${CHANNELS.join(', ')}.`)
  const manifestText = r2Get(`${version}/${MANIFEST}`, local)
  if (!manifestText) fail(`${version}/${MANIFEST} is not in the bucket. Publish it first.`)
  const manifest = JSON.parse(manifestText)
  if (manifest.version !== version) fail(`${version}/${MANIFEST} is for ${manifest.version}.`)

  const current = r2Get(`channels/${channel}.json`, local)
  const scratch = mkdtempSync(join(tmpdir(), 'tnr-channel-'))
  try {
    const file = join(scratch, `${channel}.json`)
    writeFileSync(file, `${JSON.stringify({ version, promotedAt: new Date().toISOString() })}\n`)
    r2Put(`channels/${channel}.json`, file, local)
  } finally {
    rmSync(scratch, { recursive: true, force: true })
  }
  const was = current ? JSON.parse(current).version : 'nothing'
  console.log(`The ${channel} channel now offers ${version} (was ${was}). Installed apps see it within 10 minutes.`)
  console.log(`Files in ${version}: ${manifest.files.map(file => file.name).join(', ')}`)
  if (channel === 'stable' && !local) {
    console.log(`Also: gh release edit ${version} --repo ${REPO} --draft=false --prerelease=false, and bump VERSION on the website.`)
  }
}

function option(args, name) {
  const index = args.indexOf(name)
  return index >= 0 ? args[index + 1] : undefined
}

const [command, version, ...rest] = process.argv.slice(2)
const local = rest.includes('--local')
const dir = resolve(option(rest, '--dir') || join(ROOT, 'release', version || ''))
switch (command) {
  case 'keygen':
    keygen()
    break
  case 'manifest':
    if (!version) fail('usage: node publish-release.mjs manifest <version> [--dir <dir>] [--base-url <url>]')
    writeManifest(version, dir, option(rest, '--base-url'), null)
    break
  case 'publish':
    if (!version) fail('usage: node publish-release.mjs publish <version> [--dir <dir>] [--no-github] [--local]')
    publish(version, dir, { github: !rest.includes('--no-github'), local })
    break
  case 'promote':
    if (!version || !rest[0]) fail('usage: node publish-release.mjs promote <version> <staging|stable> [--local]')
    promote(version, rest[0], { local })
    break
  default:
    fail('usage: node publish-release.mjs keygen | manifest <version> | publish <version> | promote <version> <channel>')
}
