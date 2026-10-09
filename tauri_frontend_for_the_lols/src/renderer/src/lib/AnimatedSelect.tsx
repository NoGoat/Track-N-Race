import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import Select, { type ClassNamesConfig, type GroupBase, type Props, type SelectInstance, type StylesConfig } from 'react-select'
import { SELECT_MENU_ANIMATION_MS } from './selectStyles'

type MenuPhase = 'closed' | 'open' | 'closing'

function shouldReduceAnimations(): boolean {
  return document.documentElement.dataset.reduceAnimations === 'true'
}

/** Keeps react-select's menu mounted just long enough to play its exit animation. Single-select only. */
export default function AnimatedSelect<
  Option,
  IsMulti extends false = false,
  Group extends GroupBase<Option> = GroupBase<Option>,
>(props: Props<Option, IsMulti, Group>) {
  const {
    className,
    classNames,
    defaultMenuIsOpen = false,
    menuIsOpen: controlledMenuIsOpen,
    onMenuClose,
    onMenuOpen,
    styles,
    ...selectProps
  } = props
  const [phase, setPhase] = useState<MenuPhase>(
    defaultMenuIsOpen || controlledMenuIsOpen ? 'open' : 'closed',
  )
  const selectRef = useRef<SelectInstance<Option, IsMulti, Group> | null>(null)

  const openMenu = useCallback(() => {
    setPhase('open')
    onMenuOpen?.()
  }, [onMenuOpen])

  const closeMenu = useCallback(() => {
    if (phase !== 'open') return
    // The menu remains mounted for its exit animation, so a pending focus scroll
    // would otherwise snap the list back to the selected option before it closes.
    if (selectRef.current) selectRef.current.scrollToFocusedOptionOnUpdate = false
    onMenuClose?.()
    setPhase(shouldReduceAnimations() ? 'closed' : 'closing')
  }, [onMenuClose, phase])

  // Ends the exit animation; reopening or unmounting first cancels it.
  useEffect(() => {
    if (phase !== 'closing') return
    const timer = setTimeout(() => setPhase('closed'), SELECT_MENU_ANIMATION_MS)
    return () => clearTimeout(timer)
  }, [phase])

  // A controlled menuIsOpen drives the phase directly.
  if (controlledMenuIsOpen === true && phase !== 'open') setPhase('open')
  if (controlledMenuIsOpen === false && phase === 'open') setPhase(shouldReduceAnimations() ? 'closed' : 'closing')

  const animatedStyles = useMemo<StylesConfig<Option, IsMulti, Group>>(() => ({
    ...styles,
    menu: (base, state) => {
      const menuStyle = styles?.menu ? styles.menu(base, state) : base
      if (phase !== 'closing') return menuStyle
      return {
        ...menuStyle,
        animation: `${state.placement === 'top' ? 'selectMenuExitTop' : 'selectMenuExitBottom'} ${SELECT_MENU_ANIMATION_MS}ms cubic-bezier(0.2, 0, 0, 1) both`,
        pointerEvents: 'none',
        willChange: 'clip-path',
      }
    },
  }), [phase, styles])

  const noDragClassNames = useMemo<ClassNamesConfig<Option, IsMulti, Group>>(() => ({
    ...classNames,
    menuPortal: state => [classNames?.menuPortal?.(state), 'react-select-no-drag'].filter(Boolean).join(' '),
  }), [classNames])

  return (
    <Select<Option, IsMulti, Group>
      ref={selectRef}
      {...selectProps}
      className={[className, 'react-select-no-drag'].filter(Boolean).join(' ')}
      classNames={noDragClassNames}
      defaultMenuIsOpen={defaultMenuIsOpen}
      menuIsOpen={phase !== 'closed'}
      onMenuOpen={openMenu}
      onMenuClose={closeMenu}
      styles={animatedStyles}
    />
  )
}
