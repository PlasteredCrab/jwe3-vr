# Step 1 of the binding-map pipeline: dump every printable string in the exe with its VA.
# Run from a scratch folder:  python -E <tools>/strings.py [--exe <path>]  ->  strings.tsv
# Then: leascan.py (RIP-relative LEA refs in code) -> luaapi.py (Lua binding map) / refs.py / disasm.py
import os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe import Exe, exe_path, VM_MIN

x = Exe(exe_path())
out = []
for nm, s in x.sections():
    if s.SizeOfRawData >= VM_MIN:
        continue  # the VM blob holds no useful strings and takes minutes
    for m in re.finditer(rb'[\x20-\x7e]{5,200}\x00', x.raw(s)):
        out.append((nm, hex(x.va(s, m.start())), m.group()[:-1].decode()))
open('strings.tsv', 'w', encoding='utf8').write('\n'.join('\t'.join(o) for o in out))
print(len(out), 'strings')
