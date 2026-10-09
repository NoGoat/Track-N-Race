import { useEffect, useState } from 'react'

export function useWindowState() {
  const [isMaximized, setIsMaximized] = useState(false)
  const [isFullscreen, setIsFullscreen] = useState(false)
  const [headerVisible, setHeaderVisible] = useState(false)

  useEffect(() => window.windowControls.onMaximizeChange(setIsMaximized), [])
  useEffect(() => window.windowControls.onFullscreenChange(setIsFullscreen), [])
  // The fullscreen-only header hides again when fullscreen ends.
  if (!isFullscreen && headerVisible) setHeaderVisible(false)

  return { headerVisible, isFullscreen, isMaximized, setHeaderVisible }
}
