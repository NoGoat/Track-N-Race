// ESLint config for electron-frontend, used by .github/workflows/eslint.yml.
// Run from electron-frontend with --config pointing here; paths are relative to it.
import js from '@eslint/js'
import tseslint from 'typescript-eslint'
import reactHooks from 'eslint-plugin-react-hooks'
import globals from 'globals'

export default tseslint.config(
  {
    ignores: ['node_modules/**', 'out/**', 'dist/**', 'build/**', 'node_addon/**']
  },
  js.configs.recommended,
  ...tseslint.configs.recommended,
  {
    files: ['src/main/**/*.{js,ts}', 'src/preload/**/*.{js,ts}', 'scripts/**/*.{js,mjs}', '*.{js,mjs,ts}'],
    languageOptions: { globals: globals.node }
  },
  {
    files: ['src/renderer/**/*.{js,jsx,ts,tsx}'],
    languageOptions: { globals: globals.browser },
    plugins: { 'react-hooks': reactHooks },
    rules: reactHooks.configs.recommended.rules
  }
)
