/// <reference types="vite/types/importMeta.d.ts" />

// The bundled track_<id>.json maps the renderer draws. The native engine needs
// them too, to match a session's car positions against every map, so the main
// process ships its own lazily loaded copy: each file is a separate chunk read
// only when Modded Maps is first turned on.
// tsc checks the main process as CommonJS, where import.meta is not allowed;
// electron-vite replaces this glob with per-file chunks before bundling.
// @ts-expect-error TS1343
const builtinMapSources = import.meta.glob<string>('../renderer/src/assets/maps/track_*.json', {
  query: '?raw',
  import: 'default',
})

export function loadBuiltinTrackMaps(): Promise<string[]> {
  return Promise.all(Object.values(builtinMapSources).map(load => load()))
}
