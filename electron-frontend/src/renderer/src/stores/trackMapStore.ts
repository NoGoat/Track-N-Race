import { useEffect } from 'react'
import { create } from 'zustand'
import { TRACK_MAPS, type TrackMapData } from '../lib/trackMaps'
import type { TrackMapCatalog, TrackMapResolution, TrackMapSettings } from '../types'

// Modded maps: the settings, the saved maps and which map the live session or
// loaded recording uses. libtnrp decides; this store mirrors its answers.

interface TrackMapStoreState {
  settings: TrackMapSettings
  catalog: TrackMapCatalog
  // The live session's or loaded recording's "track_map" row.
  session: TrackMapResolution | null
  // Saved maps fetched so far, by ID.
  saved: Record<string, TrackMapData>
}

export const useTrackMapStore = create<TrackMapStoreState>(() => ({
  settings: { modded: false, generate: false },
  catalog: { maps: [], pins: {} },
  session: null,
  saved: {},
}))

const pendingLoads = new Set<string>()
let initialized = false

function isResolution(value: unknown): value is TrackMapResolution {
  return !!value && typeof value === 'object' && (value as { type?: unknown }).type === 'track_map'
}

export async function refreshTrackMapCatalog(): Promise<void> {
  const value = await window.trackMapsBridge.catalog() as Partial<TrackMapCatalog> | null
  const catalog: TrackMapCatalog = {
    maps: Array.isArray(value?.maps) ? value.maps : [],
    pins: value?.pins && typeof value.pins === 'object' ? value.pins : {},
  }
  // A map renamed or replaced on disk is fetched again.
  useTrackMapStore.setState({ catalog, saved: {} })
}

function handleRow(row: Record<string, unknown>): void {
  const type = row.type
  if (type === 'track_map' && isResolution(row) && row.scope !== 'recording') {
    useTrackMapStore.setState({ session: row })
  } else if (type === 'track_maps_changed') {
    void refreshTrackMapCatalog()
  } else if (type === 'playback_close') {
    const session = useTrackMapStore.getState().session
    if (session?.scope === 'playback') useTrackMapStore.setState({ session: null })
  }
}

function initTrackMapStore(): void {
  if (initialized) return
  initialized = true
  void window.trackMapsBridge.getSettings().then(settings => useTrackMapStore.setState({ settings }))
  window.trackMapsBridge.onSettings(settings => {
    useTrackMapStore.setState(settings.modded ? { settings } : { settings, session: null })
    if (settings.modded) void window.trackMapsBridge.state().then(state => {
      if (isResolution(state)) useTrackMapStore.setState({ session: state })
    })
  })
  void refreshTrackMapCatalog()
  void window.trackMapsBridge.state().then(state => {
    if (isResolution(state)) useTrackMapStore.setState({ session: state })
  })
  window.telemetryBridge.onBatch((batch: string) => {
    if (!batch.includes('"type":"track_map') && !batch.includes('"type":"playback_close"')) return
    for (const line of batch.split('\n')) {
      if (!line.includes('"type":"track_map') && !line.includes('"type":"playback_close"')) continue
      try { handleRow(JSON.parse(line)) } catch { /* malformed row: skip it */ }
    }
  })
  window.telemetryBridge.on(raw => {
    if (raw && typeof raw === 'object') handleRow(raw as Record<string, unknown>)
  })
}

function loadSaved(id: string): void {
  if (pendingLoads.has(id)) return
  pendingLoads.add(id)
  void window.trackMapsBridge.get(id).then(value => {
    pendingLoads.delete(id)
    if (value && typeof value === 'object')
      useTrackMapStore.setState(state => ({ saved: { ...state.saved, [id]: value as TrackMapData } }))
  })
}

/** Starts listening for map rows; call from anything that shows a map. */
export function useTrackMapStoreInit(): void {
  useEffect(() => { initTrackMapStore() }, [])
}

/** The map to draw for a track: the session's choice with Modded Maps on,
 *  otherwise (or until a choice arrives) the bundled map for its ID. */
export function useResolvedTrackMap(
  trackId: number | null,
  resolution: TrackMapResolution | null,
): TrackMapData | null {
  useTrackMapStoreInit()
  const modded = useTrackMapStore(state => state.settings.modded)
  const mapId = modded && resolution && resolution.track_id === trackId ? resolution.map_id : null
  const saved = useTrackMapStore(state => mapId ? state.saved[mapId] ?? null : null)
  const builtinId = mapId?.startsWith('builtin:') ? Number(mapId.slice('builtin:'.length)) : null
  const needsLoad = !!mapId && builtinId === null && !saved
  useEffect(() => {
    if (needsLoad && mapId) loadSaved(mapId)
  }, [mapId, needsLoad])

  if (trackId === null) return null
  // No choice, or none yet (the bundled maps reach the engine just after it
  // starts): the bundled map for this ID.
  if (!mapId) return TRACK_MAPS[trackId] ?? null
  if (builtinId !== null) return TRACK_MAPS[builtinId] ?? null
  return saved
}
