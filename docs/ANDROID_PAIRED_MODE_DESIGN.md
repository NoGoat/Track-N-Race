# Android Paired Mode Design

Status: Proposed  
Last updated: 2026-09-04  
Scope: Android frontend, Electron desktop application, shared `libtnrp` seams,
live telemetry, TNRD playback, pairing, discovery, transport security, and
connection lifecycle

## 1. Purpose

Paired mode lets the Android app act as a remote Track N Race display. The
desktop app remains the telemetry host: it receives and decodes the game's UDP
packets, or reads and paces a `.tnrd` recording, then sends the decoded Track N
Race row stream to the phone.

The user must be able to pair in either of two ways:

1. Scan a QR code shown by the desktop app.
2. Enter the same short code in both apps and let them find each other.

The recommended first implementation uses a secure IP connection on the local
network. It works when both devices use the same router and also when one device
provides a Wi-Fi hotspot. Bluetooth, Wi-Fi Direct, USB, internet relay, and
other options are compared in section 6, but they should not all become separate
v1 transports.

This document proposes code and architecture changes only. It does not
implement them.

## 2. Product decisions

- The desktop is authoritative for the active source, session timeline, and
  playback clock.
- In playback, `libtnrp::Engine` in the desktop process is the only clock.
  Android never advances the playhead from its own monotonic clock, network
  arrival time, or the advertised playback speed. It renders the latest
  authoritative `playback_state` received from the desktop.
- Android has exactly one active input source: `Direct UDP` or `Paired desktop`.
  It never merges the two streams and never silently falls back from paired mode
  to direct UDP.
- Paired mode mirrors both live and playback telemetry through the same Android
  dashboard state model.
- Playback is controlled on the desktop in v1. Android displays whether the
  source is live, playing, paused, seeking, or disconnected. Remote playback
  controls can be added later as authenticated command messages.
- Pairing is persistent until either device revokes it. A temporary pairing
  code is not a permanent credential.
- The desktop may support several saved phones, but v1 needs only one active
  phone connection. The protocol and credential store must not assume there can
  only ever be one.
- Android recording remains a direct-UDP feature in v1. In paired mode the
  desktop's recording setting is authoritative; the phone must not create a
  second partial recording from the mirrored stream.
- The network path carries decoded Track N Race rows, never raw F1 UDP packets.
- Android declares requirements for only its currently displayed page. The
  desktop requests the union needed by its own visible page and connected
  Android pages, then filters the shared result separately for each consumer.
- Backfill is a per-page capability, not a connection-wide default. The current
  race dashboard requests no backfill and receives only a current snapshot plus
  its necessary live rows.
- Pairing must not affect recording completeness, desktop rendering, or
  playback timing. Samples are not deliberately throttled or superseded; a
  phone that exceeds the bounded backlog is disconnected rather than blocking
  the engine.

## 3. Current architecture

The existing code already provides most of the correct data boundary:

```text
Live:      game UDP -> libtnrp Parser -> Engine -> Sink
Playback:  .tnrd    -> libtnrp Reader -> Engine -> Sink

Electron Sink -> N-API callbacks -> Electron main -> renderer IPC / PairService
Android direct JNI binary ---------\
                                      -> Capacitor ArrayBuffer -> shared TS decoder -> Canvas
Android paired WebSocket binary ----/
```

Relevant current behavior:

- `libtnrp::Sink` is the common post-decode seam for live and playback.
- Cold and control rows use canonical JSON. The four hot families use the
  version-sensitive packed format in `BinaryRows.h` on Electron's fast path.
- Electron receives both paths through the same addon callbacks. Renderer
  visibility currently gates renderer IPC, but does not stop parsing or
  recording.
- Android direct mode creates its own native `Engine`, binds UDP port `20777`,
  and requests packed hot rows. Both direct and paired hot batches use the same
  binary decoder in the Capacitor frontend. Every accepted row updates mutable
  Canvas state immediately; drawing follows the display refresh rate.
- Engine playback has an authoritative seek barrier. A seek replaces the
  timeline, restores sparse panel state, and then releases new-cursor rows.
- `Engine::setDataRequirements` currently represents one aggregate consumer.
  A paired phone must not lose data because the desktop renderer is hidden or
  because its active page requests a different row mask.
- Electron already derives stream and history masks from its active tab and
  visible layout through `historyDependencies.ts`. Android should use the same
  declarative pattern rather than subscribing from individual widgets.

Paired mode should reuse these semantics. It must not introduce a second F1
parser, a remote-only row schema, or a Java model for playback timing.

## 4. Goals and non-goals

### 4.1 Goals

- Pair in under a minute without typing an IP address in the normal case.
- Support a QR path and a same-code path.
- Reconnect a previously paired phone automatically when both apps are running
  on a reachable network.
- Mirror live and playback dashboards without requiring a game telemetry
  destination change.
- Make load, pause, play, speed changes, seek, playback finish, playback close,
  live flashback, and session replacement explicit timeline events.
- Keep the desktop playback cursor authoritative and forward its state at the
  Android presentation cadence without client-side clock extrapolation.
- Start a newly connected or paused-playback phone from a complete current
  snapshot instead of waiting for every row family to update naturally.
