# Track N Race — Tauri experiment

An independent desktop frontend in the requested folder. The React renderer was copied from the Electron frontend; the application host is Tauri 2 / Rust. The existing C++ library is linked into the application. No Electron, Node runtime, Node addon, or sidecar process is used by the installed app.

This is an implementation with static validation, not a runtime-validated release. The application has not been built, launched, or checked visually on any platform.

## Development

Requirements:

- Node.js 22.12 or newer and npm.
- Rust 1.90 or newer, with the platform's native target.
- CMake 3.18 or newer and a C++23 compiler compatible with the shared library's Glaze dependency.
- Windows: MSVC C++ tools, Windows SDK, and WebView2.
- macOS: Xcode command-line tools.
- Linux: the [Tauri system prerequisites](https://v2.tauri.app/start/prerequisites/), including WebKitGTK and the tray dependencies.

Run these commands from this directory:

~~~sh
npm ci
npm run dev
~~~

The development command starts Vite on port 1420 and builds/launches Tauri. Cargo's build script invokes CMake and builds the shared library automatically. Its third-party native dependencies are fetched through the library's existing CMake configuration.

~~~sh
npm run build
~~~

This builds the renderer, links the native engine, and creates the platform's application bundle under src-tauri/target/release/bundle/. The build:web script only builds renderer assets and does not produce a desktop application.

To build and run the production executable locally:

~~~sh
npm start
~~~

This builds the production renderer and native executable for the current machine, skips installer packaging, and launches the freshly built application. It stops if the build fails. Arguments are forwarded to the app, for example: npm start -- "path/to/session.tnrd".

Checks that do not build or launch the application:

~~~sh
npm run typecheck
cargo fmt --manifest-path src-tauri/Cargo.toml --all -- --check
~~~

Both npm and Cargo dependency lockfiles are included. Dependency caches and build output are ignored.

## Native connection

The data path is:

~~~text
React / Zustand / WebGL charts
  ↕ existing window.*Bridge interfaces
src/platform/bridges.ts + ipc.ts
  ↕ Tauri commands and acknowledged binary Channel
src-tauri/src/host.rs
  ↕ small, owned C ABI
src-tauri/native/core.cpp
  ↕ tnrp::Engine / TnrdReader / calculateLapDelta / exportTnrdFileToXlsx
../protocol_parser_library
~~~

The Cargo build script builds the CMake wrapper and statically links tnrp, Glaze's consumers, XLSX support, Zstandard, and zlib. No modifications to the shared library are required.

Native build output lives in .native/<target>/<profile>/ at the frontend root. This avoids MSBuild file-tracker failures caused by CMake's compiler-probe paths exceeding Windows path limits inside Cargo's deeply nested build directory. Rust output remains in Cargo's target directory. The new native directory is selected automatically; no deletion of existing Cargo output is needed.

Implemented library operations include:

- Live capture, protocol selection, forwarding configuration, team colors, and strategy settings.
- Recording settings and orderly recording flush on exit.
- Playback load/close, play/pause/speed, driver/focus selection, seeking, history requirements, and visibility restoration.
- Live lap requests, lap-history subscriptions, all-laps/window backfill.
- Independent secondary recording analysis and native lap-delta calculation.
- XLSX export with progress.
- Pairing controls and opaque native pairing-state persistence.

The Rust host serializes native commands on blocking workers, keeping C++ work off the webview thread. The engine retains ownership of its internal capture, playback, recording, and pairing threads.

Binary envelopes contain a little-endian 32-bit JSON-header length, a UTF-8 metadata header, then opaque binary bytes. The host does not reinterpret F1 packets or change recording formats. The same renderer decoders consume the library's binary output. Each envelope is acknowledged before the next is sent. Seek acknowledgements remain separate: post-seek rows wait until the renderer has installed history.

The native queue bounds ordinary telemetry backlog at 32 MiB; a dropped backlog triggers a library history restore. Required control/history responses are retained, and individual history responses can exceed that limit. The renderer's seek buffer is capped at 64 MiB. Superseded queued seek responses are discarded.

## Desktop behavior and storage

Settings live in Tauri's application-data directory for com.tracknrace.tauri, in settings.json. They are loaded before React mounts and persisted through Rust. This frontend has independent settings and pairing identities; it does not alter Electron's preferences.

The inherited name window.electronStore is retained only as a renderer compatibility API. Its implementation uses the Tauri settings cache and Rust persistence.

The host supports native file dialogs, recording-file startup arguments, single-instance file activation, macOS file-open events, tray Show/Quit, fullscreen, and a custom titlebar option. Native window decorations are the default. Both .tnrd and legacy .trnd extensions are registered when packaged.

Diagnostics are written to launch-diagnostics/main.log inside the app-data directory. Pairing credentials remain in native storage and are removed before public pairing events reach the renderer. Shutdown saves the final pairing state and waits for recording cleanup.

## Current differences and validation limits

- There are no published Tauri release artifacts. Automatic update offers are disabled so the app cannot offer an Electron installer as an upgrade.
- Electron/V8/Blink process-memory statistics and Node-API exception controls are not applicable. The renderer retention logger and optional React/WebGL instrumentation remain.
- The command worker currently serializes long operations, including exports and analysis. Responsiveness under overlapping operations needs runtime measurement.
- Windows uses WebView2; macOS and Linux use WebKit. Chart performance, large-history transfer, file associations, tray behavior, packaging, and custom-titlebar behavior still need runtime validation.
- This renderer is a copy, not an automatic mirror of Electron. Changes to shared UI behavior must be carried across deliberately.

Validation performed while creating this frontend: TypeScript no-emit checks, C++ syntax-only checking against the actual library headers, Rust parsing/formatting, Tauri configuration-schema validation, and bridge-operation coverage inspection. These do not establish that the final Rust/C++ executable links or runs.
