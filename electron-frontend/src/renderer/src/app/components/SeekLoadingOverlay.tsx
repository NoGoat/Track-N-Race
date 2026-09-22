import { memo } from 'react'
import { useTelemetryStore } from '../../stores/telemetryStore'

// Mounted for the whole pending seek, but the CSS keeps it transparent for the
// first 100ms, so seeks that install quickly unmount before it is ever seen.
// It captures pointer input so the stale charts underneath cannot be hovered or
// clicked; the playback bar sits outside <main> and stays usable.
export default memo(function SeekLoadingOverlay() {
  const seekPending = useTelemetryStore(s => s.seekPending)
  if (!seekPending) return null
  return (
    <div className="seek-loading-overlay loading-scrim absolute inset-0 z-[90] flex flex-col items-center justify-center" role="status" aria-live="polite">
      <div className="loading-scrim-label mb-3 tracking-widest uppercase text-sm font-bold">Loading</div>
      <div className="loading-scrim-track w-48 h-1.5 rounded-full overflow-hidden relative">
        <div className="seek-loading-bar loading-scrim-fill absolute inset-y-0 w-1/2 rounded-full" />
      </div>
    </div>
  )
})