- Keep queues bounded and make hot display data latest-wins under congestion.
- Change Android's subscription atomically on every page change and stop
  forwarding families used only by the previous page.
- Keep range backfill available for future history/chart pages while disabling
  it for the current race dashboard.
- Authenticate saved devices, encrypt telemetry, and make revocation visible.
- Version the remote protocol independently of the F1 game year and TNRD file
  version.
- Leave a clean path for the Qt desktop host to implement the same protocol
  later without moving networking into the parser.

### 4.2 Non-goals for v1

- Sending raw game UDP to Android for it to decode again.
- Remote control of the game or operating system.
- Android ownership of TNRD playback or direct access to the desktop file.
- Building Android chart/Analyze pages in this work. Pair Protocol v1 retains a
  selective backfill mechanism for those future pages, while the initial race
  dashboard is latest-state only.
- Cloud accounts, discovery across the internet, or NAT traversal.
- Background telemetry when the Android UI is not actively in use. A foreground
  service can be designed separately if that product behavior is wanted.
- Simultaneously building and maintaining LAN, Bluetooth, Wi-Fi Direct, USB,
  and cloud data transports.

## 5. User experience

### 5.1 Desktop

Add a `Paired devices` page in Settings with:

- an `Enable paired mode` switch;
- current reachability (`Available on this network`, `Firewall blocked`, or
  `No usable local network`);
- `Pair a phone` actions for `Show QR code` and `Use matching code`;
- a short-lived pairing countdown;
- the connected phone name, connection quality, and source (`Live` or
  `Playback`);
- saved devices with `Rename`, `Disconnect`, and `Forget` actions;
- a `Forget all devices` action that rotates server credentials.

The QR screen shows the QR, an expiry time, and a human-readable desktop name.
The same-code screen asks the user to enter an eight-character Base32 code in
both apps. Ambiguous characters (`0/O`, `1/I/L`) are excluded. Pairing windows
expire after two minutes and close after one success.

The desktop must show and require confirmation of a new phone name before it
issues a persistent credential. This makes an unexpected pairing attempt
visible even if a short code is guessed.

### 5.2 Android

Settings gains a `Telemetry source` section:

- `Direct from game` retains today's UDP behavior.
- `Paired desktop` stops the local UDP engine and opens the paired-device page.

The paired-device page has `Scan desktop QR`, `Enter matching code`, saved
desktops, and connection state. Manual endpoint entry (`host` and `port`) is an
advanced fallback for networks where multicast discovery is disabled.

The dashboard should use a compact status treatment rather than a blocking
dialog:

- `Desktop · Live`
- `Desktop · Playback paused`
- `Desktop · Playback 2x`
- `Reconnecting to <name>…`
- `Desktop unavailable`

On disconnect, retain the last values briefly but visibly mark them stale. Clear
them after five seconds. Do not switch to the phone's UDP listener without an
explicit user action.

### 5.3 Reconnect

A saved desktop is identified by its cryptographic identity, not by its IP
address. Android discovers the current address with DNS-SD, tries remembered
addresses as a fallback, authenticates with its device credential, and requests
a new snapshot. Address changes therefore do not require re-pairing.

Use exponential retry with jitter while the paired page/dashboard is active:
approximately 0.5, 1, 2, 4, then 8 seconds, capped at 8 seconds. A user-requested
disconnect stops retries. Authentication failure stops retries and asks the
user to pair again.

## 6. Connection options

QR and matching codes are pairing methods, not transports. They can bootstrap
several transports. The practical options are:

| Option | Works without router | Desktop portability | Telemetry fit | Decision |
|---|---:|---:|---:|---|
| Same-LAN TCP/WebSocket + DNS-SD | Via phone/PC hotspot | Strong | Strong | **Use for v1** |
| Direct IP from QR/manual entry | Via hotspot | Strong | Strong | **Required fallback** |
| Wi-Fi Direct | Yes | Weak to mixed | Strong after connection | Defer |
| Wi-Fi Aware | Yes | Poor; mainly mobile APIs | Strong | Do not use for desktop v1 |
| Bluetooth Classic/RFCOMM | Yes | Mixed; native OS work | Adequate | Optional future fallback |
| Bluetooth LE GATT/L2CAP | Yes | Mixed; permission-heavy | Possible for a reduced dashboard, awkward for the full stream | Discovery/bootstrap only if ever added |
| USB tethering | Yes | Strong as an IP network | Strong | Support implicitly through the LAN transport |
| Custom USB accessory protocol | Yes | Weak and hardware/driver-specific | Strong | Do not build |
| Internet relay | Yes, with internet | Strong | Strong but adds service cost/latency | Future remote mode |
| WebRTC data channel | Yes, with signaling; TURN may be needed | Strong | Strong | Future remote mode, not local v1 |
| Raw decoded UDP multicast/unicast | Same network only | Strong | Low overhead, but lossy and unauthenticated | Do not use as the primary paired transport |
| Google Nearby Connections | Yes | No general Electron/Windows/macOS/Linux endpoint | Strong on supported mobile platforms | Not suitable here |
| NFC | Only for very short range bootstrap | Most desktops lack it | Not a data pipe | No |

