import { readFileSync, writeFileSync, mkdirSync } from 'fs'
import { fileURLToPath } from 'url'
import { dirname, join } from 'path'
import pngToIco from 'png-to-ico'
import { PNG } from 'pngjs'

const __dirname = dirname(fileURLToPath(import.meta.url))
const root = join(__dirname, '..')

function padToSquare(pngPath) {
  const src = PNG.sync.read(readFileSync(pngPath))
  const dim = Math.max(src.width, src.height)
  const dst = new PNG({ width: dim, height: dim, filterType: -1 })
  dst.data.fill(0)
  const ox = Math.floor((dim - src.width) / 2)
  const oy = Math.floor((dim - src.height) / 2)
  for (let y = 0; y < src.height; y++) {
    for (let x = 0; x < src.width; x++) {
      const si = (y * src.width + x) * 4
      const di = ((oy + y) * dim + (ox + x)) * 4
      dst.data[di]     = src.data[si]
      dst.data[di + 1] = src.data[si + 1]
      dst.data[di + 2] = src.data[si + 2]
      dst.data[di + 3] = src.data[si + 3]
    }
  }
  return PNG.sync.write(dst)
}

mkdirSync(join(root, 'build'), { recursive: true })

const variants = [
  { src: 'icon_solid.png',             dst: 'icon.ico',               preview: 'icon.png' },
  { src: 'icon_transparent.png',       dst: 'icon_transparent.ico',   preview: 'icon_transparent.png' },
  { src: 'icon_transparent_light.png', dst: 'icon_transparent_light.ico', preview: 'icon_transparent_light.png' },
]

for (const { src, dst, preview } of variants) {
  const squarePng = padToSquare(join(root, src))
  if (preview) {
    writeFileSync(join(root, 'build', preview), squarePng)
    console.log(`Wrote build/${preview}`)
  }
  const ico = await pngToIco(squarePng)
  writeFileSync(join(root, 'build', dst), ico)
  console.log(`Wrote build/${dst}`)
}
