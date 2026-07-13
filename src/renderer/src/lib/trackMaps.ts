export interface MapTransform {
  min_x: number
  min_z: number
  scale: number
  off_x: number
  off_z: number
}

export interface TrackMapData {
  track_id:       number
  track_name:     string
  circuit_name:   string
  track_length_m: number
  view_box:       { width: number; height: number }
  rotation_deg:   number
  transform:      MapTransform
  sectors:        Array<{ index: number; points: [number, number][] }>
  drs_zones:      Array<{
    start:        [number, number]
    end:          [number, number]
  }>
  // Straight Line Mode (2026) zones — dry- and wet-weather variants. Same
  // {start,end} shape as DRS; the polyline is re-derived from the centerline.
  slm_dry?:       Array<{ start: [number, number]; end: [number, number] }>
  slm_wet?:       Array<{ start: [number, number]; end: [number, number] }>
  speed_traps:    [number, number][]
  start_finish:   [number, number] | null
}

// Vite expands this glob at dev-server startup/build time, so newly generated
// track_*.json files are bundled without needing a matching import or registry entry.
const trackMapModules = import.meta.glob<TrackMapData>('../assets/maps/track_*.json', {
  eager: true,
  import: 'default'
})

export const TRACK_MAPS: Record<number, TrackMapData> = Object.fromEntries(
  Object.values(trackMapModules).map((trackMap) => [trackMap.track_id, trackMap])
)