### 6.1 Why LAN IP is the default

Android's Network Service Discovery API is DNS-SD over mDNS and is specifically
intended to find services on the local network. The transport underneath it is
ordinary IP, so the same implementation also works over Ethernet-to-Wi-Fi, a
phone hotspot, a PC hotspot, and USB tethering. QR and manual endpoint entry
keep pairing usable on guest networks that block multicast.

This gives the best combination of throughput, latency, cross-platform desktop
support, and debuggability. It also avoids Bluetooth runtime permissions and
manufacturer-specific peer-to-peer behavior.

### 6.2 Why Bluetooth is not v1

Bluetooth Classic can carry this dashboard, but Electron has no portable
first-party RFCOMM server API. Each desktop OS would need native discovery,
pairing, permissions, adapter-state, and reconnect handling. BLE adds MTU and
notification fragmentation plus the same desktop portability work. Android 12+
also requires runtime Nearby Devices permissions for scan/connect operations.

Bluetooth remains reasonable only if field testing proves that users commonly
lack a usable Wi-Fi/hotspot path. If added, it should carry the same Pair
Protocol frames through a transport adapter rather than define another data
schema.

### 6.3 Why Wi-Fi Direct and Nearby Connections are not v1

Wi-Fi Direct provides a fast link without an access point, but the Android API
is only one half of the product. Windows, macOS, and Linux have different
support and user flows. Google Nearby Connections abstracts Bluetooth and
Wi-Fi well on its supported mobile SDKs, but it does not provide the general
desktop endpoint needed by this Electron application.

The user can get the same router-free result in v1 by enabling a phone or PC
hotspot and using the normal LAN path.

## 7. Recommended architecture

```text
                                  +-> Electron renderer IPC
Game UDP -> Parser -> Engine Sink -|
TNRD ----> Reader -> Engine Sink   +-> Pair service -> secure socket -> Android
                                                        |
                                                        +-> same dashboard store

Android Direct UDP -> native Engine -> same dashboard store
```

### 7.1 Desktop pair service

Create a main-process `PairService` that:

- owns discovery, pairing windows, saved-device authentication, sockets,
  per-client subscriptions, and bounded outgoing queues;
- receives engine JSON and binary callbacks before renderer visibility gating;
- is independent of `BrowserWindow` visibility and renderer seek acknowledgement;
- filters the union engine stream into each peer's subscription;
- replaces a peer's requirements when its visible Android page changes and
  routes any optional backfill by request ID;
- forwards every subscribed hot batch immediately and disconnects a peer if its
  bounded socket backlog exceeds the hard limit;
- forwards the desktop engine's authoritative playback state without creating
  a second clock;
- exposes status and device-management IPC to the desktop Settings UI;
- never performs JSON parsing or socket writes on the engine callback thread.

The service should stay alive while the Electron main process is alive. Closing
or hiding the window must not disconnect a phone if the app is configured to
remain in the tray. Exiting the app closes the service cleanly.

### 7.2 Shared library responsibilities

It is appropriate to add pairing support to `protocol_parser_library`, but the
library boundary should remain narrow:

- define the versioned Pair Protocol envelope and capability structures;
- provide row-mask helpers and binary-frame validation;
- reuse `tnrp::bin::decodeBatch` rather than creating an Android-specific
  fourth binary decoder;
- provide an atomic current-state snapshot API for a row mask;
- expose explicit timeline-reset metadata for playback load/seek/close and live
  rewind/session replacement;
- optionally implement the transport-independent pairing/authentication state
  machine if an audited cryptographic implementation is shared by desktop and
  Android.

The parser, UDP listener, TNRD reader, and writer should not know about QR,
mDNS, Android device names, sockets, or Electron settings. Platform hosts own
network discovery, credential storage, QR UI, and firewall messaging.

### 7.3 Consumer requirements

Paired mode exposes a limitation in today's single aggregate
`Engine::setDataRequirements` contract. Replace or wrap it with consumer-scoped
requirements:

```text
renderer requirements --+
phone A requirements ----+--> union in Engine/parser --> shared Sink stream
phone B requirements ----+                         |--> host filters per consumer
```

Each consumer has a stable ID, subscription generation, stream row mask,
history row mask, and explicit backfill policy. Removing a consumer or changing
pages recomputes the union. Recording remains complete regardless of the union,
as it is today.

The union controls which decoded row families the engine materializes and
forwards; the desktop host still filters the union per destination. A phone must
not receive rows merely because the Electron page or another phone needs them.
Likewise, hiding or navigating the Electron renderer must not remove rows still
needed by Android.

These masks describe Track N Race row families, not F1 UDP packet IDs. The UDP
listener still receives the game's datagrams, while the parser avoids producing
unneeded decoded outputs where its packet-to-row mapping permits. Recording
continues to parse and store the complete recording input.

The current race dashboard has this exact page requirement:

