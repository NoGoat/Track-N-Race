import { resolve } from 'path'
import { defineConfig } from 'electron-vite'
import react from '@vitejs/plugin-react-swc'
import { readFileSync } from 'fs'

const pkg = JSON.parse(readFileSync(resolve(__dirname, 'package.json'), 'utf-8'))
const includePerformanceDiagnostics = process.env.INCLUDE_PERFORMANCE_DIAGNOSTICS === '1'
const appVersion = process.env.RC_VERSION || pkg.version

export default defineConfig({
  main: {
    define: {
      __APP_VERSION__: JSON.stringify(appVersion)
    },
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
      __APP_VERSION__: JSON.stringify(appVersion),
      __ENABLE_PERFORMANCE_DIAGNOSTICS__: JSON.stringify(includePerformanceDiagnostics)
    }
  }
})
