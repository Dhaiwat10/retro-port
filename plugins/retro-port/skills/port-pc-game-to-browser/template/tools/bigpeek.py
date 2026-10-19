import sys, struct, re
sys.path.insert(0, sys.argv[1]); from bigls import read_big
path = sys.argv[2]; f = open(path, 'rb'); _, ents = read_big(path)
def peek(pat, n=3, nbytes=48):
    hits = [e for e in ents if re.search(pat, e[0], re.I)][:n]
    for name, off, size in hits:
        f.seek(off); b = f.read(nbytes)
        print(f"  {name} ({size} B): {b[:16].hex(' ')} | {b.decode('latin1').encode('unicode_escape').decode()[:60]}")
for pat in [r'\.o$', r'\.exe$', r'\.dff$', r'\.txd$', r'\.xml$', r'\.fsh$', r'\.big$', r'\.ffn$', r'\.english$', r'\.bvh$', r'\.ash$', r'\.rsg$']:
    print(pat); peek(pat)
