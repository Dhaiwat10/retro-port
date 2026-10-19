import sys, re
data = open(sys.argv[1], 'rb').read()
for m in re.finditer(rb'[\x20-\x7e]{%d,}' % int(sys.argv[2] if len(sys.argv) > 2 else 5), data):
    print(m.group().decode())
