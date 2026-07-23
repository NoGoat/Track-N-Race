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

## Screenshots

### Overview Page
This page has the Speed, RPM and ERS trace along with some information like tyre status and car damage. You can turn off cards that you don't need via the Edit Overlay button at the top left.

![Overview](screenshots/Screenshot%202026-07-24%20at%2012.43.36%E2%80%AFAM.png)

### Analyze
The Analyze page is meant for comparing any data point against any lap. You can select two laps and compare most of the available data points against each other.

![Analyze](screenshots/Screenshot%202026-07-24%20at%2012.44.09%E2%80%AFAM.png)

### Session
The session page mostly keeps track of things like lap count, air temp, track temp, weather and events. It also includes a full map of almost every circuit. It has maps for every circuit from F1 25 and the 2026 DLC. It should be missing any map exclusive to F1 24 since I couldn't generate those maps due to not owning the game.

![Session](screenshots/Screenshot%202026-07-24%20at%2012.44.30%E2%80%AFAM.png)

### Strategy
The Strategy page is still a work in progress. Its supposed to mathematically suggest strategies based on your lap times compared to your rival's lap times but its very barebones for now.

![Strategy](screenshots/Screenshot%202026-07-24%20at%2012.46.06%E2%80%AFAM.png)

### Standings
The Standings page is your bog standard timing tower. It shows your current position, your sector and lap times and all that good stuff.

![Standings](screenshots/Screenshot%202026-07-24%20at%2012.46.26%E2%80%AFAM.png)

### Inputs
The inputs page shows your gear, throttle and Steering. Nothing too impressive here.

![Inputs](screenshots/Screenshot%202026-07-24%20at%2012.46.33%E2%80%AFAM.png)

### Power
The power page shows your harvest, your ERS vs ICE power split, your ERS store and your fuel level.

![Power](screenshots/Screenshot%202026-07-24%20at%2012.46.38%E2%80%AFAM.png)

### Tyres
The Tyres page. It shows your current set, your used sets, and your current tyre status. It also has a Graph view where you can see the history of tyres

![Tyres](screenshots/Screenshot%202026-07-24%20at%2012.46.43%E2%80%AFAM.png)

### Tyres: Graph View
The Graph view plots the Surface Temp, the Inner Temp, the Brake Temp and Tyre Life on a chart.

![Tyres Graph View](screenshots/Screenshot%202026-07-24%20at%2012.46.46%E2%80%AFAM.png)

### Misc
This page has the G Forces and Ride Heights. If you need it, you can find it here.

![Misc](screenshots/Screenshot%202026-07-24%20at%2012.46.50%E2%80%AFAM.png)

### Overview with Tyre Charts
You can enable the tyre charts in the Overview page as well.

![Overview with Tyre Charts](screenshots/Screenshot%202026-07-24%20at%201.12.35%E2%80%AFAM.png)

### Table View
You can toggle each chart to instead show a table in case you want to see the raw values for yourself. It is toggleable on a per chart basis, so you can pick and choose as needed.

![Table view](screenshots/Screenshot%202026-07-24%20at%201.24.58%E2%80%AFAM.png)

### Events
Events are highlighted in the titlebar.

![Event in titlebar](screenshots/Screenshot%202026-07-24%20at%201.26.26%E2%80%AFAM.png)

### Normal and Compact cards
Almost all cards can be configured to either be normal or compact. This one is toggleable on a per section basis.

Normal Cards:
![Normal cards](screenshots/Screenshot%202026-07-24%20at%201.28.19%E2%80%AFAM.png) 
Compact Cards:
![Compact cards](screenshots/Screenshot%202026-07-24%20at%201.29.13%E2%80%AFAM.png)

### Minimal Recorder
In case you are worried about performance hiccups, there is a minimal recorder. This one will just record the TNRD files and you can view it later.
![Minimal Recorder](screenshots/Screenshot%202026-07-24%20at%201.19.26%E2%80%AFAM.png)

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
