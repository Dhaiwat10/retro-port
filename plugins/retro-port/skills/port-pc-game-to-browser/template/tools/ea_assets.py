"""EA asset toolkit (used by EA Sports PC titles of the mid-2000s such as Cricket 07 and Rugby 06): EA BIG archives, RefPack decompression, FSH (SHPI) textures.

Usage:
  ea_assets.py list    <archive.big|.gob>
  ea_assets.py extract <archive> <outdir> [--recursive]   # decompresses RefPack, unpacks nested .big
  ea_assets.py fsh2png <file.fsh|dir> <outdir>             # converts textures to PNG
  ea_assets.py stats   <outdir>                            # tallies extracted files by magic
  ea_assets.py hash    <game/path.ext> [...]                # archive entry name for a game path
"""
import os
import struct
import sys
from collections import Counter

import numpy as np
from PIL import Image


# ---------------------------------------------------------------- BIG archives

def name_hash(path):
    """Archive entry name for a game path, as computed by the game (FUN_006f9530): lowercase, '\\' -> '/',
    MD5 of the path without its extension, as hex, with the original extension appended."""
    import hashlib
    p = path.lower().replace("\\", "/")
    stem, dot, ext = p.rpartition(".")
    if not dot:
        return hashlib.md5(p.encode()).hexdigest()
    return hashlib.md5(stem.encode()).hexdigest() + "." + ext


def read_big_index(data):
    magic = data[:4]
    if magic not in (b"BIGF", b"BIG4"):
        raise ValueError(f"not a BIG archive (magic {magic!r})")
    count = struct.unpack_from(">I", data, 8)[0]
    pos, entries = 16, []
    for _ in range(count):
        off, size = struct.unpack_from(">II", data, pos)
        end = data.index(b"\0", pos + 8)
        entries.append((data[pos + 8:end].decode("latin1"), off, size))
        pos = end + 1
    return entries


# ---------------------------------------------------------------- RefPack

def is_refpack(buf):
    return len(buf) >= 5 and buf[1] == 0xFB and (buf[0] & 0x3E) == 0x10


def refpack_decompress(src):
    flags = src[0]
    size_len = 4 if flags & 0x80 else 3
    pos = 2
    if flags & 0x01:  # compressed size present; skip it
        pos += size_len
    out_size = int.from_bytes(src[pos:pos + size_len], "big")
    pos += size_len
    out = bytearray(out_size)
    o = 0
    n = len(src)
    while pos < n:
        b0 = src[pos]
        if b0 < 0x80:
            b1 = src[pos + 1]; pos += 2
            lit = b0 & 3
            cnt = ((b0 & 0x1C) >> 2) + 3
            dist = ((b0 & 0x60) << 3) + b1 + 1
        elif b0 < 0xC0:
            b1, b2 = src[pos + 1], src[pos + 2]; pos += 3
            lit = b1 >> 6
            cnt = (b0 & 0x3F) + 4
            dist = ((b1 & 0x3F) << 8) + b2 + 1
        elif b0 < 0xE0:
            b1, b2, b3 = src[pos + 1], src[pos + 2], src[pos + 3]; pos += 4
            lit = b0 & 3
            cnt = ((b0 & 0x0C) << 6) + b3 + 5
            dist = ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1
        elif b0 < 0xFC:
            pos += 1
            lit = ((b0 & 0x1F) + 1) * 4
            cnt = 0
            dist = 0
        else:
            pos += 1
            lit = b0 & 3
            out[o:o + lit] = src[pos:pos + lit]
            o += lit
            break
        out[o:o + lit] = src[pos:pos + lit]
        o += lit
        pos += lit
        if cnt:
            start = o - dist
            if dist >= cnt:
                out[o:o + cnt] = out[start:start + cnt]
            else:  # overlapping copy
                for i in range(cnt):
                    out[o + i] = out[start + i]
            o += cnt
    if o != out_size:
        raise ValueError(f"RefPack size mismatch: got {o}, expected {out_size}")
    return bytes(out)


def maybe_decompress(buf):
    return refpack_decompress(buf) if is_refpack(buf) else buf


# ---------------------------------------------------------------- extraction

def extract(archive_path, outdir, recursive=False):
    with open(archive_path, "rb") as f:
        data = f.read()
    return _extract_bytes(data, outdir, recursive)


