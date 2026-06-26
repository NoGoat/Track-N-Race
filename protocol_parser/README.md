# protocol_parser — telemetry bridge

Headless C++ bridge that owns UDP port 20777, parses F1 telemetry via
`protocol_parser_library` (libtnrp), records `.tnrd` files, and streams parsed
rows to the Electron app over a named pipe (Windows) / unix domain socket (Linux).

Electron spawns it on launch and closes its stdin on quit; the bridge dies with
the app. See [`../src/main/bridgeManager.ts`](../src/main/bridgeManager.ts).

## Build

Builds the library as a subdirectory; no Qt required (raw-socket UDP backend).

```sh
cmake -S protocol_parser -B protocol_parser/build -DCMAKE_BUILD_TYPE=Release
cmake --build protocol_parser/build -j$(( $(nproc) - 2 ))
```

Output: `protocol_parser/build/protocol_parser` (Linux) /
`protocol_parser/build/Release/protocol_parser.exe` (Windows). electron-builder
copies it next to the app executable (see `package.json` → `build.extraFiles`).

First configure fetches nlohmann/json v3.11.3 and zlib v1.3.1 (same pins as the
native recorder), so the `.tnrd` format stays byte-identical.

## CLI

```
protocol_parser --pipe <name/path> [--port 20777] [--bind 0.0.0.0]
                [--protocol auto|f1_24|f1_25] [--log-enabled] [--log-dir <dir>]
```

## Control protocol (newline-JSON on stdin)

```
{"cmd":"set_override","value":"auto|f1_24|f1_25"}
{"cmd":"set_logging","enabled":true,"dir":"/path"}
{"cmd":"restart_udp","port":20777,"bind":"0.0.0.0"}
{"cmd":"player_load","path":"/file.tnrd"}
{"cmd":"player_play"} | "player_pause" | "player_close"
{"cmd":"player_seek","pct":0.5}
{"cmd":"player_set_speed","mult":2.0}
```

Output: one JSON row per line (the same row shapes the React renderer already
consumes), plus control rows `protocol_status`, `protocol_warning`,
`playback_state`, `playback_loaded`, `playback_finished`, `playback_close`.

## Status

Verified end-to-end on Linux (automated): live UDP→parse→pipe, `.tnrd` record,
record→playback round-trip, and seek-rewind (`playback_seek_flush` carries the
dense history up to the seek point). On load the bridge emits `playback_lap_blocks`
(per-lap Speed/RPM/ERS blocks + lap list + events) so the renderer enters true
playback mode — without it the renderer re-filters a growing buffer every frame
and slows down over time.

Known minor gap: the old `page-visibility` playback throttle (pause streaming when
the window is hidden) is not reproduced; playback streams regardless of focus.
macOS note: the unix-socket path uses `os.tmpdir()`, which can exceed the AF_UNIX
~108-char limit on macOS — fine on Linux (`/tmp`).
