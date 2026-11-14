# Android self-update: implementation plan

Status: planned, not started (2026-11-14). Electron and Qt already update
themselves on Windows; this brings the same flow to the Android app. Read
`update_worker/README.md` first for how releases, the manifest and the Worker
fit together.

## Before you start (on the new PC)

- Pull the commit with the desktop self-update work. It includes
  `protocol_parser_library/include/tnrp/Updates.h`, `update_worker/`,
  `publish-release.mjs`, and the Android JNI plumbing below.
- Copy two private keys from the old PC. Neither is in git:
  - `~/.track-n-race/update-signing-key.pem`: signs the update manifest. You
    need it to sign the test manifest, because the app only trusts the key
    built into `Updates.cpp`.
  - The Android release keystore named in `android_frontend/keystore.properties`.
    APKs must be signed with it, or one can't install over another.
- An up-to-date Android SDK (compileSdk/targetSdk 37), a phone with USB
  debugging, and Node (for wrangler and the stand-in GitHub server).

## What already exists

| Piece | Where | State |
| --- | --- | --- |
| APK in every GitHub release | `release/<v>/Track-N-Race-Android.apk` from `android_frontend/build-signed-apk.ps1/.sh` | Done. Release key, versionCode `major*10000 + minor*100 + patch` |
| Signed manifest + Worker | `publish-release.mjs`, `update_worker/` | Done. The script doesn't recognise the APK yet (step 1) |
| Version comparison, manifest signature check, file hash check | `tnrp::compareVersions`, `tnrp::evaluateUpdateJson`, `tnrp::verifyUpdateFile` | Done, library-tested |
| Android JNI | `native_bridge.cpp`; `NativeTelemetry.nativeUpdateServiceUrl`, `nativeUpdateCheckPath`, `nativeUpdateEvaluateJson`, `nativeUpdateCompareVersions` (2 = not comparable), `nativeUpdateVerifyFile` | Plumbed, **never compiled**. Build it first |
| Reference implementations | `electron-frontend/src/main/updateChecker.ts`, `qt_frontend/src/UpdateChecker.cpp` | Use them as the model for behaviour and wording |

Everything still to do is Kotlin/Compose UI, the download, and the install.
No new library logic (per the frontend-parity rules).

## Steps

### 1. Teach the manifest about the APK

`publish-release.mjs` `describe()` expects `Track-N-Race-<Product>-<OS>.<ext>`,
so `Track-N-Race-Android.apk` is skipped. Special-case it as
`{ product: 'android', os: 'android', arch: 'universal', format: 'apk' }`.
`build-signed-apk` also writes a `.sha256` file; leave it out of the manifest.
Check: `node publish-release.mjs manifest <v> --dir <folder with the apk>` lists it.

### 2. Compile the existing JNI plumbing

Build the app once before writing any UI, so mistakes in the untested
`nativeUpdate*` functions show up on their own.

### 3. Update check (`AppUpdater.kt`, new, next to `TelemetryController.kt`)

- Run once per app start, from `MainActivity.onCreate` or the controller.
  Network work goes off the main thread (coroutines).
- Respect a setting `updates.enabled` (default on) and a 24 h limit stored in
  the app's SharedPreferences (`RecordingStorage.PREFS`, `track_n_race_android`):
  `updates.lastCheckAt`, `updates.skippedVersion`. Record the attempt before
  the request, as desktop does.
- Request `nativeUpdateServiceUrl() + nativeUpdateCheckPath("android", "android",
  "universal", "apk", BuildConfig.VERSION_NAME)`. An empty service URL (nothing
  deployed yet) means: skip the check, or fall back to the GitHub
  `releases/latest` tag for notify-only, like desktop.
- 204 means up to date. A 200 body goes to `nativeUpdateEvaluateJson(...)`.
  Act only on `status == "available"`, a version newer than the installed one,
  and a version that isn't skipped.
- `BuildConfig` needs `buildFeatures { buildConfig = true }` in
  `app/build.gradle`. Only `compose = true` is set today.

### 4. Who may install (signing-key check)

An APK only installs over the app when both carry the same signing key.
Local builds signed with the debug key (`tnrLocalReleaseSigning`,
`build-and-run`) can't take a release update, and dev builds report `0.1.0`,
so they'd always see an update.

- Add a constant with the release certificate's SHA-256.
  `build-signed-apk` prints it.
- At run time, read the app's own signing certificate
  (`PackageManager.getPackageInfo(..., GET_SIGNING_CERTIFICATES)`), hash it, and
  compare. A match means installable; anything else means notify-only, opening
  the GitHub release page.

### 5. Dialog and setting

- Compose dialog modelled on `UpdateAvailableDialog.tsx`: "Track N Race X is
  available", installed vs latest, **Skip this version / Remind me later /
  Update now** (or **Download** when notify-only). It shows download progress
  and errors in place.
