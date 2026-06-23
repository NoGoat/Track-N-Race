# Track N Race

A real-time telemetry dashboard for F1 25, built as a native desktop application. Track N Race receives UDP telemetry broadcast by the game and presents it through a structured, multi-tab interface — covering everything from live car inputs and thermal data to a full timing tower and interactive track map.

---

## 📋 Table of Contents

- [Overview](#overview)
- [Features](#features)
- [Tech Stack](#tech-stack)
- [Prerequisites](#prerequisites)
- [Installation](#installation)
- [Game Configuration](#game-configuration)
- [Usage](#usage)
- [Project Structure](#project-structure)
- [Configuration](#configuration)
- [Building for Distribution](#building-for-distribution)
- [License](#license)

---

## Overview

Track N Race connects to F1 25 over a local UDP socket (default port `20777`) and parses the game's binary telemetry packets in real time. Parsed data is relayed to the renderer process via Electron IPC and visualized across several focused dashboard tabs. The application is frameless with a custom title bar and supports both dark and light themes.

---

## ✨ Features

### Overview Tab
- Live stat cards for speed, RPM, gear, throttle, brake, DRS, engine temperature, ERS store, fuel load, race position, and tyre compound
- Rolling speed/RPM chart with selectable time windows (15s – 10m)
- Thermal panel displaying surface temperature, inner temperature, and brake temperature per corner, alongside tyre wear/life trends
- Damage panel covering aero components (front wing, rear wing, floor, diffuser, sidepod), drivetrain (gearbox, engine), and per-corner tyre/brake damage

### Input Tab
- Gear indicator with animated display
- Throttle and brake trace chart
- Steering angle chart

### Power Tab
- ICE and MGU-K power output in kW, with a combined total and ICE/MGU-K split indicator
- ERS store, ERS deployment, and ERS harvest trend charts (MGU-K and MGU-H)
- Fuel load history chart
- Stats bar covering total power, ICE, MGU-K, ERS percentage, and current fuel

### Tyres Tab
- Per-corner surface temperature, inner temperature, and brake temperature charts
- Tyre life / tyre wear trend over the session with a switchable display mode
- Full tyre set allocation for the weekend, showing compound, wear, availability, and lap delta

### Timing Tower Tab
- Live 20-car timing table: position, driver code, race number, lap number, last lap time, gap to leader, S1/S2/S3 sector times, and current tyre compound
- Sector times freeze on lap completion and display for 7 seconds, giving a full sector breakdown per car per lap
- Pit lane, invalid lap, time penalty, drive-through, and stop-go badges per car
- Driver selection: clicking a row updates the sidebar and several detail panels to show that driver's data
- Fastest-lap holder highlighted in purple

### Session Tab
- Grand Prix name, circuit name, and session type with a colour-coded accent (blue for practice, gold for qualifying, red for race)
- Session time remaining and real-time marshal zone strip (green, yellow, blue flags)
- Track temperature, air temperature, track length, time of day, pit speed limit, pit window, and recommended rejoin position
- Current weather with a 5-step forecast strip showing condition and rain probability
- Interactive track map rendered on a Canvas element at up to 60 fps, with DRS zones, speed traps, start/finish line, sector boundaries, and live car positions using team livery colours
- Map supports dots-only, labels-only, or combined display mode; static drivers can be hidden after a configurable timeout
- Sector-colour mode for the map track lines (optional)
- Proximity widget showing the player's immediate neighbours on track with inter-car gaps
- Scrollable race event log (fastest lap, penalties, safety car, DRS, flags, retirements, race winner)

### Misc Tab
- Lateral and longitudinal G-force chart
- Front and rear aero ride height chart (plank edge height above road surface in mm)

### Event Banners
- Transient notifications appear in the header for configurable durations (2 – 10 seconds): fastest lap, penalties, flags, DRS state changes, safety car, race winner, and more
- In fullscreen mode, a floating banner replaces the header notification
- Safety car, virtual safety car, and formation lap indicators persist in the header until the condition ends

### Customisable Layouts
- Each tab (Overview, Input, Misc, Power, Tyres) has an edit mode that lets individual panels and charts be shown or hidden. Preferences are persisted across sessions.

### General
- Custom frameless window with minimize, maximize, restore, fullscreen, and close controls
- Dark and light themes
- Taskbar icon adapts to the Windows taskbar theme automatically
- All settings (UDP port, bind address, appearance, map options) are accessible from a dedicated settings tab

---

## 🛠 Tech Stack

| Layer | Technology |
|---|---|
| Application framework | [Electron](https://www.electronjs.org/) v42 |
| Build tooling | [electron-vite](https://electron-vite.org/) v5 |
| UI framework | [React](https://react.dev/) 18 with TypeScript |
| Styling | [Tailwind CSS](https://tailwindcss.com/) v3 |
| Charts | [uPlot](https://github.com/leeoniya/uPlot) v1.6 |
| Select inputs | [react-select](https://react-select.com/) v5 |
| Icons | [lucide-react](https://lucide.dev/) |
| Persistent settings | [electron-store](https://github.com/sindresorhus/electron-store) v8 |
| Packaging | [electron-builder](https://www.electron.build/) v26 |

---

## Prerequisites

- [Node.js](https://nodejs.org/) 20 or later
- npm (bundled with Node.js)
- F1 25 (PC) with UDP telemetry enabled

---

## Installation

```bash
# Clone the repository
git clone https://github.com/NoGoat/Track-N-Race.git
cd Track-N-Race

# Install dependencies
npm install
```

---

## Game Configuration

F1 25 must be configured to broadcast UDP telemetry to the machine running Track N Race.

1. Open F1 25 and navigate to **Settings → Telemetry Settings**.
2. Set **UDP Telemetry** to `On`.
3. Set **UDP Broadcast Mode** to `Off` (or `On` if the dashboard is on a different device on the same network).
4. Set **UDP IP Address** to the IP address of the machine running Track N Race (use `127.0.0.1` if running on the same machine).
5. Set **UDP Port** to `20777` (the application default).
6. Set **UDP Send Rate** to `60Hz` for the smoothest data.
7. Set **UDP Format** to `2024` or later.

If you change the port, update it in **Track N Race → Settings → UDP Port** and click **Apply & Restart**.

---

## Usage

```bash
# Start in development mode (with hot reload)
npm run dev

# Preview the production build
npm run start
```

The application window opens automatically. Once F1 25 is running with a session active, data will begin populating the dashboard. The session badge in the title bar changes from **Offline** to the current session type (Practice, Qualifying, Race, etc.) when a connection is established.

---

## 📁 Project Structure

```
Track-N-Race/
├── src/
│   ├── main/                   # Electron main process
│   │   ├── index.ts            # Window creation, IPC handlers, theme polling
│   │   ├── udpReceiver.ts      # UDP socket lifecycle (start/stop)
│   │   ├── packetParsers.ts    # Binary packet parsing (F1 25 UDP spec)
│   │   ├── packetHeader.ts     # Packet header parsing
│   │   └── rateLimiter.ts      # Per-packet-type rate limiting
│   ├── preload/
│   │   └── index.ts            # Context bridge (telemetry, window controls, store, UDP)
│   └── renderer/
│       └── src/
│           ├── App.tsx         # Root component, tab routing, event banners
│           ├── types.ts        # Shared TypeScript interfaces for all message types
│           ├── components/     # One file per panel/chart/tab component
│           ├── hooks/          # useTelemetry, useAppConfig, useSize, useChartTooltip
│           └── lib/            # Track map geometry data
├── build/                      # Icons for packaging
├── electron.vite.config.ts
├── package.json
└── tailwind.config.js
```

### Key Modules

**`packetParsers.ts`** — Decodes the binary UDP packets emitted by F1 25 according to the game's telemetry specification. Handles packet IDs 0–3, 4, 6–7, 10–13 (Motion, Session, Lap Data, Events, Participants, Car Telemetry, Car Status, Car Damage, Session History, Tyre Sets, and MotionEx). Each parser produces a typed row that is broadcast to the renderer via IPC.

**`useTelemetry.ts`** — Subscribes to the IPC telemetry bridge in the renderer, maintains rolling buffers for chart data, and derives aggregated state (fastest lap holder, lap history, per-lap telemetry slices) that feeds the dashboard components.

**`TrackMap.tsx`** — Renders a Canvas-based track map at up to 60 fps using a `requestAnimationFrame` loop that runs entirely outside React's render cycle. Car positions are received directly from the IPC bridge without state updates to avoid unnecessary re-renders.

---

## ⚙️ Configuration

All settings are persisted via `electron-store` and survive application restarts.

| Setting | Description | Default |
|---|---|---|
| UDP Port | Port the game broadcasts to | `20777` |
| Bind Address | Network interface to listen on | `0.0.0.0` |
| Theme | Dark or light interface | `dark` |
| Time Window | Rolling window duration for charts | `30s` |
| Tyre View Mode | Cards or graphs in the Overview tab | `cards` |
| Tyre Wear Mode | Show remaining life or accumulated wear | `life` |
| Event Banner Duration | Display time per transient notification | `3s` |
| Sector Colors | Color map track lines by sector | `off` |
| Drivers Mode | Dots, labels, or both on the track map | `dots + labels` |
| Hide Static Drivers | Remove unmoving drivers from map after N seconds | `10s` |
| Map Timeout | Alias for the above | `10s` |

---

## Building for Distribution

```bash
# Build the application bundle only (unpacked)
npm run pack

# Build a full installer
npm run dist
```

The installer is output to the `dist/` directory. On Windows, an NSIS installer is produced. Linux and MacOS are not yet configured.

---

## License

This project is licensed under the terms of the [GPLv3 License](LICENSE).
