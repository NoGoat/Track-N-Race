const LIGHT_SERIES_COLORS: Record<string, string> = {
  '#5794F2': '#0B57D0',
  '#2196F3': '#0B57D0',
  '#0EA5E9': '#0B57D0',
  '#FADE2A': '#765900',
  '#FFD700': '#765900',
  '#FDD835': '#765900',
  '#F0A500': '#A04300',
  '#FF9830': '#A04300',
  '#FB923C': '#A04300',
  '#37872D': '#0D6B2F',
  '#73BF69': '#0D6B2F',
  '#00C853': '#0D6B2F',
  '#BF5FFF': '#7C3BA6',
  '#B877DB': '#7C3BA6',
  '#9F7AEA': '#7C3BA6',
  '#A0A8B8': '#56606B',
  '#8E8E8E': '#56606B',
}

/** Preserve the vivid Dark Mode palette while increasing contrast on the
 * muted Light Mode chart surface. Non-palette/user colours pass through. */
export function themeSeriesColor(color: string, isDark: boolean): string {
  return isDark ? color : LIGHT_SERIES_COLORS[color.toUpperCase()] ?? color
}
