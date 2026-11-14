# Update service

A Cloudflare Worker that tells the apps whether a newer release exists. It
returns the newest release's signed `manifest.json`. The apps check the signature
themselves (`protocol_parser_library/src/Updates.cpp`), so the Worker only finds
the right manifest and caches GitHub. Downloads go straight to GitHub.

## Deploy (once)

```sh
cd update_worker
npm install
npx wrangler login
npx wrangler deploy
```

Put the URL that `wrangler deploy` prints in `kUpdateServiceUrl` in
`protocol_parser_library/src/Updates.cpp`. Until then, the apps fall back to
the GitHub release check, which can only open the download page.

To raise GitHub's API rate limit (optional), run
`npx wrangler secret put GITHUB_TOKEN` with a fine-grained token that has no
permissions.

To stop offering a broken release, set `BLOCKED_VERSIONS` (for example `1.7.0`)
in `wrangler.toml` or the dashboard. Installs then get the newest release that
isn't blocked.

## Releasing

1. Build: `./build-release-windows.ps1 1.7.0` (and the Linux/macOS scripts on
   those machines).
2. On each machine: `node publish-release.mjs publish 1.7.0`. The first run
   creates a draft release. Each run uploads that machine's files and re-signs
   the merged manifest.
3. Publish the draft: `gh release edit 1.7.0 --repo NoGoat/Track-N-Race --draft=false`.

Drafts are never offered.

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
