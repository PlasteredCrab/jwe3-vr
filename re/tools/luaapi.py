# Step 3: the Lua binding map. Bindings are registered as
#   lea <reg>,[C function] ... lea r8,["Name"] ... call lua_setfield(L, -2, "Name")
# so for every LEA of a string into r8, the nearest earlier LEA of a code address is the binding.
# Output lua_api.tsv: name, function VA, registration site VA, first-byte flag. "VMJMP" marks a
# binding whose first instruction jumps out of the game code (into the Denuvo VM): call those
# from Lua, never hook them.
#   python -E <tools>/luaapi.py [--exe <path>]   (after strings.py and leascan.py)
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe import Exe, exe_path, load_leas, load_strings

x = Exe(exe_path())
s, craw, cva = x.code()
LO, HI = cva, cva + s.Misc_VirtualSize
strs = load_strings()
leas = load_leas()


def flag(fva):
    b = craw[fva - cva:fva - cva + 5]
    if len(b) == 5 and b[0] == 0xE9:
        t = fva + 5 + struct.unpack_from('<i', b, 1)[0]
        if not LO <= t < HI:
            return 'VMJMP'
    return ''


out = []
for i, (a, t, r) in enumerate(leas):
    if r == 8 and t in strs:
        j = i - 1
        while j >= 0 and a - leas[j][0] < 0x140:
            a2, t2, r2 = leas[j]
            if r2 != 8 and LO <= t2 < HI:
                out.append((strs[t], hex(t2), hex(a), flag(t2)))
                break
            if r2 == 8 and leas[j][1] in strs:
                break
            j -= 1
open('lua_api.tsv', 'w').write('\n'.join('\t'.join(o) for o in out))
vm = sum(1 for o in out if o[3])
print(len(out), 'bindings,', vm, f'({100 * vm / max(1, len(out)):.1f}%) start with a jump into the VM')
