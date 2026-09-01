import { memo } from 'react'
import type { BannerItem } from '../bannerHelpers'

export default memo(function FullscreenBanner({ banner, headerVisible, isFullscreen }: { banner: BannerItem | null; headerVisible: boolean; isFullscreen: boolean }) {
  if (!isFullscreen || headerVisible || !banner) return null
  return (
    <div className="pointer-events-none absolute left-1/2 top-3 z-40 max-w-[calc(100vw-24px)] -translate-x-1/2">
      <div
        key={`${banner.label}:${banner.sub ?? ''}`}
        role="status"
        aria-live="polite"
        className="fullscreen-event-banner flex h-9 min-w-0 items-stretch overflow-hidden rounded border border-[var(--border)] bg-[var(--bg-menu)]"
      >
        <div className="flex min-w-0 items-center px-3">
          <span className="shrink-0 whitespace-nowrap text-[10px] font-black uppercase tracking-[0.14em]" style={{ color: `color-mix(in srgb, ${banner.color} 72%, var(--text-primary))` }}>
            {banner.label}
          </span>
          {banner.sub && (
            <>
              <span className="mx-2.5 h-3 w-px shrink-0 bg-[var(--border)]" />
              <span className="min-w-0 truncate whitespace-nowrap text-[10px] font-semibold tabular-nums text-[var(--text-secondary)]">
                {banner.sub}
              </span>
            </>
          )}
        </div>
      </div>
    </div>
  )
})