| Requirement | Value | Reason |
|---|---:|---|
| Stream rows | `telemetry`, `status`, `lap`, `session` | Every field consumed by the Canvas dashboard state comes from these four families |
| Stream mask | `54` (`2 | 4 | 16 | 32`) | Shared logical row-family bit assignments |
| History rows | none | The dashboard retains latest values only |
| History mask | `0` | Prevents live/playback backfill |
| Backfill policy | `none` | No current-lap, finite-window, or all-session history |
| Mandatory control | `protocol_status`, `playback_state`, `timeline_reset`, connection/error state | Format labels, authoritative clock, and lifecycle correctness |

An atomic current snapshot is not backfill. On attach, reconnect, page entry,
playback load, or seek, the race dashboard receives at most one latest record
from each of its four stream families plus the mandatory control state. It does
not receive rows leading up to that point.

### 7.4 Android source abstraction

Refactor the activity-facing input into a small source interface:

```text
TelemetrySource
  start(listener)
  stop()
  state()

DirectUdpSource       -> current NativeTelemetry/Engine JNI path
PairedDesktopSource   -> discovery + secure connection + Pair Protocol
```

Both sources publish packed hot rows, JSON control rows, and connection state to
the Capacitor `Telemetry` plugin. The plugin owns source switching. Hot batches
from either source use one direct WebView `ArrayBuffer` transport and Electron's
existing TypeScript decoder; cold rows use ordinary Capacitor events. The Java
layer does not hand-copy or decode the binary layout.

### 7.5 Android page-requirement registry

Add one Android-owned registry analogous to Electron's
`historyDependencies.ts`. It maps a stable page ID to the complete requirement
for that page; activities, views, and cards do not send independent network
subscriptions.

```text
AndroidPageRequirement
  pageId
  streamMask
  historyMask
  backfill = none | finite-window | current-lap | all-session
  windowSeconds?                 // finite-window only
```

Initial entry:

```text
race-dashboard
  streamMask  = telemetry | status | lap | session   // 54
  historyMask = 0
  backfill    = none
```

Future pages may opt into a range only for the families their visible charts
actually consume. A page with cards and charts should union only its visible
sections, just as Electron does. Android must not prefetch rows for hidden tabs,
placeholder pages, off-screen sections, or likely next pages.

On page change, Android sends one replacement subscription with a new request
ID. The desktop atomically replaces that phone's old requirement, acknowledges
the accepted masks/policy, and sends current state for newly enabled families.
Any in-flight backfill belonging to an older request ID is cancelled or dropped.
The old and new page requirements are never accumulated indefinitely.

## 8. Pairing and authentication

### 8.1 Common rules

- Pairing is possible only while a user-opened two-minute pairing window is
  active on the desktop.
- Discovery records contain only protocol version, an opaque server ID, port,
  and availability flags. They contain no permanent bearer credential.
- Every pairing attempt is rate-limited by address and server ID.
- Successful pairing creates a random per-phone credential. The QR secret or
  matching code is discarded immediately and cannot reconnect later.
- Android stores its credential in Android Keystore-backed encrypted storage.
  Electron stores server identity and per-device credentials through
  `safeStorage`; it must not place plaintext secrets in `electron-store`.
- Device revocation deletes that device credential and disconnects it.
- Pairing secrets, credentials, QR payloads, and complete discovery TXT records
  must never be logged.

### 8.2 QR flow

1. Desktop opens a pairing window and generates a 128-bit or stronger one-time
   secret.
2. Desktop displays a QR payload containing Pair Protocol version, opaque server
   ID, current endpoint candidates, expiry, one-time secret, and the desktop
   identity-key/certificate fingerprint.
3. Android scans and validates the payload, rejects unknown versions or expired
   data, and connects to one advertised endpoint.
4. Android pins the fingerprint, proves possession of the one-time secret over
   the encrypted channel, and sends its proposed device name and fresh client
   identity.
5. Desktop shows the phone name and asks the user to allow it.
6. Desktop issues a per-device reconnect credential; both sides persist the
   association and erase the one-time secret.
7. Android authenticates the new session, subscribes, receives an atomic
   snapshot, and enters `Connected`.

Suggested URI shape, with compact binary values Base64URL encoded:

```text
tnrpair://v1/<server-id>?p=<port>&e=<expiry>&k=<one-time-secret>&fp=<fingerprint>&a=<endpoints>
```

The parser must impose total and per-field size limits before allocating.

For Android, Google Code Scanner is a reasonable default because the current
minimum SDK is 26 and the API supplies a system scanning UI without a camera
permission. It depends on Google Play services and an on-demand module, so the
matching-code flow must remain a complete fallback. A Play-services-free build
can instead use a bundled scanner with an explicit camera permission.

### 8.3 Same-code flow

The same-code flow cannot safely be implemented by sending a six/eight-character
code as a password over an unauthenticated socket. Use a reviewed
password-authenticated key exchange (PAKE), such as a compliant SPAKE2
implementation, and bind the following into its transcript:

- Pair Protocol version;
- desktop and phone roles;
- both ephemeral identities/nonces;
- discovered server ID;
- endpoint and desktop identity fingerprint.

Flow:

1. The user opens `Use matching code` on both apps and enters the same random or
   user-chosen eight-character code.
2. Desktop advertises a pairing-capable DNS-SD service. Android discovers all
   candidates and begins the PAKE with each candidate using an opaque attempt ID.
