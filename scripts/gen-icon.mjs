import { Resvg } from '@resvg/resvg-js'
import { readFileSync, writeFileSync } from 'fs'
import { fileURLToPath } from 'url'
import { dirname, join } from 'path'
import pngToIco from 'png-to-ico'

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