def _extract_bytes(data, outdir, recursive):
    os.makedirs(outdir, exist_ok=True)
    stats = Counter()
    for name, off, size in read_big_index(data):
        raw = data[off:off + size]
        try:
            buf = maybe_decompress(raw)
        except Exception as e:  # keep going; report the failure
            print(f"  ! {name}: {e}", file=sys.stderr)
            buf, stats["failed"] = raw, stats["failed"] + 1
        stats["refpack" if is_refpack(raw) else "stored"] += 1
        rel = name.replace("\\", "/").lstrip("/")
        dest = os.path.join(outdir, rel)
        if recursive and buf[:4] in (b"BIGF", b"BIG4"):
            stats += _extract_bytes(buf, dest + ".d", True)
            stats["nested_big"] += 1
        os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
        with open(dest, "wb") as f:
            f.write(buf)
        stats["files"] += 1
    return stats


# ---------------------------------------------------------------- FSH textures

def _rgb565(v):
    r = ((v >> 11) & 0x1F) * 255 // 31
    g = ((v >> 5) & 0x3F) * 255 // 63
    b = (v & 0x1F) * 255 // 31
    return r, g, b


def _decode_dxt(data, w, h, kind):
    """kind: 1 = DXT1, 3 = DXT3, 5 = DXT5. Returns RGBA uint8 array."""
    bw, bh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
    bsize = 8 if kind == 1 else 16
    img = np.zeros((bh * 4, bw * 4, 4), np.uint8)
    p = 0
    for by in range(bh):
        for bx in range(bw):
            blk = data[p:p + bsize]; p += bsize
            alpha = np.full(16, 255, np.uint8)
            if kind == 3:
                bits = int.from_bytes(blk[:8], "little")
                alpha = np.array([((bits >> (4 * i)) & 0xF) * 17 for i in range(16)], np.uint8)
                blk = blk[8:]
            elif kind == 5:
                a0, a1 = blk[0], blk[1]
                bits = int.from_bytes(blk[2:8], "little")
                if a0 > a1:
                    pal = [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
                else:
                    pal = [a0, a1] + [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255]
                alpha = np.array([pal[(bits >> (3 * i)) & 7] for i in range(16)], np.uint8)
                blk = blk[8:]
            c0, c1, idx = struct.unpack("<HHI", blk[:8])
            r0, r1 = _rgb565(c0), _rgb565(c1)
            if c0 > c1 or kind != 1:
                cols = [r0 + (255,), r1 + (255,),
                        tuple((2 * a + b) // 3 for a, b in zip(r0, r1)) + (255,),
                        tuple((a + 2 * b) // 3 for a, b in zip(r0, r1)) + (255,)]
            else:
                cols = [r0 + (255,), r1 + (255,),
                        tuple((a + b) // 2 for a, b in zip(r0, r1)) + (255,), (0, 0, 0, 0)]
            px = np.array([cols[(idx >> (2 * i)) & 3] for i in range(16)], np.uint8)
            if kind != 1:
                px[:, 3] = alpha
            img[by * 4:by * 4 + 4, bx * 4:bx * 4 + 4] = px.reshape(4, 4, 4)
    return img[:h, :w]


FSH_TYPES = {0x60: "DXT1", 0x61: "DXT3", 0x62: "DXT5", 0x7D: "ARGB8888", 0x7F: "RGB888",
             0x78: "RGB565", 0x7E: "ARGB1555", 0x6D: "ARGB4444", 0x7B: "P8"}


def _decode_fsh_image(data, off):
    code = data[off] & 0x7F
    w, h = struct.unpack_from("<HH", data, off + 4)
    px = data[off + 16:]
    n = w * h
    if code in (0x60, 0x61, 0x62):
        rgba = _decode_dxt(px, w, h, {0x60: 1, 0x61: 3, 0x62: 5}[code])
    elif code == 0x7D:
        a = np.frombuffer(px[:n * 4], np.uint8).reshape(h, w, 4)
        rgba = a[..., [2, 1, 0, 3]]
    elif code == 0x7F:
        a = np.frombuffer(px[:n * 3], np.uint8).reshape(h, w, 3)
        rgba = np.dstack([a[..., [2, 1, 0]], np.full((h, w), 255, np.uint8)])
    elif code in (0x78, 0x7E, 0x6D):
        v = np.frombuffer(px[:n * 2], "<u2").reshape(h, w).astype(np.uint32)
        if code == 0x78:
            r, g, b, al = (v >> 11 & 31) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31, 255 + 0 * v
        elif code == 0x7E:
            r, g, b, al = (v >> 10 & 31) * 255 // 31, (v >> 5 & 31) * 255 // 31, (v & 31) * 255 // 31, (v >> 15) * 255
        else:
            r, g, b, al = (v >> 8 & 15) * 17, (v >> 4 & 15) * 17, (v & 15) * 17, (v >> 12) * 17
        rgba = np.dstack([r, g, b, al]).astype(np.uint8)
    elif code == 0x7B:
        idx = np.frombuffer(px[:n], np.uint8).reshape(h, w)
        nxt = int.from_bytes(data[off + 1:off + 4], "little")
        pal = None
        if nxt:  # palette is usually the next attachment
            poff = off + nxt
            pcode = data[poff] & 0x7F
            pw = struct.unpack_from("<H", data, poff + 4)[0]
            praw = data[poff + 16:]
            if pcode == 0x2A:
                pal = np.frombuffer(praw[:pw * 4], np.uint8).reshape(pw, 4)[:, [2, 1, 0, 3]]
            elif pcode == 0x24:
                p3 = np.frombuffer(praw[:pw * 3], np.uint8).reshape(pw, 3)[:, [2, 1, 0]]
                pal = np.hstack([p3, np.full((pw, 1), 255, np.uint8)])
        if pal is None:
            pal = np.stack([np.arange(256, dtype=np.uint8)] * 3 + [np.full(256, 255, np.uint8)], 1)
        pal = np.vstack([pal, np.zeros((max(0, 256 - len(pal)), 4), np.uint8)])
        rgba = pal[idx]
    else:
        raise ValueError(f"unsupported FSH pixel format 0x{code:02X}")
    return Image.fromarray(np.ascontiguousarray(rgba), "RGBA"), FSH_TYPES.get(code, hex(code)), (w, h)


def fsh_images(data):
    data = maybe_decompress(data)
    if data[:4] != b"SHPI":
        raise ValueError("not an SHPI file")
    count = struct.unpack_from("<I", data, 8)[0]
    for i in range(count):
        name = data[16 + i * 8:20 + i * 8].decode("latin1").rstrip("\0")
        off = struct.unpack_from("<I", data, 20 + i * 8)[0]
        yield name, off, data


def fsh2png(path, outdir):
    os.makedirs(outdir, exist_ok=True)
    paths = [path] if os.path.isfile(path) else [
        os.path.join(r, f) for r, _, fs in os.walk(path) for f in fs if f.lower().endswith(".fsh")]
    fmt, ok, bad = Counter(), 0, 0
    for p in paths:
        with open(p, "rb") as f:
            data = f.read()
        base = os.path.splitext(os.path.basename(p))[0]
        try:
            for name, off, buf in fsh_images(data):
                try:
                    img, kind, _ = _decode_fsh_image(buf, off)
                    img.save(os.path.join(outdir, f"{base}_{name}.png"))
                    fmt[kind] += 1; ok += 1
                except Exception as e:
                    fmt[str(e)] += 1; bad += 1
        except Exception as e:
            print(f"  ! {p}: {e}", file=sys.stderr); bad += 1
    print(f"converted {ok} images, {bad} failed")
    for k, v in fmt.most_common():
        print(f"  {k}: {v}")


# ---------------------------------------------------------------- stats

MAGICS = {b"SHPI": "fsh texture", b"BIGF": "big archive", b"\x7fELF": "ELF (EAGL model)",
          b"<?xm": "xml", b"HIER": "bvh", b"FntF": "font", b"MZ\x90\x00": "pe exe", b"SCHl": "EA audio (SCHl)"}


def stats(outdir):
    c = Counter()
    for r, _, fs in os.walk(outdir):
        for f in fs:
            with open(os.path.join(r, f), "rb") as h:
                m = h.read(4)
            c[MAGICS.get(m, "xml-ish" if m[:1] == b"<" else f"other:{m.hex()}")] += 1
    for k, v in c.most_common(30):
        print(f"  {v:6}  {k}")


if __name__ == "__main__":
    cmd, args = sys.argv[1], sys.argv[2:]
    if cmd == "list":
        with open(args[0], "rb") as f:
            for n, o, s in read_big_index(f.read()):
                print(f"{s:10}  {n}")
    elif cmd == "extract":
        st = extract(args[0], args[1], "--recursive" in args)
        print(dict(st))
    elif cmd == "fsh2png":
        fsh2png(args[0], args[1])
    elif cmd == "stats":
        stats(args[0])
    elif cmd == "hash":
        for a in args:
            print(f"{name_hash(a)}  {a}")
    else:
        print(__doc__)
