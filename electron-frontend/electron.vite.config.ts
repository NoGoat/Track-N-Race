import { resolve } from 'path'
import { defineConfig } from 'electron-vite'
import react from '@vitejs/plugin-react-swc'
import { readFileSync } from 'fs'

const pkg = JSON.parse(readFileSync(resolve(__dirname, 'package.json'), 'utf-8'))
const includeReactScan = process.env.INCLUDE_REACT_SCAN === '1'

export default defineConfig({
  main: {
    // electron-store is ESM-only, so bundle it into the CommonJS main process.
    build: {
      externalizeDeps: { exclude: ['electron-store'] }
    }
  },
  preload: {},
  renderer: {
    resolve: {
      alias: {
        '@renderer': resolve('src/renderer/src')
      }
    },
    plugins: [react()],
    define: {
      __APP_VERSION__: JSON.stringify(process.env.RC_VERSION || pkg.version),
      __ENABLE_REACT_SCAN__: JSON.stringify(includeReactScan)
    }
  }
})
