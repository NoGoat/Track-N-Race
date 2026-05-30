import { Resvg } from '@resvg/resvg-js'
import { readFileSync, writeFileSync } from 'fs'
import { fileURLToPath } from 'url'
import { dirname, join } from 'path'
import pngToIco from 'png-to-ico'
import { PNG } from 'pngjs'

const __dirname = dirname(fileURLToPath(import.meta.url))
const root = join(__dirname, '..')

const svg = readFileSync(join(root, 'build', 'icon.svg'), 'utf-8')

function renderAtSize(size) {
  const resvg = new Resvg(svg, { fitTo: { mode: 'width', value: size } })
  return resvg.render().asPng()
}

const sizes = [256, 48, 32, 16]
const pngs = sizes.map(renderAtSize)

writeFileSync(join(root, 'build', 'icon.png'), pngs[0])
console.log('Wrote build/icon.png (256px preview)')

const ico = await pngToIco(pngs)
writeFileSync(join(root, 'build', 'icon.ico'), ico)
console.log('Wrote build/icon.ico')

// Pad a PNG to square with transparent background so png-to-ico can handle it
function padToSquare(pngPath) {
  const src = PNG.sync.read(readFileSync(pngPath))
  const dim = Math.max(src.width, src.height)
  const dst = new PNG({ width: dim, height: dim, filterType: -1 })
  dst.data.fill(0) // transparent
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

for (const variant of ['icon_transparent', 'icon_transparent_light']) {
  const squarePng = padToSquare(join(root, `${variant}.png`))
  const variantIco = await pngToIco(squarePng)
  writeFileSync(join(root, 'build', `${variant}.ico`), variantIco)
  console.log(`Wrote build/${variant}.ico`)
}
