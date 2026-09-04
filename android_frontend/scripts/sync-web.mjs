import { cp, mkdir, readFile, rm, writeFile } from 'node:fs/promises'
import { dirname, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const projectRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const webBuild = resolve(projectRoot, 'build', 'web')
const assetsRoot = resolve(projectRoot, 'app', 'src', 'main', 'assets')
const publicRoot = resolve(assetsRoot, 'public')
const apacheLicense = resolve(projectRoot, 'app', 'src', 'main', 'res', 'raw', 'apache_2_0.txt')
const fontLicense = resolve(projectRoot, '..', 'electron-frontend', 'src', 'renderer', 'src', 'assets', 'fonts', 'OFL.txt')
const lucideLicense = resolve(projectRoot, 'node_modules', 'lucide-react', 'LICENSE')
const toastifyLicense = resolve(projectRoot, 'node_modules', 'react-toastify', 'LICENSE')
const clsxLicense = resolve(projectRoot, 'node_modules', 'clsx', 'license')

await rm(publicRoot, { recursive: true, force: true })
await mkdir(publicRoot, { recursive: true })
await cp(webBuild, publicRoot, { recursive: true })
await mkdir(resolve(publicRoot, 'licenses'), { recursive: true })
await cp(apacheLicense, resolve(publicRoot, 'licenses', 'apache-2.0.txt'))
await cp(fontLicense, resolve(publicRoot, 'licenses', 'cascadia-code.txt'))
await cp(lucideLicense, resolve(publicRoot, 'licenses', 'lucide-react.txt'))
await cp(toastifyLicense, resolve(publicRoot, 'licenses', 'react-toastify.txt'))
await cp(clsxLicense, resolve(publicRoot, 'licenses', 'clsx.txt'))

const config = JSON.parse(await readFile(resolve(projectRoot, 'capacitor.config.json'), 'utf8'))
delete config.webDir
await writeFile(resolve(assetsRoot, 'capacitor.config.json'), `${JSON.stringify(config, null, 2)}\n`)
await writeFile(resolve(assetsRoot, 'capacitor.plugins.json'), '[]\n')
