import { useEffect, useLayoutEffect, useRef, useState, type CSSProperties } from 'react'
import { createPortal } from 'react-dom'
import { Chrome, ChromeInputType } from '@uiw/react-color'
import { ArrowDownUp } from 'lucide-react'

// Shared color picker used by Analysis and Settings. Keeping one component
// preserves identical positioning, theme integration, keyboard dismissal, and
// hex-only behavior everywhere colors are edited.
export default function ColorPicker({
  label, color, onChange, triggerClassName, triggerStyle, disabled = false,
}: {
  label: string
  color: string
  onChange: (color: string) => void
  triggerClassName?: string
  triggerStyle?: CSSProperties
  disabled?: boolean
}) {
  const [open, setOpen] = useState(false)
  const [position, setPosition] = useState({ left: 8, top: 8 })
  const [formatIconHost, setFormatIconHost] = useState<HTMLElement | null>(null)
  const buttonRef = useRef<HTMLButtonElement>(null)
  const pickerRef = useRef<HTMLDivElement>(null)

  useEffect(() => {
    if (disabled) setOpen(false)
  }, [disabled])

  useLayoutEffect(() => {
    if (!open) return
    const place = () => {
      const rect = buttonRef.current?.getBoundingClientRect()
      if (!rect) return
      const pickerWidth = 230
      const pickerHeight = 260
      const left = Math.max(8, Math.min(rect.left, window.innerWidth - pickerWidth - 8))
      const below = rect.bottom + 6
      const top = below + pickerHeight <= window.innerHeight
        ? below
        : Math.max(8, rect.top - pickerHeight - 6)
      setPosition({ left, top })
    }
    place()
    window.addEventListener('resize', place)
    return () => window.removeEventListener('resize', place)
  }, [open])

  useLayoutEffect(() => {
    if (!open) {
      setFormatIconHost(null)
      return
    }
    const nativeIcon = pickerRef.current?.querySelector<SVGElement>('svg[viewBox="0 0 1024 1024"]')
    setFormatIconHost(nativeIcon?.parentElement ?? null)
  }, [open])

  useEffect(() => {
    if (!open) return
    const closeOutside = (event: PointerEvent) => {
      const target = event.target as Node
      if (!buttonRef.current?.contains(target) && !pickerRef.current?.contains(target)) setOpen(false)
    }
    const closeOnEscape = (event: KeyboardEvent) => {
      if (event.key === 'Escape') setOpen(false)
    }
    document.addEventListener('pointerdown', closeOutside)
    document.addEventListener('keydown', closeOnEscape)
    return () => {
      document.removeEventListener('pointerdown', closeOutside)
      document.removeEventListener('keydown', closeOnEscape)
    }
  }, [open])

  const chromeStyle = {
    '--github-background-color': 'var(--bg-menu)',
    '--github-border': '1px solid var(--border)',
    '--github-box-shadow': '0 14px 36px rgba(0, 0, 0, 0.38)',
    '--github-arrow-border-color': 'var(--border)',
    '--editable-input-label-color': 'var(--text-secondary)',
    '--editable-input-box-shadow': 'var(--border) 0 0 0 1px inset',
    '--editable-input-color': 'var(--text-primary)',
    '--chrome-arrow-fill': 'var(--text-secondary)',
    '--chrome-arrow-background-color': 'var(--bg-hover)',
    width: 230,
    borderRadius: 6,
    fontFamily: '"Cascadia Code", ui-monospace, monospace',
  } as CSSProperties

  return <>
    <button
      ref={buttonRef}
      type="button"
      disabled={disabled}
      draggable={false}
      aria-label={`${label} color`}
      aria-haspopup="dialog"
      aria-expanded={open}
      onClick={() => setOpen(value => !value)}
      className={triggerClassName ?? 'w-5 h-5 rounded border border-[var(--border)] cursor-pointer shrink-0 shadow-inner'}
      style={{ backgroundColor: color, ...triggerStyle }}
    />
    {open && !disabled && createPortal(
      <div ref={pickerRef} role="dialog" aria-label={`${label} color picker`} className="fixed z-[10000]" style={position}>
        <Chrome
          className="analyze-color-picker"
          color={color}
          inputType={ChromeInputType.HEXA}
          showAlpha={false}
          showTriangle={false}
          style={chromeStyle}
          onChange={result => onChange(result.hex)}
        />
        {formatIconHost?.isConnected && createPortal(
          <span className="w-8 h-8 flex items-center justify-center pointer-events-none text-[var(--text-secondary)]">
            <ArrowDownUp size={16} strokeWidth={1.75} className="analyze-color-format-icon" />
          </span>,
          formatIconHost,
        )}
      </div>,
      document.body,
    )}
  </>
}
