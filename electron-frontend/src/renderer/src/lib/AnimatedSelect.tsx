import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import Select, { type GroupBase, type Props, type StylesConfig } from 'react-select'
import { SELECT_MENU_ANIMATION_MS } from './selectStyles'

type MenuPhase = 'closed' | 'open' | 'closing'

function shouldReduceAnimations(): boolean {
  return document.documentElement.dataset.reduceAnimations === 'true'
    || window.matchMedia('(prefers-reduced-motion: reduce)').matches
}

/** Keeps react-select's menu mounted just long enough to play its exit animation. */
export default function AnimatedSelect<
  Option,
  IsMulti extends boolean = false,
  Group extends GroupBase<Option> = GroupBase<Option>,
>(props: Props<Option, IsMulti, Group>) {
  const {
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
  const closeTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null)

  const cancelClose = useCallback(() => {
    if (closeTimerRef.current !== null) {
      clearTimeout(closeTimerRef.current)
      closeTimerRef.current = null
    }
  }, [])

  const openMenu = useCallback(() => {
    cancelClose()
    setPhase('open')
    onMenuOpen?.()
  }, [cancelClose, onMenuOpen])

  const closeMenu = useCallback(() => {
    if (phase !== 'open') return
    onMenuClose?.()
    if (shouldReduceAnimations()) {
      setPhase('closed')
      return
    }
    setPhase('closing')
    closeTimerRef.current = setTimeout(() => {
      closeTimerRef.current = null
      setPhase('closed')
    }, SELECT_MENU_ANIMATION_MS)
  }, [onMenuClose, phase])

  useEffect(() => cancelClose, [cancelClose])

  useEffect(() => {
    if (controlledMenuIsOpen === true && phase !== 'open') openMenu()
    if (controlledMenuIsOpen === false && phase === 'open') closeMenu()
  }, [closeMenu, controlledMenuIsOpen, openMenu, phase])

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

  return (
    <Select<Option, IsMulti, Group>
      {...selectProps}
      defaultMenuIsOpen={defaultMenuIsOpen}
      menuIsOpen={phase !== 'closed'}
      onMenuOpen={openMenu}
      onMenuClose={closeMenu}
      styles={animatedStyles}
    />
  )
}