3. Only the desktop with the matching code derives the same high-entropy key and
   completes key confirmation. The code is never sent over the network.
4. Both apps display the peer name; the desktop user approves the phone.
5. The PAKE key authenticates the desktop identity and the encrypted credential
   enrollment. Both apps erase the short code.

Use a vetted PAKE implementation; do not implement elliptic-curve or transcript
cryptography from scratch. Limit attempts (for example five per pairing window),
add a delay after failure, and expire the window. These controls are still
required because short human codes permit online guessing even when PAKE blocks
passive and offline attacks.

If a vetted cross-platform PAKE cannot be accepted as a dependency, ship QR
pairing plus manual host/port entry first. Do not downgrade the matching-code
flow to unauthenticated plaintext.

## 9. Discovery and transport

### 9.1 Discovery

Advertise `_tracknrace-pair._tcp.local` with DNS-SD/mDNS. Android uses
`NsdManager` and stops discovery when the page is no longer active. Use the
older discovery/resolve path on API levels where the newer combined callback is
unavailable.

DNS-SD is a hint, not identity proof. A hostile peer can advertise the same
service type; authentication decides whether the discovered service is the
saved desktop.

The QR payload and advanced endpoint entry bypass discovery. This is essential
on guest Wi-Fi, enterprise networks, VPN configurations, and access points with
multicast or client-to-client traffic disabled.

### 9.2 Data transport

Use a persistent secure WebSocket (`wss`) with subprotocol
`tnr.telemetry.v1`, or an equivalent TLS stream with the exact same message
framing. WSS is preferred because it provides message boundaries, ping/pong,
widely tested clients, and a clean path to a future relay.

The desktop uses a local identity certificate/key. A QR-paired Android client
pins its fingerprint; a matching-code client authenticates that fingerprint in
the PAKE transcript. Saved clients then use the pinned identity plus their
per-device reconnect credential. Do not rely on a publicly trusted DNS
certificate for `.local` addresses, and do not allow an accept-all Android
`TrustManager`.

Bind on IPv4 and IPv6 local interfaces. Do not enable UPnP/NAT-PMP or expose the
port intentionally to the public internet. On Windows, request a firewall rule
for private networks only and show actionable UI if the port is blocked.

### 9.3 Android permissions

The current app's normal `INTERNET` permission is enough for ordinary sockets at
its present target SDK. Add `ACCESS_NETWORK_STATE` for reachability-aware
discovery/reconnect behavior. No Bluetooth or location permission is needed for
the recommended path.

Plan now for `ACCESS_LOCAL_NETWORK`: Android 17 requires this runtime permission
for apps targeting API 37+ that access LAN devices. Keep permission prompting
inside the paired-mode onboarding so direct UDP users are not asked before they
need it.

## 10. Pair Protocol v1

The Pair Protocol is independent of:

- F1 packet format (`2024`, `2025`, `2026`, ...);
- TNRD container version (`V1` ... `V5`);
- Android application version;
- the internal Electron IPC channel names.

### 10.1 Handshake

After transport authentication:

```json
{"type":"hello","pairProtocol":1,"appVersion":"1.4.1","binaryRowsVersion":2,"deviceId":"...","capabilities":["latest-state","selective-backfill"]}
{"type":"welcome","pairProtocol":1,"binaryRowsVersion":2,"serverId":"...","source":"live","protocolYear":2026,"formula":13,"generation":42,"capabilities":["subscribe","snapshot","selective-backfill","playback-state"]}
{"type":"subscribe","pageId":"race-dashboard","streamMask":54,"historyMask":0,"backfill":{"mode":"none"},"requestId":1}
{"type":"subscription_ack","pageId":"race-dashboard","streamMask":54,"historyMask":0,"backfill":{"mode":"none"},"requestId":1}
```

Both sides reject an unsupported major version with a small structured error.
Minor additive capabilities are negotiated by name. Never infer binary layout
compatibility from app version.

The desktop includes its latest effective protocol year and raw Session
`m_formula` in `welcome`, so Android can select year/formula-dependent UI before
the first post-connect telemetry batch. Both values are nullable until packets
or playback metadata identify them; later `protocol_status` rows keep them
current.

### 10.2 Frames

Use WebSocket text frames for small control messages and binary frames for data:

| Frame | Payload | Use |
|---|---|---|
| `control` | bounded UTF-8 JSON | hello, subscribe/ack, authoritative source/playback state, error, heartbeat |
| `rows` | newline-delimited canonical row JSON | cold/state/event rows |
| `hot` | existing `BinaryRows.h` batch | telemetry, motion, positions, motion_ex |
| `snapshot` | generation + JSON rows + latest packed hot records | initial/reconnect/paused state |
| `timeline_reset` | generation, reason, target time/lap | load, seek, close, flashback, new session |

Every data frame carries a monotonically increasing `generation` and `sequence`.
The receiver discards older generations, duplicate sequence numbers, and any
ordinary stream rows received before the snapshot for a new generation is
installed.

