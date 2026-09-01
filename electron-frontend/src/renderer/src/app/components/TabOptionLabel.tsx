import {
  CalendarDays,
  ChartNoAxesCombined,
  GamepadDirectional,
  LayoutDashboard,
  LifeBuoy,
  Lightbulb,
  ListOrdered,
  Shapes,
  Zap,
  type LucideIcon,
} from 'lucide-react'
import type { Tab } from '../appConfig'

interface TabOption {
  value: Tab
  label: string
}

const TAB_ICONS: Record<Tab, LucideIcon> = {
  core: LayoutDashboard,
  analyze: ChartNoAxesCombined,
  session: CalendarDays,
  strategy: Lightbulb,
  timing_tower: ListOrdered,
  input: GamepadDirectional,
  power: Zap,
  tyres: LifeBuoy,
  misc: Shapes,
}

export function formatTabOptionLabel(option: TabOption) {
  const Icon = TAB_ICONS[option.value]
  return (
    <span className="flex min-w-0 items-center gap-2">
      <Icon aria-hidden="true" className="shrink-0" size={13} strokeWidth={1.8} />
      <span className="truncate">{option.label}</span>
    </span>
  )
}
