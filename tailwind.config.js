/** @type {import('tailwindcss').Config} */
export default {
  content: ['./src/renderer/index.html', './src/renderer/src/**/*.{ts,tsx}'],
  theme: {
    extend: {
      // Bundled Cascadia Code is the app's monospace face. Force `font-mono`
      // to it so it doesn't fall back to a system font (e.g. Consolas on Windows).
      fontFamily: {
        mono: ["'Cascadia Code'", 'ui-monospace', 'monospace'],
      },
    },
  },
  plugins: [],
}
