// Button bitmask constants from F1 25 UDP spec
const BTN_A          = 0x00000001  // Cross / A
const BTN_Y          = 0x00000002  // Triangle / Y
const BTN_B          = 0x00000004  // Circle / B
const BTN_X          = 0x00000008  // Square / X
const BTN_DPAD_LEFT  = 0x00000010
const BTN_DPAD_RIGHT = 0x00000020
const BTN_DPAD_UP    = 0x00000040
const BTN_DPAD_DOWN  = 0x00000080
const BTN_MENU       = 0x00000100  // Options / Menu
const BTN_LB         = 0x00000200
const BTN_RB         = 0x00000400
const BTN_LT         = 0x00000800
const BTN_RT         = 0x00001000
const BTN_LS         = 0x00002000  // Left stick click
const BTN_RS         = 0x00004000  // Right stick click
const BTN_VIEW       = 0x00080000  // Special / View

interface Props {
  buttonStatus: number
}

function pressed(buttonStatus: number, mask: number) {
  return (buttonStatus & mask) !== 0
}

export default function ControllerVisualizer({ buttonStatus }: Props) {
  const p = (mask: number) => pressed(buttonStatus, mask)

  // Trigger fill heights (0 = released, displayed as partial fill when pressed)
  const lt = p(BTN_LT)
  const rt = p(BTN_RT)

  return (
    <div className="bg-[var(--bg-panel)] border border-[var(--border)] rounded p-4 h-full flex flex-col">
      <div className="flex items-center justify-between mb-3 shrink-0">
        <h2 className="text-[11px] text-[var(--text-secondary)] uppercase tracking-widest">Controller</h2>
        <span className="text-[11px] text-[var(--text-secondary)]">Xbox One</span>
      </div>
      <div className="flex-1 min-h-0 flex items-center justify-center">
        <svg
          viewBox="0 0 440 280"
          className="w-full h-full"
          style={{ maxHeight: '100%', maxWidth: '100%' }}
        >
          {/* ── Body ── */}
          {/* Left grip */}
          <ellipse cx="108" cy="218" rx="68" ry="52" fill="var(--bg-accent)" />
          {/* Right grip */}
          <ellipse cx="332" cy="218" rx="68" ry="52" fill="var(--bg-accent)" />
          {/* Main center body */}
          <ellipse cx="220" cy="148" rx="148" ry="110" fill="var(--bg-accent)" />
          {/* Top bridge */}
          <rect x="100" y="70" width="240" height="60" rx="12" fill="var(--bg-accent)" />

          {/* ── Triggers ── */}
          {/* LT background */}
          <rect x="68" y="38" width="72" height="28" rx="10" fill="var(--bg-input)" />
          {/* LT fill when pressed */}
          {lt && <rect x="68" y="38" width="72" height="28" rx="10" fill="#5794F2" opacity="0.85" />}
          <text x="104" y="57" textAnchor="middle" fill={lt ? '#fff' : 'var(--text-secondary)'} fontSize="11" fontFamily="ui-monospace,monospace" fontWeight="600">LT</text>

          {/* RT background */}
          <rect x="300" y="38" width="72" height="28" rx="10" fill="var(--bg-input)" />
          {lt && <rect x="300" y="38" width="72" height="28" rx="10" fill="#5794F2" opacity="0" />}
          {rt && <rect x="300" y="38" width="72" height="28" rx="10" fill="#5794F2" opacity="0.85" />}
          <text x="336" y="57" textAnchor="middle" fill={rt ? '#fff' : 'var(--text-secondary)'} fontSize="11" fontFamily="ui-monospace,monospace" fontWeight="600">RT</text>

          {/* ── Bumpers ── */}
          {/* LB */}
          <rect x="72" y="70" width="76" height="22" rx="8" fill={p(BTN_LB) ? '#5794F2' : 'var(--border)'} />
          <text x="110" y="85" textAnchor="middle" fill={p(BTN_LB) ? '#fff' : '#8e8e8e'} fontSize="11" fontFamily="ui-monospace,monospace" fontWeight="600">LB</text>

          {/* RB */}
          <rect x="292" y="70" width="76" height="22" rx="8" fill={p(BTN_RB) ? '#5794F2' : 'var(--border)'} />
          <text x="330" y="85" textAnchor="middle" fill={p(BTN_RB) ? '#fff' : '#8e8e8e'} fontSize="11" fontFamily="ui-monospace,monospace" fontWeight="600">RB</text>

          {/* ── Xbox button (center top) ── */}
          <circle cx="220" cy="102" r="14" fill={p(BTN_MENU) || p(BTN_VIEW) ? '#fff' : 'var(--border)'} />
          <circle cx="220" cy="102" r="10" fill="var(--bg-input)" />
          <text x="220" y="106" textAnchor="middle" fill="var(--text-secondary)" fontSize="10" fontFamily="ui-monospace,monospace">⊙</text>

          {/* ── View button (small, left of center) ── */}
          <rect x="172" y="130" width="22" height="14" rx="4" fill={p(BTN_VIEW) ? '#5794F2' : 'var(--border)'} />
          <text x="183" y="141" textAnchor="middle" fill={p(BTN_VIEW) ? '#fff' : 'var(--text-secondary)'} fontSize="8" fontFamily="ui-monospace,monospace">≡</text>

          {/* ── Menu button (small, right of center) ── */}
          <rect x="246" y="130" width="22" height="14" rx="4" fill={p(BTN_MENU) ? '#5794F2' : 'var(--border)'} />
          <text x="257" y="141" textAnchor="middle" fill={p(BTN_MENU) ? '#fff' : 'var(--text-secondary)'} fontSize="8" fontFamily="ui-monospace,monospace">☰</text>

          {/* ── Left stick (top-left area) ── */}
          <circle cx="148" cy="162" r="24" fill={p(BTN_LS) ? '#5794F2' : 'var(--border)'} />
          <circle cx="148" cy="162" r="18" fill={p(BTN_LS) ? '#3a5fd4' : '#111417'} />
          <text x="148" y="167" textAnchor="middle" fill={p(BTN_LS) ? '#fff' : 'var(--text-secondary)'} fontSize="9" fontFamily="ui-monospace,monospace">LS</text>

          {/* ── D-pad (bottom-left area) ── */}
          {/* Up */}
          <rect x="103" y="178" width="18" height="18" rx="3" fill={p(BTN_DPAD_UP) ? '#d9d9d9' : 'var(--border)'} />
          {/* Down */}
          <rect x="103" y="212" width="18" height="18" rx="3" fill={p(BTN_DPAD_DOWN) ? '#d9d9d9' : 'var(--border)'} />
          {/* Left */}
          <rect x="83" y="196" width="18" height="18" rx="3" fill={p(BTN_DPAD_LEFT) ? '#d9d9d9' : 'var(--border)'} />
          {/* Right */}
          <rect x="123" y="196" width="18" height="18" rx="3" fill={p(BTN_DPAD_RIGHT) ? '#d9d9d9' : 'var(--border)'} />
          {/* Center cross */}
          <rect x="103" y="196" width="18" height="18" fill="var(--bg-accent)" />

          {/* ── Right stick (bottom-right area) ── */}
          <circle cx="276" cy="200" r="24" fill={p(BTN_RS) ? '#5794F2' : 'var(--border)'} />
          <circle cx="276" cy="200" r="18" fill={p(BTN_RS) ? '#3a5fd4' : '#111417'} />
          <text x="276" y="205" textAnchor="middle" fill={p(BTN_RS) ? '#fff' : 'var(--text-secondary)'} fontSize="9" fontFamily="ui-monospace,monospace">RS</text>

          {/* ── Face buttons ── */}
          {/* Y — top (yellow) */}
          <circle cx="318" cy="138" r="14" fill={p(BTN_Y) ? '#F0A500' : 'var(--border)'} />
          <text x="318" y="143" textAnchor="middle" fill={p(BTN_Y) ? '#000' : '#8e8e8e'} fontSize="11" fontFamily="ui-monospace,monospace" fontWeight="700">Y</text>

          {/* X — left (blue) */}
          <circle cx="298" cy="158" r="14" fill={p(BTN_X) ? '#5794F2' : 'var(--border)'} />
          <text x="298" y="163" textAnchor="middle" fill={p(BTN_X) ? '#fff' : '#8e8e8e'} fontSize="11" fontFamily="ui-monospace,monospace" fontWeight="700">X</text>

          {/* B — right (red) */}
          <circle cx="338" cy="158" r="14" fill={p(BTN_B) ? '#C4162A' : 'var(--border)'} />
          <text x="338" y="163" textAnchor="middle" fill={p(BTN_B) ? '#fff' : '#8e8e8e'} fontSize="11" fontFamily="ui-monospace,monospace" fontWeight="700">B</text>

          {/* A — bottom (green) */}
          <circle cx="318" cy="178" r="14" fill={p(BTN_A) ? '#37872D' : 'var(--border)'} />
          <text x="318" y="183" textAnchor="middle" fill={p(BTN_A) ? '#fff' : '#8e8e8e'} fontSize="11" fontFamily="ui-monospace,monospace" fontWeight="700">A</text>
        </svg>
      </div>
    </div>
  )
}
