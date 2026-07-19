import { memo } from 'react'
import type { BannerItem } from '../bannerHelpers'

export default memo(function FullscreenBanner({ banner, headerVisible, isFullscreen }: { banner: BannerItem | null; headerVisible: boolean; isFullscreen: boolean }) {
  if (!isFullscreen || headerVisible || !banner) return null
  return (
    <div className="absolute top-4 left-1/2 -translate-x-1/2 z-40 pointer-events-none">
      <div className="flex items-center gap-3 px-4 py-1.5 rounded-full border shadow-lg backdrop-blur-md animate-banner-in text-xs" style={{ background: 'rgba(10, 15, 30, 0.85)', backgroundImage: 'linear-gradient(rgba(255,255,255,0.02), rgba(255,255,255,0))', borderColor: `${banner.color}50`, boxShadow: `0 4px 20px -2px ${banner.color}15, 0 2px 8px -1px rgba(0,0,0,0.5)` }}>
        <div className="w-1.5 h-1.5 rounded-full shrink-0 animate-pulse" style={{ backgroundColor: banner.color }} />
        <span className="font-black uppercase tracking-[0.2em]" style={{ color: banner.color }}>{banner.label}</span>
        {banner.sub && <><span className="text-[var(--text-secondary)]">·</span><span className="text-[var(--text-secondary)] font-semibold">{banner.sub}</span></>}
      </div>
    </div>
  )
})
