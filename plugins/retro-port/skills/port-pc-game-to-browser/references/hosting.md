# Private hosting (and online rooms) on Cloudflare

Contents: when it's needed · what gets deployed · first deploy · publishing a build · local testing ·
TURN · temporary sharing · rules

## When it's needed

`./play.sh` is enough for playing alone on one computer. Hosting is for playing on other devices
(a phone, another laptop) and for online play between two homes: the page must come over HTTPS, and
the netplay rooms run in the Worker. Everything here goes on **the user's own Cloudflare account**
(the free plan covers it: Workers, R2 storage, Durable Objects) and is deployed only with their
approval.

## What gets deployed

`recomp/web/cloudflare/`:
- `worker.js`: serves the page, build and game files from an R2 bucket with the cross-origin headers,
  range requests (resumable downloads) and **HTTP basic auth** (any user name, the `PASSWORD` secret);
  `/_upload/` (with the `UPLOAD_TOKEN` secret) for `upload.py`; `/_netplay_ticket` and `/netplay/<code>`
  (a Durable Object per room) for online play.
- `wrangler.toml`: Worker and bucket named `<id>-web` (from `port.json`), the R2 binding and the
  `NetplayRoom` Durable Object migration.
- `upload.py`: uploads `dist/` in parts; skips game files already there at the same size and always
  re-sends the page, build and manifest (a rebuild can keep a file's size).

## First deploy

The user logs in once (it opens their browser; they click Allow):

```bash
cd recomp/web/cloudflare
npx wrangler login
npx wrangler r2 bucket create <id>-web
npx wrangler deploy
npx wrangler secret put PASSWORD          # the user types the site password; you never see it
openssl rand -hex 24 > .live-upload-token && npx wrangler secret put UPLOAD_TOKEN < .live-upload-token
```

`.live-upload-token` is git-ignored. Don't echo secrets into the conversation or into files outside the
port. If the user already has a deployed Worker and lost the upload token, generate and set a new one
the same way (Cloudflare never shows secrets again).

## Publishing a build

```bash
python3 recomp/web/make_dist.py --game game
PORT_UPLOAD_TOKEN=$(cat recomp/web/cloudflare/.live-upload-token) \
  python3 recomp/web/cloudflare/upload.py https://<id>-web.<account>.workers.dev --dist dist
```

The first upload sends the whole game (often 1 GB+); later ones only the page and build (~40 MB).
Verify without the password: the page returns 401, `/netplay/X` without a ticket returns 403.
Each player's browser downloads the game once on first visit.

## Local testing of the Worker

```bash
printf 'PASSWORD=%s\nUPLOAD_TOKEN=%s\n' "$(openssl rand -hex 8)" "$(openssl rand -hex 24)" > recomp/web/cloudflare/.dev.vars
npx wrangler dev --config recomp/web/cloudflare/wrangler.toml --ip 127.0.0.1 --port 8799
PORT_UPLOAD_TOKEN=$(sed -n 's/^UPLOAD_TOKEN=//p' recomp/web/cloudflare/.dev.vars) python3 recomp/web/cloudflare/upload.py http://127.0.0.1:8799 --dist dist
```

`wrangler dev` runs the same Worker with local storage and rooms (in `.wrangler/`, git-ignored: it will
hold a copy of the game). Two tabs on http://127.0.0.1:8799 can then play each other.

## TURN (optional)

Without TURN, two players whose networks block a direct WebRTC connection play through the room, with
more delay. With Cloudflare Realtime TURN credentials (`TURN_KEY_ID`, `TURN_KEY_API_TOKEN` as Worker
secrets), they're relayed through the data centres nearest each of them instead (measured: ~12 ms round
trip through a local data centre against ~160 ms through a distant room). TURN may cost money beyond the
free tier: let the user decide.

## Temporary sharing without deploying

A Cloudflare quick tunnel (`cloudflared tunnel --url http://127.0.0.1:8799`) gives the local Worker a real
HTTPS URL for as long as it runs, so a second laptop can join without any setup. It's reachable from
the internet while it runs, so only with the Worker's password on, and shut it down afterwards.

## Rules

- Never deploy without a password. An unguessable URL is not access control.
- The hosted copy is for the user and people who own the game; don't help make it public.
- Ask before each deploy, upload or tunnel: they publish something on the user's account.
