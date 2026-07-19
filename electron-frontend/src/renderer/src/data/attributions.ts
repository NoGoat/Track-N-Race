// Third-party attributions for the Attribution page in Settings.
// License texts are imported verbatim via Vite's `?raw` so they are inlined at
// build time (no runtime fs access, survives packaging). All texts live under
// src/renderer/src/assets/ so the Electron app is self-contained.

import reactLicense from '../assets/licenses/react.txt?raw'
import reactDomLicense from '../assets/licenses/react-dom.txt?raw'
import electronLicense from '../assets/licenses/electron.txt?raw'
import electronStoreLicense from '../assets/licenses/electron-store.txt?raw'
import lucideReactLicense from '../assets/licenses/lucide-react.txt?raw'
import reactSelectLicense from '../assets/licenses/react-select.txt?raw'
import uiwReactColorLicense from '../assets/licenses/uiw-react-color.txt?raw'
import timechartLicense from '../assets/licenses/timechart.txt?raw'
import d3AxisLicense from '../assets/licenses/d3-axis.txt?raw'
import d3ColorLicense from '../assets/licenses/d3-color.txt?raw'
import d3ScaleLicense from '../assets/licenses/d3-scale.txt?raw'
import d3SelectionLicense from '../assets/licenses/d3-selection.txt?raw'
import glMatrixLicense from '../assets/licenses/gl-matrix.txt?raw'
import tslibLicense from '../assets/licenses/tslib.txt?raw'
import cascadiaLicense from '../assets/licenses/cascadia-code.txt?raw'
import zlibLicense from '../assets/licenses/zlib.txt?raw'
import zstandardLicense from '../assets/licenses/zstandard.txt?raw'
import glazeLicense from '../assets/licenses/glaze.txt?raw'
import nodeAddonApiLicense from '../assets/licenses/node-addon-api.txt?raw'

export type AttributionCategory = 'app' | 'font' | 'node-addon'

export interface Attribution {
  name: string
  version: string
  license: string
  homepage: string
  licenseText: string
  category: AttributionCategory
  badge?: string
}

export const ATTRIBUTIONS: Attribution[] = [
  // Application libraries
  {
    name: 'Electron',
    version: '42.2.0',
    license: 'MIT',
    homepage: 'https://electronjs.org',
    licenseText: electronLicense,
    category: 'app',
  },
  {
    name: 'React',
    version: '18.3.1',
    license: 'MIT',
    homepage: 'https://react.dev',
    licenseText: reactLicense,
    category: 'app',
  },
  {
    name: 'React DOM',
    version: '18.3.1',
    license: 'MIT',
    homepage: 'https://react.dev',
    licenseText: reactDomLicense,
    category: 'app',
  },
  {
    name: 'electron-store',
    version: '8.2.0',
    license: 'MIT',
    homepage: 'https://github.com/sindresorhus/electron-store',
    licenseText: electronStoreLicense,
    category: 'app',
  },
  {
    name: 'lucide-react',
    version: '1.16.0',
    license: 'ISC',
    homepage: 'https://lucide.dev',
    licenseText: lucideReactLicense,
    category: 'app',
  },
  {
    name: 'react-select',
    version: '5.10.2',
    license: 'MIT',
    homepage: 'https://react-select.com',
    licenseText: reactSelectLicense,
    category: 'app',
  },
  {
    name: '@uiw/react-color',
    version: '2.10.3',
    license: 'MIT',
    homepage: 'https://github.com/uiwjs/react-color',
    licenseText: uiwReactColorLicense,
    category: 'app',
  },
  {
    name: 'TimeChart',
    version: '1.0.0-beta.10',
    license: 'MIT',
    homepage: 'https://github.com/huww98/TimeChart',
    licenseText: timechartLicense,
    category: 'app',
    badge: 'Forked',
  },
  {
    name: 'd3-axis',
    version: '3.0.0',
    license: 'ISC',
    homepage: 'https://d3js.org/d3-axis/',
    licenseText: d3AxisLicense,
    category: 'app',
  },
  {
    name: 'd3-color',
    version: '3.1.0',
    license: 'ISC',
    homepage: 'https://d3js.org/d3-color/',
    licenseText: d3ColorLicense,
    category: 'app',
  },
  {
    name: 'd3-scale',
    version: '4.0.2',
    license: 'ISC',
    homepage: 'https://d3js.org/d3-scale/',
    licenseText: d3ScaleLicense,
    category: 'app',
  },
  {
    name: 'd3-selection',
    version: '3.0.0',
    license: 'ISC',
    homepage: 'https://d3js.org/d3-selection/',
    licenseText: d3SelectionLicense,
    category: 'app',
  },
  {
    name: 'gl-matrix',
    version: '3.4.4',
    license: 'MIT',
    homepage: 'https://glmatrix.net/',
    licenseText: glMatrixLicense,
    category: 'app',
  },
  {
    name: 'tslib',
    version: '2.8.1',
    license: '0BSD',
    homepage: 'https://github.com/microsoft/tslib',
    licenseText: tslibLicense,
    category: 'app',
  },

  // Font
  {
    name: 'Cascadia Code',
    version: '',
    license: 'OFL-1.1',
    homepage: 'https://github.com/microsoft/cascadia-code',
    licenseText: cascadiaLicense,
    category: 'font',
  },

  // Node addon native libraries
  {
    name: 'zlib',
    version: '',
    license: 'Zlib',
    homepage: 'https://zlib.net',
    licenseText: zlibLicense,
    category: 'node-addon',
  },
  {
    name: 'Zstandard',
    version: '1.5.7',
    license: 'BSD-3-Clause',
    homepage: 'https://facebook.github.io/zstd/',
    licenseText: zstandardLicense,
    category: 'node-addon',
  },
  {
    name: 'Glaze',
    version: '',
    license: 'MIT',
    homepage: 'https://github.com/stephenberry/glaze',
    licenseText: glazeLicense,
    category: 'node-addon',
  },
  {
    name: 'node-addon-api',
    version: '',
    license: 'MIT',
    homepage: 'https://github.com/nodejs/node-addon-api',
    licenseText: nodeAddonApiLicense,
    category: 'node-addon',
  },
]

export const ATTRIBUTION_SECTIONS: { category: AttributionCategory; label: string }[] = [
  { category: 'app', label: 'Application Libraries' },
  { category: 'font', label: 'Font' },
  { category: 'node-addon', label: 'Node Addon' },
]
