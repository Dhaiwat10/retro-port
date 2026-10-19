import sys, pefile, math, collections, datetime
def entropy(b):
    if not b: return 0
    c = collections.Counter(b); n = len(b)
    return -sum(v/n*math.log2(v/n) for v in c.values())
for path in sys.argv[1:]:
    pe = pefile.PE(path)
    print("="*70); print(path.split('/')[-1])
    fh, oh = pe.FILE_HEADER, pe.OPTIONAL_HEADER
    print("Machine %#x  Timestamp %s  ImageBase %#x  Entry %#x  Linker %d.%d  Subsystem %d" % (
        fh.Machine, datetime.datetime.utcfromtimestamp(fh.TimeDateStamp), oh.ImageBase,
        oh.AddressOfEntryPoint, oh.MajorLinkerVersion, oh.MinorLinkerVersion, oh.Subsystem))
    print("LAA flag:", bool(fh.Characteristics & 0x20), " Relocs:", hasattr(pe,'DIRECTORY_ENTRY_BASERELOC'))
    for s in pe.sections:
        d = s.get_data()
        print("  %-9s VA %#09x VSize %#09x Raw %#09x ent %.2f  %s" % (
            s.Name.rstrip(b'\0').decode(errors='replace'), s.VirtualAddress, s.Misc_VirtualSize,
            s.SizeOfRawData, entropy(d), hex(s.Characteristics)))
    if hasattr(pe, 'DIRECTORY_ENTRY_IMPORT'):
        for imp in pe.DIRECTORY_ENTRY_IMPORT:
            names = [ (i.name or b'#%d' % i.ordinal).decode() for i in imp.imports ]
            print("  IMPORT %-16s (%3d) %s" % (imp.dll.decode(), len(names), ", ".join(names[:12]) + (" ..." if len(names)>12 else "")))
    if hasattr(pe, 'VS_VERSIONINFO') and hasattr(pe, 'FileInfo'):
        for fi in pe.FileInfo:
            for e in fi:
                if hasattr(e, 'StringTable'):
                    for st in e.StringTable:
                        for k, v in st.entries.items(): print("  VER", k.decode(), "=", v.decode())
    if hasattr(pe, 'DIRECTORY_ENTRY_DEBUG'):
        for d in pe.DIRECTORY_ENTRY_DEBUG:
            if d.entry and hasattr(d.entry, 'PdbFileName'): print("  PDB:", d.entry.PdbFileName.decode(errors='replace'))
    if hasattr(pe, 'RICH_HEADER') and pe.RICH_HEADER:
        vals = pe.RICH_HEADER.values
        print("  Rich header entries (prodid,build,count):", [(vals[i]>>16, vals[i]&0xffff, vals[i+1]) for i in range(0,len(vals),2)])