- **Paired mode:** don't interrupt a live paired session. Only show the dialog
  when no paired desktop is connected (paired state is in `TelemetryController`
  / `PairedTelemetryClient`), or hold it until disconnect.
- Settings page (`pages/SettingsPage.kt`): one switch, "Check for updates".
  Keep the copy short.

### 6. Download and verify

- Download to `cacheDir/update/<fileName>` (`.part` first, then rename) with
  `HttpURLConnection` or whatever the app already uses. No storage permission
  is needed.
- Abort if the size goes over the signed `size`. Report progress.
- Then call `nativeUpdateVerifyFile(path, sha512, size)` off the main thread.
  Non-empty means mismatch: delete the file and show the error.
- At startup, delete anything left in `cacheDir/update/`.

### 7. Install with PackageInstaller

- Manifest: `<uses-permission android:name="android.permission.REQUEST_INSTALL_PACKAGES" />`.
- On **Update now**: if `!packageManager.canRequestPackageInstalls()`, send the
  user to `Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES` for this package, then
  continue when they return. Ask only at this point, not in first-time setup.
- Create a `PackageInstaller.Session` (`MODE_FULL_INSTALL`) and stream the
  verified APK into it. On Android 12+ call
  `setRequireUserAction(SessionParams.USER_ACTION_NOT_REQUIRED)`. Commit with a
  `PendingIntent` to a receiver.
- In the receiver, `STATUS_PENDING_USER_ACTION` means start the
  `Intent.EXTRA_INTENT` it carries (the system "Update this app?" screen).
  Treat other failure statuses as errors and show them in the dialog.
- Expect this: the **first** self-update always asks for confirmation, because
  the browser or file manager is recorded as the installer. After that, Track
  N Race is the installer and Android 12+ may install without asking. Android
  decides; both paths must work.

### 8. After the install

Android kills the app and doesn't reopen it. Register a receiver for
`Intent.ACTION_MY_PACKAGE_REPLACED` and post a notification ("Updated to X,
tap to open"). `POST_NOTIFICATIONS` is already declared.

### 9. Debug-only test hooks

- **Endpoint override.** Android has no `TNR_UPDATE_ENDPOINT`. Add a debug-only
  override, for example `adb shell setprop debug.tnr.update_endpoint
  http://127.0.0.1:8790`, read in debug builds only. Use it in place of
  `nativeUpdateServiceUrl()`, and skip the 24 h limit when it's set. Release
  builds ignore it.
- **Plain http.** Android blocks it by default. Add a debug-only
  `network_security_config` that allows cleartext to `127.0.0.1` only. The
  library already accepts http download URLs only for `127.0.0.1`/`localhost`.

### 10. Ledger

Update the "Self-update" row in `docs/frontends_parity.md`: Android → Done,
plus a note on the first-install confirmation behaviour.

## End-to-end test (on the phone)

1. Build two release-signed APKs, `build-signed-apk 0.9.1` and `0.9.2`.
   Because the debug override in step 9 must be in a release-signed build,
   either temporarily enable it for this test or add a `releaseTest` build type
   signed with the release key. Remove that afterwards.
2. Put the 0.9.2 APK in a folder and sign a test manifest:
   `node publish-release.mjs manifest 0.9.2 --dir <folder> --base-url http://127.0.0.1:8789/download`.
3. Start the stand-in GitHub server: `node update_worker/mock-github.mjs <folder> 8789`.
   It lists a draft 9.9.9 (which must never be offered) and a published 0.9.2
   with every file in the folder, and serves `/download/<name>`. Start the Worker with
   `npx wrangler dev --port 8790 --var GITHUB_API:http://127.0.0.1:8789/repos/NoGoat/Track-N-Race`.
   Port 8787 may be taken by the website's dev server.
4. `adb reverse tcp:8790 tcp:8790` and `adb reverse tcp:8789 tcp:8789`, set the
   endpoint override, and `adb install` 0.9.1.
5. Check, with screenshots from the phone:
   - the dialog appears with 0.9.1 → 0.9.2
   - "Install unknown apps" is asked once
   - the system confirmation appears
   - after the update, the app reports 0.9.2 (`adb shell dumpsys package com.nogoat.tracknrace | grep versionName`)
   - the "Updated" notification appears
   - reopening the app shows no update dialog
6. Install 0.9.1 again (`adb install -d`, or uninstall first) and update once
   more. This time Track N Race is the recorded installer, so note whether
   Android skips the confirmation.
7. Tampered APK: serve a copy with one byte flipped and the original manifest.
   The dialog must show the SHA-512 mismatch and stay on 0.9.1.
8. A debug-key build must show **Download**, not **Update now**.
9. Clean up: reinstall your real version, `adb reverse --remove-all`, clear the
   setprop.

## Out of scope

Play Store or F-Droid distribution. Until this ships,
[Obtainium](https://github.com/ImranR98/Obtainium) pointed at the GitHub repo
updates the app with no code.
