import { components } from 'react-select'
import { ChevronDown, X } from 'lucide-react'

function DropdownIndicator(props: any) {
  return (
    <components.DropdownIndicator {...props}>
      <ChevronDown size={12} />
    </components.DropdownIndicator>
  )
}

function ClearIndicator(props: any) {
  return (
    <components.ClearIndicator {...props}>
      <X size={12} />
    </components.ClearIndicator>
  )
}

export const selectComponents = { DropdownIndicator, ClearIndicator }

function SeparatorOption(props: any) {
  if (props.data?.isSeparator) {
    return (
      <div
        role="separator"
        className="mx-1 my-1.5 h-px bg-[var(--text-secondary)] opacity-60"
      />
    )
  }
  return <components.Option {...props} />
}

export const selectComponentsWithSeparator = { ...selectComponents, Option: SeparatorOption }
