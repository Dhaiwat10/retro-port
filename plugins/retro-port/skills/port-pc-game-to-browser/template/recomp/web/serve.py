#!/usr/bin/env python3
"""Local server for the browser build (localhost only).

Threads in WebAssembly need SharedArrayBuffer, which browsers only enable on cross-origin isolated
pages, so every response carries COOP/COEP headers.

  /            recomp/web (the page)
  /build/      recomp/build_web/www (game.js / .wasm from build_web.sh)
  /game/       with --game DIR: the files the game needs from that folder (see port.json and
               gamefiles.py) plus manifest.json - the same layout make_dist.sh produces for hosting.
               The page downloads them into the browser's storage on first visit.

With --password, every request needs HTTP basic auth (any user name, that password): use it whenever
the server is reachable from outside this machine (e.g. through a tunnel).

Usage: python3 recomp/web/serve.py [--port 8007] [--game game] [--host 127.0.0.1] [--password PW]
"""
import argparse
import base64
import hmac
import http.server
import json
import os
import sys
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from gamefiles import game_files, manifest_for  # noqa: E402
BUILD = os.path.join(os.path.dirname(HERE), "build_web", "www")


class Handler(http.server.SimpleHTTPRequestHandler):
    game = None
    password = None

    def authorized(self):
        if not self.password:
            return True
        auth = self.headers.get("Authorization", "")
        if auth.startswith("Basic "):
            try:
                _, _, pw = base64.b64decode(auth[6:]).decode("utf-8", "replace").partition(":")
                if hmac.compare_digest(pw, self.password):
                    return True
            except ValueError:
                pass
        self.send_response(401)
        self.send_header("WWW-Authenticate", 'Basic realm="Game", charset="UTF-8"')
        self.send_header("Content-Length", "0")
        self.end_headers()
        return False

    def do_HEAD(self):
        if self.authorized():
            super().do_HEAD()
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map, ".wasm": "application/wasm",
                      ".js": "text/javascript"}

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    files = {}   # published path -> source file

    def translate_path(self, path):
        p = urllib.parse.unquote(urllib.parse.urlparse(path).path)
        if p.startswith("/build/"):
            return os.path.join(BUILD, p[len("/build/"):])
        if p.startswith("/game/") and self.game:
            return self.files.get(p[len("/game/"):], "/nonexistent")
        return os.path.join(HERE, p.lstrip("/"))

    def do_GET(self):
        if not self.authorized():
            return
        if urllib.parse.urlparse(self.path).path == "/game/manifest.json" and self.game:
            body = json.dumps(manifest_for(self.files)).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        rng = self.headers.get("Range", "")
        path = self.translate_path(self.path)
        if rng.startswith("bytes=") and os.path.isfile(path):
            self.send_range(path, rng)   # lets the page resume an interrupted download mid-file
            return
        super().do_GET()

    def send_range(self, path, rng):
        size = os.path.getsize(path)
        try:
            first, _, last = rng[len("bytes="):].split(",")[0].partition("-")
            start = int(first) if first else size - int(last)
            end = int(last) if first and last else size - 1
        except ValueError:
            start, end = size, size - 1
        if start >= size or start > end:
            self.send_response(416)
            self.send_header("Content-Range", f"bytes */{size}")
            self.end_headers()
            return
        end = min(end, size - 1)
        self.send_response(206)
        self.send_header("Content-Type", self.guess_type(path))
        self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.send_header("Content-Length", str(end - start + 1))
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()
        with open(path, "rb") as fh:
            fh.seek(start)
            left = end - start + 1
            while left > 0:
                chunk = fh.read(min(left, 1 << 20))
                if not chunk:
                    break
                self.wfile.write(chunk)
                left -= len(chunk)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8007)
    ap.add_argument("--game", help="game folder to serve at /game/ (downloaded by the page on first visit)")
    ap.add_argument("--host", default="127.0.0.1", help="interface to listen on (default: this machine only)")
    ap.add_argument("--password", help="require HTTP basic auth with this password (any user name)")
    a = ap.parse_args()
    Handler.password = a.password
    Handler.game = os.path.abspath(a.game) if a.game else None
    if Handler.game:
        Handler.files = game_files(Handler.game)
    srv = http.server.ThreadingHTTPServer((a.host, a.port), Handler)
    print(f"Game: http://localhost:{a.port}/" + (f"  (serving {len(Handler.files)} game files)" if a.game else ""))
    srv.serve_forever()


if __name__ == "__main__":
    main()
