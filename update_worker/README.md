# Update service

A Cloudflare Worker that tells the apps whether a newer release exists. It
returns the newest release's signed `manifest.json`. The apps check the signature
themselves (`protocol_parser_library/src/Updates.cpp`), so the Worker only finds
the right manifest and caches GitHub. Downloads go straight to GitHub.

## Environments

| | Worker | URL | Offers |
|---|---|---|---|
| Production | `track-n-race-updates` | `https://api.track-n-race.com` | published releases |
| Staging | `track-n-race-updates-staging` | `https://track-n-race-updates-staging.<account>.workers.dev` | published releases and prereleases |

`kUpdateServiceUrl` in `protocol_parser_library/src/Updates.cpp` is the
production URL. Apps use staging only when started with `TNR_UPDATE_ENDPOINT`.
Neither environment ever offers drafts.

## Deploy

```sh
cd update_worker
npm install
npx wrangler login
npx wrangler deploy --env staging   # prints the staging workers.dev URL
npx wrangler deploy                 # attaches api.track-n-race.com
```

The production deploy creates the `api` DNS record and certificate on the
track-n-race.com zone. It doesn't touch the website Worker. Nothing calls it
until a build with the new `kUpdateServiceUrl` ships: 1.6.2 and earlier only
check GitHub.

Smoke test (204 means nothing newer, 200 returns the signed manifest):

```sh
curl -i "https://api.track-n-race.com/v1/check?product=qt&os=windows&arch=x64&format=installer&v=1.6.2"
```

To raise GitHub's API rate limit (optional), run
`npx wrangler secret put GITHUB_TOKEN` (and again with `--env staging`) with a
fine-grained token that has no permissions.

To stop offering a broken release, set `BLOCKED_VERSIONS` (for example `1.7.0`)
in `wrangler.toml` or the dashboard. Installs then get the newest release that
isn't blocked.

## Releasing

1. Build: `./build-release-windows.ps1 1.7.0` (and the Linux/macOS scripts on
   those machines).
2. On each machine: `node publish-release.mjs publish 1.7.0`. The first run
   creates a draft release. Each run uploads that machine's files and re-signs
   the merged manifest.
3. Make it a prerelease so only staging offers it:
   `gh release edit 1.7.0 --repo NoGoat/Track-N-Race --draft=false --prerelease`.
   Production, GitHub's "latest" release and the website (`VERSION` in its
   `wrangler.jsonc`) all ignore it, so no installed app sees it yet. The
   release page and tag are public, though.
4. Test it from an older build (see below).
5. Go live: `gh release edit 1.7.0 --repo NoGoat/Track-N-Race --prerelease=false`,
   then bump `VERSION` on the website. The files and signed manifest stay the
   same, so what you tested is exactly what ships. Production picks it up
   within 10 minutes (`RELEASES_TTL`).

## Testing a release before it goes live

You need an installed build older than the release. Build one locally and
don't publish it: `./build-release-windows.ps1 1.6.9`. Only Windows installer
builds install an update themselves; other builds open the download page.

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

```sh
npx wrangler dev --port 8790 --var GITHUB_API:http://127.0.0.1:8789/repos/NoGoat/Track-N-Race
```

`GITHUB_API` points the Worker at a stand-in for the GitHub API:
`node mock-github.mjs <release folder> 8789` serves that folder as a published
0.9.2 release, next to a draft 9.9.9 that must never be offered. Start the app
with `TNR_UPDATE_ENDPOINT=http://127.0.0.1:8790` to use the local Worker. That
also skips the once-a-day check limit. Plain-http download URLs are accepted
only for `127.0.0.1` and `localhost`.
