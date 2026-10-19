#!/usr/bin/env python3
"""Uploads dist/ (from make_dist.py) to the Worker's R2 bucket through its /_upload/ endpoint.

    python3 recomp/web/cloudflare/upload.py https://<id>-web.<you>.workers.dev  [--dist dist]

Needs the UPLOAD_TOKEN secret in the environment (PORT_UPLOAD_TOKEN). Game files already in the bucket
with the same size are skipped, so re-running resumes; the page, the build and game/manifest.json are
always sent, because a rebuild can change them without changing their size. Large files go up in parts
(Workers accept at most 100 MB per request).
"""
import argparse
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request

PART = 48 * 1024 * 1024
SINGLE_MAX = 64 * 1024 * 1024


def call(base, token, key, method="GET", query=None, body=None):
    url = f"{base}/_upload/{urllib.parse.quote(key)}" + (("?" + urllib.parse.urlencode(query)) if query else "")
    req = urllib.request.Request(url, data=body, method=method, headers={"X-Upload-Token": token,
                                 "User-Agent": "port-upload/1"})   # Cloudflare blocks Python-urllib (error 1010)
    with urllib.request.urlopen(req, timeout=600) as r:
        return json.loads(r.read() or b"null")


def remote_size(base, token, key):
    try:
        return call(base, token, key, query={"head": "1"})["size"]
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return -1
        raise


def upload_file(base, token, key, path):
    size = os.path.getsize(path)
    if size <= SINGLE_MAX:
        with open(path, "rb") as fh:
            call(base, token, key, "PUT", body=fh.read())
        return
    upload_id = call(base, token, key, "POST", {"mpu": "create"})["uploadId"]
    parts = []
    with open(path, "rb") as fh:
        n = 0
        while True:
            chunk = fh.read(PART)
            if not chunk:
                break
            n += 1
            for attempt in range(4):
                try:
                    parts.append(call(base, token, key, "PUT", {"uploadId": upload_id, "part": n}, chunk))
                    break
                except (urllib.error.URLError, TimeoutError):
                    if attempt == 3:
                        raise
            print(f"    part {n} ({min(n * PART, size) / 1e6:.0f}/{size / 1e6:.0f} MB)", flush=True)
    call(base, token, key, "POST", {"uploadId": upload_id, "complete": "1"}, json.dumps(parts).encode())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("base", help="the Worker's URL")
    ap.add_argument("--dist", default="dist")
    a = ap.parse_args()
    token = os.environ.get("PORT_UPLOAD_TOKEN")
    if not token:
        sys.exit("set PORT_UPLOAD_TOKEN to the Worker's UPLOAD_TOKEN secret")
    base = a.base.rstrip("/")
    files = []
    for root, _, names in os.walk(a.dist):
        for n in names:
            full = os.path.join(root, n)
            key = os.path.relpath(full, a.dist).replace(os.sep, "/")
            if key in ("_headers",):   # Pages-only file; the Worker sets headers itself
                continue
            files.append((key, full))
    files.sort(key=lambda kf: os.path.getsize(kf[1]))
    total = sum(os.path.getsize(f) for _, f in files)
    done = 0
    for key, full in files:
        size = os.path.getsize(full)
        immutable = key.startswith("game/") and key != "game/manifest.json"   # EA's files never change
        if immutable and remote_size(base, token, key) == size:
            done += size
            continue
        print(f"{key} ({size / 1e6:.1f} MB)", flush=True)
        upload_file(base, token, key, full)
        done += size
        print(f"  {done / 1e6:.0f}/{total / 1e6:.0f} MB uploaded", flush=True)
    print(f"done: {len(files)} files, {total / 1e6:.0f} MB in the bucket")


if __name__ == "__main__":
    main()
