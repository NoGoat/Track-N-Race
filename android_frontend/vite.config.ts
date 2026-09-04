import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react-swc'

export default defineConfig({
  plugins: [react()],
  base: './',
  build: {
    outDir: 'build/web',
    emptyOutDir: true,
    target: 'chrome111'
  }
})
