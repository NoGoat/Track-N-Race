import syncedTooltipIcon from '../../assets/icons/synced-tooltip.svg'

interface Props {
  className?: string
  size?: number
}

export default function SyncedTooltipIcon({ className = '', size = 16 }: Props) {
  const mask = `url("${syncedTooltipIcon}")`

  return <span
    aria-hidden="true"
    className={`block shrink-0 ${className}`}
    style={{
      width: size,
      height: size,
      backgroundColor: 'currentColor',
      maskImage: mask,
      maskPosition: 'center',
      maskRepeat: 'no-repeat',
      maskSize: 'contain',
      WebkitMaskImage: mask,
      WebkitMaskPosition: 'center',
      WebkitMaskRepeat: 'no-repeat',
      WebkitMaskSize: 'contain',
    }}
  />
}
