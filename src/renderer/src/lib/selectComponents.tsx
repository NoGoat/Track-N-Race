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
