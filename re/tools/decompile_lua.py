# Decompile selected JWE3 Lua bytecode modules (extracted by cobra-tools' ovl_tool_cmd.py)
# using cobra-tools' bundled luadec. Run with cobra-tools' venv python:
#   E:\Modding\tools\cobra-tools\.venv\Scripts\python.exe decompile_lua.py <bin_dir> <out_dir> <regex>...
# cobra-tools location: COBRA_TOOLS environment variable, else E:\Modding\tools\cobra-tools.
import os, re, sys
COBRA = os.environ.get("COBRA_TOOLS", r"E:\Modding\tools\cobra-tools")
sys.path.insert(0, COBRA)
os.chdir(COBRA)
from modules.formats.utils.lua_conversion import bin_to_lua

src, dst, pats = sys.argv[1], sys.argv[2], [re.compile(p) for p in sys.argv[3:]]
os.makedirs(dst, exist_ok=True)
ok = bad = 0
for f in sorted(os.listdir(src)):
    if not f.endswith('.lua.bin'):
        continue
    mod = f[:-8]
    if not any(p.search(mod) for p in pats):
        continue
    out, err = bin_to_lua(os.path.join(src, f))
    if out:
        open(os.path.join(dst, mod + '.lua'), 'wb').write(out)
        flag = b'DECOMPILER ERROR' in out
        ok += 1
        print(('PARTIAL ' if flag else 'ok      ') + mod)
    else:
        bad += 1
        print('FAILED  ' + mod + '  ' + str(err)[:120])
print(f'{ok} decompiled, {bad} failed')
