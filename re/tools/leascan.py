# Step 2: every RIP-relative "lea reg,[rip+disp32]" in the game code -> leas.pkl [(va, target, reg)].
#   python -E <tools>/leascan.py [--exe <path>]
import os, pickle, re, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe import Exe, exe_path

x = Exe(exe_path())
s, craw, cva = x.code()
# lea reg,[rip+disp32]: REX.W(48/4C) 8D modrm(mod=00,rm=101)
leas = []
for m in re.finditer(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', craw):
    o = m.start()
    disp = struct.unpack_from('<i', craw, o + 3)[0]
    a = cva + o
    reg = ((craw[o + 2] >> 3) & 7) + (8 if craw[o] == 0x4c else 0)
    leas.append((a, a + 7 + disp, reg))
pickle.dump(leas, open('leas.pkl', 'wb'))
print(len(leas), 'LEAs in', s.Name.rstrip(b'\0').decode(), hex(cva))
