import sys, struct, collections, os
def read_big(path):
    f = open(path, 'rb'); magic = f.read(4)
    if magic not in (b'BIGF', b'BIG4', b'BIGH'): return magic, None
    total = struct.unpack('<I', f.read(4))[0]; count, hdr = struct.unpack('>II', f.read(8))
    ents = []
    for _ in range(count):
        off, size = struct.unpack('>II', f.read(8)); name = b''
        while (c := f.read(1)) != b'\0': name += c
        ents.append((name.decode('latin1'), off, size))
    return magic, ents
if __name__ == "__main__":
      for p in sys.argv[1:]:
        magic, ents = read_big(p)
        print("=" * 60); print(os.path.basename(p), magic)
        if ents is None: continue
        ext = collections.Counter(); sz = collections.Counter()
        for n, o, s in ents:
            e = os.path.splitext(n)[1].lower() or '(none)'; ext[e] += 1; sz[e] += s
        print(f"{len(ents)} files")
        for e, c in sorted(ext.items(), key=lambda x: -sz[x[0]])[:25]: print(f"  {e:10} {c:6} files {sz[e]/1e6:9.1f} MB")
        if len(sys.argv) > 1 and os.environ.get('SHOW'): [print('   ', n, s) for n, o, s in ents[:int(os.environ['SHOW'])]]