Set explicit limits before decoding: maximum frame size, maximum JSON row size,
maximum rows per batch, maximum position count, known binary tag/record lengths,
and valid mask bits. A malformed binary record closes the connection with a
protocol error because its layout cannot be resynchronized safely.

### 10.3 Snapshot API

Add an engine-level atomic snapshot operation that returns, for a row mask:

- the latest packed hot record per requested hot family;
- the latest cold/state row per requested family;
- `protocol_status` and source mode;
- playback state when a clip is loaded;
- current session time, lap number, and timeline generation.

The snapshot must be internally consistent with one engine cursor. It must not
be reconstructed from whichever rows happened to reach Electron main. This is
especially important when a phone connects while playback is paused and no new
hot row will naturally arrive.

The snapshot is filtered by the active page's stream mask and contains only the
latest state. It never implies or triggers historical backfill. For the race
dashboard, it contains no more than the latest `telemetry`, `status`, `lap`, and
`session` rows plus mandatory control state.

### 10.4 Selective subscription and backfill

The `subscribe` message replaces, rather than adds to, the phone's prior page
requirement. The server validates that `historyMask` is a subset of
`streamMask`, applies the newest request ID, and returns `subscription_ack` with
the accepted values. An unsupported page ID is not an error if its explicit
masks and policy are valid; page IDs exist for diagnostics and test fixtures,
not as the protocol's source of truth.

Backfill modes are explicit on the wire:

| Mode | Meaning |
|---|---|
| `none` | Current snapshot and subsequent rows only |
| `finite-window` | Requested historical families for the last `windowSeconds` |
| `current-lap` | Requested historical families from current lap start to the authoritative cursor |
| `all-session` | Requested historical families from recording/session start to the authoritative cursor |

`race-dashboard` always uses `none` and `historyMask: 0`, in live and playback.
Future pages can request one of the other modes without changing Pair Protocol
framing. Backfill responses carry the subscription request ID and timeline
generation, are additive, and never move the desktop playback cursor. The host
routes an engine history result only to the requesting phone; it is not
broadcast to Electron or other phones simply because they share the engine
sink.

A newer page subscription cancels or invalidates the older page's backfill.
Normal stream rows may be buffered behind a short installation barrier so the
history and current snapshot become visible atomically, but every buffer remains
bounded.

### 10.5 Delivery and backpressure

Each phone receives engine batches in order through its WebSocket. Hot frames
are neither delayed to a presentation cadence nor replaced with a latest value.
If the socket's pending bytes exceed 8 MiB, the service closes that peer with a
slow-client error instead of allowing unbounded memory or silently discarding
individual samples. Lifecycle, warning, and race-event messages remain ordered
on the same connection.

A five-second ping and fifteen-second liveness timeout are suitable starting
values. Measure them on real race Wi-Fi before freezing them as protocol
constants.

## 11. Live behavior

On connection in live mode:

1. authenticate;
2. negotiate capabilities;
3. register the currently displayed Android page's consumer requirements;
4. send `timeline_reset(reason=attach)` and an atomic snapshot;
5. release subsequent live rows for that generation.

The pair service consumes callbacks before Electron's `rendererVisible` check.
Minimizing the desktop window therefore does not freeze Android. Recording is
also unaffected.

A game flashback or detected session-time regression starts a new generation,
clears Android's current state, installs the engine snapshot at the rewound
time, and resumes. A new session identity does the same with
`reason=new_session`.

## 12. Playback behavior

Desktop playback remains the single owner of file loading, play/pause, speed,
seek, close, and time. Specifically, the desktop `libtnrp::Engine` advances
`currentTime_` and emits `playback_state`; Electron main forwards that state to
the pair service. Android is a presentation client, not a second player.

### 12.1 Load and play

When a recording loads, send a new-generation reset followed by the same
current-state snapshot that the engine exposes to local consumers. Forward
`playback_state` independently of telemetry cadence so Android immediately
shows paused/playing state and speed.

Normal playback rows then use the same `rows` and `hot` frames as live data.
Android does not need to know whether they came from `Parser` or `TnrdReader`.

### 12.2 Authoritative playback timing

While playing, the engine already emits `playback_state` from its playback loop.
The pair service forwards authoritative playback state as it arrives. It
forwards transitions—load, play, pause, speed change, seek completion, finish,
and close—immediately without waiting for a scheduled presentation update.

The state carries `current_time`, `total_time`, `speed`, `playing`, and
`start_time`. Android may calculate the absolute recording cursor as
`start_time + current_time`, but it must not:

- add elapsed Android wall-clock time between messages;
- advance time using `speed`;
- derive the playhead from the newest telemetry row's `session_time`;
- compensate the displayed cursor using measured network latency.

Android's display-refresh draw loop may repaint the last received value; it does not
create new playback time. If playback-state delivery stalls, the visible clock
freezes and the connection becomes stale instead of drifting away from the
desktop. Pause and seek therefore show the exact desktop cursor, and reconnect
starts from the cursor in the new atomic snapshot.

### 12.3 Seek

A desktop seek must be atomic for every phone, independently of Electron's
renderer barrier:

1. increment the remote generation and stop forwarding old-generation rows;
2. buffer bounded rows produced while the engine applies the seek;
3. when the authoritative engine seek flush commits, create a latest-state
   snapshot for Android's subscribed families;
