import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react-swc'
import { fileURLToPath } from 'node:url'
import { readFileSync } from 'node:fs'
const pkg = JSON.parse(readFileSync(new URL('./package.json', import.meta.url), 'utf8'))
export default defineConfig({
  root: 'src/renderer',
  plugins: [react()],
  resolve: { alias: { '@renderer': fileURLToPath(new URL('./src/renderer/src', import.meta.url)) } },
  define: { __APP_VERSION__: JSON.stringify(pkg.version) },
  server: { port: 1420, strictPort: true, fs: { allow: ['../..'] } },
  build: { outDir: '../../dist', emptyOutDir: true, target: ['es2022', 'safari15'] },
  clearScreen: false,
})
