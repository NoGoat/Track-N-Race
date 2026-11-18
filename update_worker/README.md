# Update service

Releases are served from Cloudflare. The R2 bucket `track-n-race-releases`
holds every release's files and its signed `manifest.json`, and serves the
files at `https://downloads.track-n-race.com`. This Worker tells the apps
whether a newer release exists: it reads its channel's pointer, then returns
that release's signed manifest. The apps check the signature themselves
(`protocol_parser_library/src/Updates.cpp`) and download straight from the
bucket's domain.

```
track-n-race-releases/
  1.7.0/Track-N-Race-Qt-Windows.exe, ...   the release files
  1.7.0/manifest.json, manifest.json.sig   the signed manifest
  channels/staging.json                    {"version": "1.7.0"}
  channels/stable.json                     {"version": "1.6.2"}
```

`publish-release.mjs` uploads releases and moves the channel pointers. It
also mirrors the files to the GitHub release, which the website's download
buttons and GitHub's download counts use.

## Environments

| | Worker | URL | Offers |
|---|---|---|---|
| Production | `track-n-race-updates` | `https://api.track-n-race.com` | `channels/stable.json` |
| Staging | `track-n-race-updates-staging` | `https://track-n-race-updates-staging.<account>.workers.dev` | `channels/staging.json` |

`kUpdateServiceUrl` in `protocol_parser_library/src/Updates.cpp` is the
production URL. Apps use staging only when started with `TNR_UPDATE_ENDPOINT`.
Both Workers read the same bucket.

## Free tier

The Workers and R2 free tiers cover this: 10 GB of storage, 10 million reads
and 1 million writes a month, no charge for downloads, and 100,000 Worker
requests a day (one update check per install per day). A release is about
600 MB across all platforms, so delete old version folders from the bucket
(Cloudflare dashboard → R2 → `track-n-race-releases`) to stay under 10 GB.
Keep at least the stable and staging versions.

## One-time setup

```sh
cd update_worker
npm install
npx wrangler login                                   # on every machine that publishes
npx wrangler r2 bucket create track-n-race-releases
npx wrangler r2 bucket domain add track-n-race-releases \
  --domain downloads.track-n-race.com --zone-id <track-n-race.com zone ID>
npx wrangler deploy --env staging   # prints the staging workers.dev URL
npx wrangler deploy                 # attaches api.track-n-race.com
```

The zone ID is on the track-n-race.com overview page in the Cloudflare
dashboard. The production deploy creates the `api` DNS record and certificate
on that zone. It doesn't touch the website Worker. Nothing calls the service
until a build with the new `kUpdateServiceUrl` ships: 1.6.2 and earlier only
check GitHub.

Smoke test (204 means nothing newer, 200 returns the signed manifest):

```sh
curl -i "https://api.track-n-race.com/v1/check?product=qt&os=windows&arch=x64&format=installer&v=1.6.2"
```

## Releasing

1. Build: `./build-release-windows.ps1 1.7.0` (and the Linux/macOS scripts and
   `android_frontend/build-signed-apk` on those machines).
2. On each machine: `node publish-release.mjs publish 1.7.0`. It uploads that
   machine's files to `1.7.0/` in the bucket, merges them into the manifest
   the other machines uploaded, re-signs it and uploads it last. Then it
   mirrors the files to the GitHub release `1.7.0`, creating it as a draft the
   first time (`--no-github` skips this). Installed apps see nothing yet.
3. `node publish-release.mjs promote 1.7.0 staging`. Only the staging Worker
   offers it.
4. Test it from an older build (see below).
5. Go live: `node publish-release.mjs promote 1.7.0 stable`. Production offers
   it within 10 minutes (`POINTER_TTL`). Then publish the GitHub draft
   (`gh release edit 1.7.0 --repo NoGoat/Track-N-Race --draft=false`) and bump
   `VERSION` on the website (its `wrangler.jsonc`). The files and signed
   manifest stay the same, so what you tested is exactly what ships.

To stop offering a broken release, promote the previous version to stable
again (`node publish-release.mjs promote 1.6.2 stable`). Installs already on
the broken version are only offered a newer one, so ship a fixed release.
`BLOCKED_VERSIONS` in `wrangler.toml` or the dashboard stops a version being
offered at all, as an emergency switch that doesn't need the publish key.

## Testing a release before it goes live

You need an installed build older than the release. Build one locally and
don't publish it: `./build-release-windows.ps1 1.6.9`. Only Windows installer
builds and release-signed Android APKs install an update themselves; other
builds open the download page.

Install it, then start it against staging. The override also skips the
once-a-day check limit:

```powershell
$env:TNR_UPDATE_ENDPOINT = "https://track-n-race-updates-staging.<account>.workers.dev"
# Then launch the installed app from this same shell (it's under $env:LOCALAPPDATA\Programs\)
```

Check that the dialog offers 1.6.9 → 1.7.0, that the download verifies and
installs, and that the app restarts on 1.7.0 and doesn't offer the update
again. To check what staging returns without the app:

```sh
curl -i "https://track-n-race-updates-staging.<account>.workers.dev/v1/check?product=qt&os=windows&arch=x64&format=installer&v=1.6.9"
```

The signing key is `~/.track-n-race/update-signing-key.pem` (or
`TNR_UPDATE_SIGNING_KEY`). **Back it up.** Without it, installed apps can't
accept another update. Its public half is `kReleasePublicKey` in `Updates.cpp`.

## Testing locally

`wrangler dev` runs the Worker against a local copy of the bucket, and
`--local` points `publish-release.mjs` at that same copy:

```sh
cd update_worker && npx wrangler dev --port 8790 --var LOCAL_DOWNLOADS:true
# in another shell, from the repo root:
node publish-release.mjs publish 0.9.2 --dir <folder with release files> --local
node publish-release.mjs promote 0.9.2 stable --local
```

`LOCAL_DOWNLOADS` makes the Worker serve the files at
`http://127.0.0.1:8790/files/<version>/<file>`, which is where `--local`
manifests point. Start the app with `TNR_UPDATE_ENDPOINT=http://127.0.0.1:8790`
to use the local Worker. That also skips the once-a-day check limit. Plain-http
download URLs are accepted only for `127.0.0.1` and `localhost`.

The Worker caches the channel pointer and manifests for 10 minutes, including
"no pointer yet". After promoting during a local test, restart `wrangler dev`
after deleting `update_worker/.wrangler/state/v3/cache`.