4. send `timeline_reset(reason=seek, target=...)` and the snapshot;
5. if the active page requested backfill, send only its requested families and
   range; the race dashboard skips this step;
6. release buffered new-cursor rows tagged with the new generation.

Superseded seek request IDs are discarded. A phone must never briefly combine
pre-seek speed/lap state with post-seek status.

### 12.4 Pause, finish, and close

- Pause changes source state but retains the snapshot.
- A phone connecting while paused receives that snapshot immediately.
- Playback finish sends the final playback state and retains final values.
- Closing playback starts a new live generation, clears playback-only state,
  restores live `protocol_status`, and waits for/installs the current live
  snapshot.

## 13. Security model

Threats in scope include another device on the LAN reading telemetry, guessing a
short code, replaying an old credential, impersonating a saved desktop through
mDNS, sending malformed frames, or flooding queues.

Required controls:

- encrypted transport with desktop identity pinning;
- one-time high-entropy QR secrets;
- PAKE plus online-attempt limits for matching codes;
- per-device random reconnect credentials and explicit revocation;
- transcript/version/role binding to prevent downgrade and unknown-key-share
  mistakes;
- expiry and single-use enforcement for pairing attempts;
- constant-time credential comparison in the authentication layer;
- no secrets in logs, crash reports, URLs opened by an external browser, or
  DNS-SD TXT records;
- strict frame, rate, string, array, and queue limits;
- no unauthenticated playback-control messages;
- credentials rotated by `Forget all devices` or desktop identity reset.

Telemetry is not as sensitive as account data, but pairing without these
controls would create a silent LAN service and make later remote-control
features unsafe by construction.

## 14. Failure behavior

| Condition | User-visible result | Recovery |
|---|---|---|
| mDNS blocked | Desktop not found | QR endpoint, manual host/port, or hotspot |
| Client isolation/firewall | Found but unreachable | Explain private-network/firewall requirement |
| QR expired | Pairing failed | Generate a new QR |
| Code mismatch/rate limit | Pairing failed without revealing candidate details | Retry after delay/new window |
| Saved credential revoked | Re-pair required | Remove stale Android association |
| Wi-Fi changes | Reconnecting | Rediscover by server identity |
| Protocol major mismatch | Update required | Show versions on both devices |
| Malformed/oversized frame | Connection closed | Log sanitized protocol reason |
| Phone too slow | Connection closes when its 8 MiB pending-byte cap is reached | Reconnect and snapshot |
| Desktop playback seek | Brief `Seeking…` state | Atomic reset and snapshot |
| Desktop exits | `Desktop unavailable` | Retry only while paired mode is active |

## 15. Implementation map

Likely code areas, without prescribing final filenames:

- `protocol_parser_library/`
  - Pair Protocol structs/framing validation;
  - consumer-scoped stream/history requirements or a compatible aggregate API;
  - atomic latest-state snapshot;
  - timeline generation/reset signals and authoritative playback state;
  - shared binary decode and optional PAKE adapter.
- `electron-frontend/node_addon/addon.cpp`
  - expose snapshot and consumer registration;
  - surface timeline resets without blocking TSFN callbacks.
- `electron-frontend/src/main/bridgeManager.ts`
  - feed `PairService` before renderer visibility/seek gating;
  - keep renderer and pair-client requirements independent.
- `electron-frontend/src/main/`
  - PairService, DNS-SD, secure server, credential storage, per-peer filtering,
    page-subscription replacement, backfill routing, bounded backpressure, and
    IPC.
- `electron-frontend/src/preload/index.ts` and renderer Settings
  - narrow device-management APIs and pairing/status UI.
- `android_frontend/src/` and `android_frontend/.../java/com/tracknrace/android/`
  - Capacitor UI, binary runtime, source abstraction, discovery, pair client,
    credentials, subscription lifecycle, snapshot installation, and stale-state
    handling.
- `android_frontend/.../cpp/native_bridge.cpp`
  - forward direct-mode packed rows without JSON conversion or a second engine.
- both apps' attribution/license pages
  - add every shipped QR, WebSocket/TLS, mDNS, or PAKE dependency as required by
    the repository dependency policy.

Do not put network socket logic into the React dashboard state, the parser, or
the TNRD reader. Do not forward Electron IPC objects such as
`playback_seek_flush_bin` directly as the public network protocol.

## 16. Rollout plan

### Phase 1: protocol and snapshots

- Define Pair Protocol v1 fixtures and limits.
- Add consumer-scoped requirements, replaceable page subscriptions, optional
  backfill modes, and atomic engine snapshots.
- Add timeline generations for live reset and playback lifecycle.
- Unit-test binary and JSON compatibility without networking.

### Phase 2: LAN pairing MVP

- Implement desktop service, Android source abstraction, WSS, DNS-SD, QR
  pairing, direct endpoint fallback, saved credentials, and revocation.
- Mirror the current Android race dashboard in live mode with stream mask `54`,
  history mask `0`, and backfill `none`.
- Add matching-code PAKE after the vetted shared implementation is selected.

