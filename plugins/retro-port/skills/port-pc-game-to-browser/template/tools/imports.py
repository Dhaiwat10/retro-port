import sys, pefile
pe = pefile.PE(sys.argv[1])
for imp in pe.DIRECTORY_ENTRY_IMPORT:
    print(imp.dll.decode(), ":", ", ".join(sorted((i.name or b'#%d'%i.ordinal).decode() for i in imp.imports)))
