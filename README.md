# Track N Race

A real-time telemetry suite for F1 24, F1 25, and F1 26.

Track N Race receives the game's UDP telemetry and displays it in a multi-tab dashboard covering car inputs, power unit and tyre data, a live timing table, and a live track map. Sessions can be recorded, replayed, and exported to XLSX. The game's format year is detected automatically.

Has an Electron version and a Qt version. Both apps have feature parity and are compatible with each other.

---

## Features

### Overview
- Live stat cards for speed, RPM, gear, throttle, brake, DRS, engine temperature, ERS store, fuel load, race position, and tyre compound
- Rolling speed/RPM chart with selectable time windows (15 seconds to 10 minutes)
- Thermal panel with surface, inner, and brake temperatures per corner, alongside tyre wear/life trends
- Damage panel covering aero (front wing, rear wing, floor, diffuser, sidepod), drivetrain (gearbox, engine), and per-corner tyre and brake damage

### Inputs
- Gear chart
- Throttle and brake trace chart
- Steering angle chart

### Power Unit
- ICE and MGU-K power output in kW, with combined total and ICE/MGU-K split indicator
- ERS store, deployment, and harvest trend charts (MGU-K and MGU-H)
- Fuel load history
- Stats bar with total power, ICE, MGU-K, ERS percentage, and current fuel

### Tyres
- Per-corner surface, inner, and brake temperature charts
- Tyre life / wear trend over the session with a switchable display mode
- Full weekend tyre set allocation: compound, wear, availability, and lap delta

### Timing Tower
- Live timing table: position, driver code, race number, lap, last lap time, gap to leader, S1/S2/S3 sector times, and current compound
- Sector times freeze on lap completion for a full per-car, per-lap breakdown
- Pit lane, invalid lap, time penalty, drive-through, and stop-go badges
- Click any driver to focus the sidebar and detail panels on their data
- Fastest-lap holder highlighted in purple

### Session & Track Map
- Grand Prix, circuit, and session type with colour-coded accents (practice, qualifying, race)
- Session time remaining and a real-time marshal zone strip (green, yellow, blue flags)
- Track and air temperature, track length, time of day, pit speed limit, pit window, and recommended rejoin position
- Current weather with a 5-step forecast strip showing conditions and rain probability
- Track map at up to 60 fps with DRS zones, speed traps, start/finish line, sector boundaries, and live car positions in team livery colours
- Map display modes: dots, labels, or both; optional sector-coloured track lines; auto-hide for static cars
- Proximity widget showing your immediate on-track neighbours with inter-car gaps
- Race event log: fastest laps, penalties, safety car, DRS, flags, retirements, race winner

### G-Force & Aero
- Lateral and longitudinal G-force chart
- Front and rear ride height chart (plank edge height in mm)

### Recording, Replay & Export
- Record any live session to a compact `.tnrd` file; recordings rotate automatically per session
- Replay recordings in a built-in session player with per-lap navigation, an event log, fastest-lap markers, and seeking
- Export recordings to XLSX for offline analysis

### Event Banners
- Transient header notifications with configurable duration (2 – 10 seconds): fastest lap, penalties, flags, DRS changes, safety car, race winner, and more
- Floating banner in fullscreen mode
- Safety car, virtual safety car, and formation lap indicators persist until the condition ends

### Customisation
- Every chart and panel on the Overview, Input, Power, Tyres, and Misc tabs can be shown or hidden per tab; layouts are remembered
- Dark and light themes, with a taskbar icon that adapts to the Windows taskbar theme
- Custom frameless window with minimize, maximize, fullscreen, and close controls
- All settings (UDP port, bind address, appearance, map options) live in a dedicated settings tab and persist across restarts

## Attributions
I would like to thank all the projects listed under here for making Track N Race possible.

### Shared and native components

- [Glaze](https://github.com/stephenberry/glaze)
- [libxlsxwriter](https://libxlsxwriter.github.io)
- [zlib](https://zlib.net)
- [Zstandard](https://facebook.github.io/zstd/)

### Electron app

- [Electron](https://www.electronjs.org/)
- [React](https://react.dev/)
- [React DOM](https://react.dev/)
- [electron-store](https://github.com/sindresorhus/electron-store)
- [lucide-react](https://lucide.dev/)
- [react-select](https://react-select.com/)
- [@uiw/react-color](https://github.com/uiwjs/react-color)
- [Zustand](https://zustand-demo.pmnd.rs/)
- [Tailwind CSS](https://tailwindcss.com/)
- [TimeChart](https://github.com/huww98/TimeChart) — maintained source fork
- [D3](https://d3js.org/) (`d3-axis`, `d3-color`, `d3-scale`, and `d3-selection`)
- [glMatrix](https://glmatrix.net/)
- [node-addon-api](https://github.com/nodejs/node-addon-api)
- [Cascadia Code](https://github.com/microsoft/cascadia-code)

### Native app

- [Qt](https://www.qt.io/)
- [QCustomPlot](https://www.qcustomplot.com/)
- [qt-toast](https://github.com/niklashenning/qt-toast) — maintained fork
- [Noto Sans](https://fonts.google.com/noto/specimen/Noto+Sans)
- [Breeze](https://invent.kde.org/plasma/breeze)
- [Breeze Icons](https://invent.kde.org/frameworks/breeze-icons)
- [KDE Frameworks](https://develop.kde.org/products/frameworks/)

## Past attributions
While not in use anymore, I would like to thank all the projects listed under here for serving me well at one point of this project.

- [uPlot](https://github.com/leeoniya/uPlot)
- [JSON for Modern C++ (nlohmann/json)](https://github.com/nlohmann/json)

I would also like to make a special shoutout to the [Telemetry Scan](https://github.com/alisezisli/Telemetry-Scan) project by [alisezisli](https://github.com/alisezisli). Track N Race originally started off as me trying to build an UI wrapper around their Python backend before evolving into the project it is today. Without Telemetry Scan, Track N Race wouldn't exist.

## License

This project is licensed under the terms of the [GPLv3 License](LICENSE).
