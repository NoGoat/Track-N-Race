// Stands in for api.github.com + release downloads for the update e2e test.
// node mock-github.mjs <release-dir> [port]
import { createServer } from 'node:http'
import { createReadStream, readdirSync, statSync, existsSync } from 'node:fs'
import { join, basename } from 'node:path'

const dir = process.argv[2]
const port = Number(process.argv[3] || 8788)
const base = `http://127.0.0.1:${port}`

function assets() {
  return readdirSync(dir).filter(name => statSync(join(dir, name)).isFile()).map(name => ({
    name,
    size: statSync(join(dir, name)).size,
    browser_download_url: `${base}/download/${encodeURIComponent(name)}`,
  }))
}

createServer((req, res) => {
  const url = new URL(req.url, base)
  console.log(new Date().toISOString(), req.method, url.pathname + url.search)
  if (url.pathname === '/repos/NoGoat/Track-N-Race/releases') {
    const body = [
      // A draft must never be offered, even though it is the highest version.
      { tag_name: '9.9.9', draft: true, prerelease: false, assets: assets() },
      { tag_name: '0.9.2', draft: false, prerelease: false, assets: assets() },
      { tag_name: '0.9.1', draft: false, prerelease: false, assets: [] },
    ]
    res.writeHead(200, { 'Content-Type': 'application/json' })
    res.end(JSON.stringify(body))
    return
  }
  if (url.pathname.startsWith('/download/')) {
    const name = basename(decodeURIComponent(url.pathname.slice('/download/'.length)))
    const path = join(dir, name)
    if (!existsSync(path)) { res.writeHead(404); res.end(); return }
    res.writeHead(200, { 'Content-Type': 'application/octet-stream', 'Content-Length': statSync(path).size })
    createReadStream(path).pipe(res)
    return
  }
  res.writeHead(404)
  res.end()
}).listen(port, '127.0.0.1', () => console.log(`mock github on ${base} serving ${dir}`))
