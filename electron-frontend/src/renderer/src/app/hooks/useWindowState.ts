import { useEffect, useState } from 'react'

export function useWindowState() {
  const [isMaximized, setIsMaximized] = useState(false)
  const [isFullscreen, setIsFullscreen] = useState(false)
  const [headerVisible, setHeaderVisible] = useState(false)

  useEffect(() => window.windowControls.onMaximizeChange(setIsMaximized), [])
  useEffect(() => window.windowControls.onFullscreenChange(setIsFullscreen), [])
  useEffect(() => { if (!isFullscreen) setHeaderVisible(false) }, [isFullscreen])

  return { headerVisible, isFullscreen, isMaximized, setHeaderVisible }
}