If matching-code support is a release requirement, Phase 2 is not complete
until PAKE ships; a plaintext code is not an acceptable temporary substitute.

### Phase 3: playback hardening

- Implement load/seek/close generation barriers and paused snapshots.
- Forward the desktop engine's playback state immediately and verify Android
  never advances an independent clock.
- Exercise repeated scrubbing, speed changes, end/replay, and reconnect during
  every playback state.

### Phase 4: expansion

- Multiple simultaneous phones if demanded.
- Authenticated Android playback controls.
- Qt host support using the same protocol fixtures.
- Optional remote relay or Bluetooth adapter only if product evidence justifies
  the maintenance cost.

## 17. Verification plan

### 17.1 Unit and fuzz tests

- QR payload parsing, expiry, version rejection, and size limits;
- PAKE success, mismatch, transcript binding, replay, expiry, and throttling;
- credential enrollment, reconnect, revocation, and rotation;
- Pair Protocol frame truncation, unknown types, oversized lengths, invalid
  UTF-8/JSON, malformed packed rows, and sequence/generation handling;
- consumer requirement union, atomic replacement, removal, and per-peer
  filtering;
- backfill mode validation, request cancellation, and response routing;
- atomic snapshot consistency in live, paused playback, and end-of-playback.

### 17.2 Integration tests

- live dashboard parity between direct UDP and paired mode for the same decoded
  fixture;
- pairing by QR, matching code, remembered identity, and manual endpoint;
- connect before telemetry, mid-session, while desktop renderer is hidden, and
  while Android rotates/recreates its activity;
- switch Android pages rapidly and verify outbound frames contain only the
  newest page's stream families;
- verify the race dashboard receives only `telemetry`, `status`, `lap`,
  `session`, mandatory control state, and no backfill in live or playback;
- enter future backfill-capable test pages and verify only their requested row
  families and ranges are returned;
- playback load, play, pause, all supported speeds, repeated overlapping seeks,
  finish/replay, close to live, and reconnect while paused;
- inject network delay and dropped updates and verify Android freezes at the
  last desktop `playback_state` rather than extrapolating its own cursor;
- live flashback/session-time regression and new-session rotation;
- desktop recording remains complete while the phone is slow or disconnected;
- revocation immediately disconnects the target phone only.

### 17.3 Network matrix

- Windows, macOS, and Linux desktop builds;
- Android API 26, Android 12 permission behavior, current target SDK, and an
  Android 17/API 37 permission test before that target is adopted;
- home router, mesh Wi-Fi, guest/client-isolated Wi-Fi, VPN enabled, phone
  hotspot, PC hotspot, Ethernet desktop plus Wi-Fi phone, and USB tethering;
- IPv4-only, IPv6-capable, IP address change, Wi-Fi roam, suspend/resume, and
  firewall denial.

### 17.4 Performance acceptance

- No socket work or waiting on the engine callback thread.
- No unbounded per-peer memory.
- No measurable regression in Electron chart pacing or TNRD recording.
- Local dashboard latency should normally remain under 100 ms without a fixed
  Android ingestion or presentation-rate gate.
- Race-dashboard bandwidth contains no hidden-page families or history ranges.
- After a seek or reconnect, Android must install one coherent snapshot before
  rendering the new generation.

## 18. Research basis

- Android NSD is based on DNS-SD/mDNS and supports local service discovery:
  [Android `NsdManager`](https://developer.android.com/reference/android/net/nsd/NsdManager)
  and [Use network service discovery](https://developer.android.com/develop/connectivity/wifi/use-nsd).
- Wi-Fi Direct can make a fast connection without an access point, but requires
  its own Android peer/discovery flow:
  [Wi-Fi Direct overview](https://developer.android.com/develop/connectivity/wifi/wifip2p).
- Bluetooth scan/connect requires modern Nearby Devices runtime permissions on
  Android 12+:
  [Bluetooth permissions](https://developer.android.com/develop/connectivity/bluetooth/bt-permissions).
- Companion Device Manager assists discovery but does not create the data
  connection itself:
  [Companion device pairing](https://developer.android.com/develop/connectivity/bluetooth/companion-device-pairing).
- Nearby Connections chooses among Bluetooth and Wi-Fi on its supported
  platforms:
  [Nearby Connections overview](https://developers.google.com/nearby/overview).
- Google Code Scanner provides an Android scanning UI without a camera
  permission and supports this project's minimum SDK:
  [Google code scanner](https://developers.google.com/ml-kit/vision/barcode-scanning/code-scanner).
- Android 17/API 37 introduces required local-network runtime permission for
  apps targeting it:
  [Local network permission](https://developer.android.com/privacy-and-security/local-network-permission).
- WSS supplies WebSocket confidentiality and integrity through TLS:
  [RFC 6455](https://www.rfc-editor.org/rfc/rfc6455.html).
- DNS-SD service instances use SRV/TXT records and can operate over mDNS:
  [RFC 6763](https://www.rfc-editor.org/rfc/rfc6763.html).
- SPAKE2 derives a strong shared key from a shared password without disclosing
  that password:
  [RFC 9382](https://www.rfc-editor.org/rfc/rfc9382.html).
