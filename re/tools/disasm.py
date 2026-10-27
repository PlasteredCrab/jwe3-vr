# Disassemble at a VA, annotating RIP-relative string refs:
#   python -E <tools>/disasm.py [--exe <path>] <hex va> [count]   (strings.tsv in the current folder)
import os, re, sys

import capstone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe import Exe, exe_path, load_strings

x = Exe(exe_path())
strs = load_strings() if os.path.exists('strings.tsv') else {}
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
a = int(sys.argv[1], 16)
n = int(sys.argv[2]) if len(sys.argv) > 2 else 40
for i in list(md.disasm(x.read_va(a, n * 8), a))[:n]:
    c = ''
    m = re.search(r'rip \+ (0x[0-9a-f]+)', i.op_str)
    if m:
        t = i.address + i.size + int(m.group(1), 16)
        c = f'  ; {hex(t)} {strs.get(t, "")!r}'
    print(f"{i.address:x}: {i.mnemonic} {i.op_str}{c}")
