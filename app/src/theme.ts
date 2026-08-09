// Design tokens for the Ticker Board companion app. Dark, terminal-flavored palette —
// monochrome surfaces with a single accent and the universal up/down market colors.

export const colors = {
  bg: '#0B0E11', // app background — near-black
  surface: '#15191E', // cards / sheets
  surfaceAlt: '#1C2127', // pressed / nested rows
  border: 'rgba(255,255,255,0.08)', // hairline dividers / input borders
  borderStrong: 'rgba(255,255,255,0.16)',

  white: '#FFFFFF',
  ink: '#0B0E11', // dark text on light buttons
  text: '#E7E9EA', // primary text
  textDim: 'rgba(231,233,234,0.62)', // secondary text
  textFaint: 'rgba(231,233,234,0.32)', // placeholders / disabled

  accent: '#5AC8FA', // brand accent (links, selection, progress)
  accentDim: 'rgba(90,200,250,0.16)',

  up: '#26C281', // gains
  upBg: 'rgba(38,194,129,0.14)',
  down: '#F6465D', // losses
  downBg: 'rgba(246,70,93,0.14)',

  warn: '#F0B90B', // econ / attention

  green: '#26C281', // status: connected / live dot
  greenBg: 'rgba(38,194,129,0.14)',
  red: '#F6465D', // status: error
  redBg: 'rgba(246,70,93,0.14)',
} as const

export const radius = {
  sm: 8,
  md: 12,
  lg: 20,
  pill: 999,
} as const

export const space = {
  xs: 4,
  sm: 8,
  md: 12,
  lg: 16,
  xl: 24,
} as const

// Standard phone content gutter.
export const layout = {
  gutter: 16,
} as const

// Monospace for prices/symbols (uses the platform mono so no font asset is required); the
// system sans for everything else.
export const fonts = {
  mono: 'monospace',
  monoIos: 'Menlo',
} as const
