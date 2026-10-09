import { components, type ClearIndicatorProps, type DropdownIndicatorProps, type GroupBase } from 'react-select'
import { ChevronDown, X } from 'lucide-react'

function DropdownIndicator<Option, IsMulti extends boolean, Group extends GroupBase<Option>>(
  props: DropdownIndicatorProps<Option, IsMulti, Group>,
) {
  return (
    <components.DropdownIndicator {...props}>
      <ChevronDown size={12} />
    </components.DropdownIndicator>
  )
}

function ClearIndicator<Option, IsMulti extends boolean, Group extends GroupBase<Option>>(
  props: ClearIndicatorProps<Option, IsMulti, Group>,
) {
  return (
    <components.ClearIndicator {...props}>
      <X size={12} />
    </components.ClearIndicator>
  )
}

export const selectComponents = { DropdownIndicator, ClearIndicator }
