# Damage Page Design

Status: Native fields and Electron page in source (`DamagePage.tsx`, Stint sidebar removed); not yet built or run. Qt page pending  
Last updated: 2026-10-05  
Scope: Electron first, with shared data semantics suitable for a later Qt port

## 1. Purpose and confirmed decisions

Add a page named **Damage** that shows the selected driver's car damage and
power-unit wear in live telemetry and recorded-session playback. It finishes
the engine-component wear that the Stint page deferred.

- The page shows only the streamed driver: the player in live, and the driver
  selector's car in V6 playback. There is no all-cars grid and no driver
  selector in live.
- Body damage, tyre and brake damage, blisters, faults, gearbox and engine
  damage use the fields already parsed.
- The six power-unit components (ICE, MGU-H, MGU-K, ES, CE, TC) and the engine
  blown/seized flags are new. They come from bytes the parsers currently skip.
- The Stint page's damage sidebar is removed. Its charts take the full width.
- Collision events, collision markers and a damage log are out of scope.
- The page updates in realtime, as rows arrive; nothing polls.
- No Qt or Android UI is part of the first delivery.

## 2. Content

Layout, top to bottom: the summary strip, the car (full width, with callouts),
and one row of eight wear cards labelled "ICE (Wear)" … "Gearbox (Wear)", with no section headings. It follows the existing Electron design language (Cascadia Code,
tabular numbers, theme variables, thin dividers, dark/midnight/light themes,
shared chart wrappers, and no page scrolling at normal window sizes).

| Group | Items | Source |
|---|---|---|
| Summary | Engine wear, Gearbox wear, DRS fault, ERS fault, engine status (OK / Blown / Seized) | Damage row |
| Body | Front wing L, front wing R, rear wing, floor, sidepod, diffuser | Existing fields |
| Wheels | Per corner: tyre damage, brake damage, blisters (blisters F1 25 onward) | Existing fields |
| Wear tiles | ICE, MGU-H, MGU-K, ES, CE, TC (new fields), then Engine and Gearbox (`engine_damage` / `gearbox_damage`, which behave as wear) | Damage row |

The Body and Wheels groups share a top-down car wireframe, nose pointing right,
with callouts around it. Reuse the current percentage formatting and colour
rules from the Stint sidebar and `DamagePanel`.

### Car asset

The wireframe is a shared SVG, kept as identical copies in
`electron-frontend/src/renderer/src/assets/car/f1-car-wireframe.svg` and
`qt_frontend/assets/car/f1-car-wireframe.svg` (`:/car/f1-car-wireframe.svg`).
It uses plain SVG only, so QtSvg renders it unchanged.

- Damage zones by id: `z-fw-l`, `z-fw-r`, `z-rw`, `z-floor`, `z-diff`,
  `z-sp-l`, `z-sp-r`, and tyres `t-fl`, `t-fr`, `t-rl`, `t-rr`. A damaged zone
  takes the severity colour on its strokes.
- Shapes with class `solid` must be filled with the panel background so hidden
  lines drop out. Electron overrides the presentation attributes with CSS by
  class (`wire`, `solid`, `detail`, `tube`); Qt rewrites them by id before
  loading.
- Invisible `a-*` points (`a-fl`, `a-floor`, `a-rw`, …) mark where each
  callout's leader line meets its part, in the asset's viewBox units.

TC is the turbocharger, ES the energy store, and CE the control electronics.

## 3. Metric definitions

| Metric | Definition | Display |
|---|---|---|
| Damage and wear values | Latest reported value at the live time or playback cursor | Integer % |
| Engine status | SEIZED if `engine_seized`, else BLOWN if `engine_blown`, else FAIL if overall engine wear (`engine_damage`) is 100% (the game leaves both flags unset for some failures, e.g. an MGU-H failure), else OK | Text, coloured |
| Faults | `drs_fault` / `ers_fault`, 0 = OK, 1 = fault. The `drs_fault` card's title is the catalog label `ui.damage.wing_fault`: DRS, or Rear Wing when the session presents as 2026, the same gate as the Overview DRS/SLM card (see below) | OK / Fault |

Colours:

- **Tyre, brake and blister values** use five 20% bands: green below 20%,
  green-yellow below 40%, yellow below 60%, yellow-red below 80%, red from 80%.
  A tyre outline always takes the band colour of its wheel's worst value, green included.
- **Body parts**: green at 0%, amber up to 20%, red above.
- **Power unit, engine and gearbox wear**: green below 50%, amber below 75%, red
  from 75%. Each tile is the shared stat card showing the wear value only.

## 4. Protocol fields

All three supplied specifications define the same trailing fields in
`CarDamageData`, after `m_engineDamage`:

| Field | Type | F1 24 offset | F1 25 / 2026 offset | Restricted |
|---|---|---|---|---|
| `m_engineMGUHWear` | uint8 % | 34 | 38 | Yes |
| `m_engineESWear` | uint8 % | 35 | 39 | Yes |
| `m_engineCEWear` | uint8 % | 36 | 40 | Yes |
| `m_engineICEWear` | uint8 % | 37 | 41 | Yes |
| `m_engineMGUKWear` | uint8 % | 38 | 42 | Yes |
| `m_engineTCWear` | uint8 % | 39 | 43 | Yes |
| `m_engineBlown` | uint8 0/1 | 40 | 44 | No |
| `m_engineSeized` | uint8 0/1 | 41 | 45 | No |

Offsets are within each car's struct. F1 24 has no `m_tyreBlisters[4]`, so its
struct is 42 bytes and every field after brakes sits 4 bytes earlier than in
F1 25 (46 bytes). The 2026 Season Pack keeps the 46-byte struct; only the car
array grows from 22 to 24. The packet is sent at 10 Hz.

The restricted-data list zeroes the six component wears for opponents whose
"Your Telemetry" is Restricted. The blown and seized flags are not on the list,
so like blisters and `ers_fault` they are readable for every car. The page shows
only the selected driver, but in V6 playback that can be a restricted opponent:
the page must show unavailable, not 0%, for restricted values.

Verified source sections:

| Supplied reference | Relevant sections | Physical pages |
|---|---|---|
| [F1 24 v27.2x](../.agents/skills/f1-udp-verify-spec/references/f1-24-v27.2x.md) | Car Damage / `CarDamageData`; Restricted data | DOCX extraction; no inferred pagination |
| [F1 25 v3](../.agents/skills/f1-udp-verify-spec/references/f1-25-v3.md) | Same | PDF 13–14, 17–18 |
| [2026 Season Pack, Season 8 v1.2](../.agents/skills/f1-udp-verify-spec/references/f1-25-2026-season-8.md) | Same | PDF 13–14, 19–20 |

## 5. Native changes

- **Rows** (`tnrp/rows.h`): add `engine_mguh_wear`, `engine_es_wear`,
  `engine_ce_wear`, `engine_ice_wear`, `engine_mguk_wear`, `engine_tc_wear`,
  `engine_blown` and `engine_seized` to `DamageRow` (player) and `TyreWearCar`
  (per car, optional), plus their glaze metadata.
- **Parsers** (`protocols/f1_24.cpp`, `f1_25.cpp`, `f1_26.cpp`, `CAR_DAMAGE`):
  read the new bytes at the version's offsets. In the per-car loop, read the
  six wears only under `hasPrivateTelemetry`, and blown/seized from the public
  base, as blisters and `ers_fault` are today.
- **V6 recording** (`tnrd/TNRD_V6.cpp`, `damage` in `appendRows`): add the new
  fields to the `ADD_OPT` list for `V6DataType::Damage`. V6 stores damage
  samples by field name, so this is not expected to need a format version bump;
  confirm that older readers ignore unknown fields before relying on it.
- **Older recordings** lack these fields. Readers and the page must treat them
  as unavailable, never as 0.
- No new packets, row types, or transport are needed.

### 2026 presentation gate

On the 2026 protocol a session can still be pre-2026 cars; 2025 and 2026 cars
never share a session, so one car decides. `tnrp::presentationFormat` uses, in
order:

1. Car Telemetry 2 `m_2026Regulations` for the player's car (car 0 when
   spectating). Live, the parser tracks it; recordings store it in the V6
   session header as `regulations_2026`, written when the file is finished.
2. Session `m_formula` (13 = F1 26), for recordings made before (1) was stored
   and for recovered recordings, whose opening session record predates it.
3. The protocol alone, for recordings made before Formula was captured.

This drives the label catalog (including `ui.damage.wing_fault`) and
capabilities such as `hasMguh`. The pairing welcome and `protocol_status` both
carry the flag, and the Android client resolves its aero mode in the same order.

## 6. Electron integration

- **Tab**: add `'damage'` to `Tab` in `app/appConfig.ts`, and wire it through
  `TabContent.tsx`, `TabOptionLabel.tsx` and `useAppConfiguration.ts` the same
  way as `'stint'`.
- **Types and history**: add the new fields to `DamageRow` and the damage
  field list in `types.ts` and `lib/columnStore.ts`.
- **Current values**: the existing `damage` row at the live time or cursor.
- **Data**: a `damagePage` consumer streams `DATA_ROW.damage` (V6 type 14).
  The page draws no history, so it requests none.
- **Stint page**: remove the Damage and Engine Wear sidebar from
  `TrendPanel.tsx`. The main grid becomes a single full-width column. Update
  `TREND_PAGE_DESIGN.md` sections 1, 2 and 4 to match.
- **Qt**: check whether `PlaybackPatchMerger.cpp` lists damage fields
  explicitly; if so, add the new ones so Qt playback does not drop them, even
  though Qt gets no Damage page yet.

## 7. Availability

- The player's own car always has every field.
- A public opponent in V6 playback has every field.
- A restricted opponent has blown/seized, blisters and ERS fault only. Body,
  wheel and power-unit values show unavailable, and the timeline stops through
  restricted intervals.
- Unknown telemetry access is treated as unavailable until established.
- Seeks, flashbacks, driver switches and session changes rebuild the
  timeline for the surviving timeline; nothing from the future of the cursor
  or from another driver may leak.

## 8. Implementation sequence and acceptance review

1. Native rows and the three parsers.
2. V6 writer fields, and confirmation that old and new recordings both load.
3. Electron types, column store and history request.
4. Visual design of the page (not started).
5. Damage page implementation, then removal of the Stint sidebar.

When implementation is authorized, review:

- Component values match the game's car-condition screen for the player in
  F1 24, F1 25 and 2026-format sessions.
- F1 24 offsets do not pick up F1 25 values, and vice versa.
- Restricted opponents show unavailable rather than 0% for private fields,
  while blown/seized still display.
- Older recordings open without errors and show the new fields as unavailable.
- Values follow the cursor correctly after seeks, flashbacks and a session
  change.
- The Stint page renders correctly without the sidebar in all themes and in a
  small window.

## 9. Open questions

- Should the timeline's X axis default to time or lap?
- Should the Overview page's `DamagePanel` gain any of the new fields, or stay
  as it is?
