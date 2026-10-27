# Phase 0 binary survey: answers the JWE3 questions the JWE2 port depends on, in one run.
#   python -E <tools>/survey.py [--exe <path>]  ->  prints a report and writes survey.md
# Covers: build stamp, section table (Denuvo VM blob, misnamed code section), imports (proxy DLL
# candidates, D3D12/DXGI), Streamline/NGX DLLs in the game folder, content packs (ACSE present?),
# Lua/UI/render strings, and the Lua stack-op counter scan the bridge relies on.
import os, re, struct, sys, time
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe import Exe, exe_path, entropy, VM_MIN

path = exe_path()
x = Exe(path)
game = os.path.dirname(path)
lines = []


def out(s=''):
    print(s)
    lines.append(s)


fh = x.pe.FILE_HEADER
out(f'# Survey of {os.path.basename(path)}')
out()
out(f'- path: `{path}`')
out(f'- size: {len(x.data) / 2**20:.1f} MB')
out(f'- PE timestamp (scan::kKnownBuild): **{fh.TimeDateStamp}** ({time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(fh.TimeDateStamp))} UTC)')
out(f'- image base: {hex(x.base)}')
out()
out('## Sections')
out()
out('| name | VA | virtual size | raw size | flags | entropy | note |')
out('| --- | --- | --- | --- | --- | --- | --- |')
code = x.code_sections()
code_name = code[0][0] if code else None
for nm, s in x.sections():
    c = s.Characteristics
    fl = ('R' if c & 0x40000000 else '-') + ('W' if c & 0x80000000 else '-') + ('X' if c & 0x20000000 else '-')
    note = ''
    if c & 0x20000000 and s.Misc_VirtualSize >= VM_MIN:
        note = 'Denuvo VM blob? (executable, huge)'
    elif nm == code_name:
        note = 'game code (largest executable section under 128 MB)'
    out(f'| `{nm}` | {hex(x.va(s))} | {s.Misc_VirtualSize / 2**20:.1f} MB | {s.SizeOfRawData / 2**20:.1f} MB | {fl} | {entropy(x.raw(s)):.2f} | {note} |')
out()

out('## Imports')
out()
x.pe.parse_data_directories(directories=[1])
proxy_candidates = {'version.dll', 'winhttp.dll', 'dinput8.dll', 'dxgi.dll', 'd3d11.dll', 'winmm.dll', 'xinput1_3.dll', 'xinput1_4.dll', 'xinput9_1_0.dll'}
for e in getattr(x.pe, 'DIRECTORY_ENTRY_IMPORT', []):
    dll = e.dll.decode(errors='replace')
    names = [i.name.decode() for i in e.imports if i.name][:6]
    mark = ' **(proxy candidate)**' if dll.lower() in proxy_candidates else ''
    out(f'- `{dll}`{mark}: {len(e.imports)} imports, e.g. {", ".join(names)}')
out()

out('## Game folder')
out()
dlls = sorted(f for f in os.listdir(game) if f.lower().endswith('.dll'))
sl = [f for f in dlls if f.lower().startswith('sl.') or 'nvngx' in f.lower() or 'ffx' in f.lower() or 'xess' in f.lower()]
out(f'- DLLs next to the exe: {", ".join(dlls) or "none"}')
out(f'- upscaler / Streamline DLLs: {", ".join(sl) or "none"}')
exes = sorted(f for f in os.listdir(game) if f.lower().endswith('.exe'))
out(f'- exes: {", ".join(exes)}')
ovl = os.path.join(game, 'Win64', 'ovldata')
if os.path.isdir(ovl):
    packs = sorted(os.listdir(ovl))
    out(f'- content packs in Win64\\ovldata: {len(packs)}; ACSE installed: {"ACSE" in packs}')
    for want in ('Content0', 'GameMain', 'Init'):
        hits = [p for p in packs if p.lower().startswith(want.lower())]
        if hits:
            out(f'  - {want}*: {", ".join(hits[:8])}')
out()

out('## Strings (outside the VM blob)')
out()
probe = ['Lua 5.1', 'Lua 5.2', 'Lua 5.3', 'Lua 5.4', 'unexpected symbol', 'loadlib', 'package', 'loadstring',
         'tryrequire', 'traceback', 'denuvo', 'Denuvo', 'Scaleform', 'GFx', 'CameraModeManager', 'SetFOVY',
         'GetMouseRay', 'SetTransform', 'GetMainCameraID', 'AddContentToCall', 'EnvironmentPrototype',
         'ParkEnvironment', 'XInputGetState', 'D3D12CreateDevice', 'CreateDXGIFactory', 'slInit']
found = Counter()
render_nodes = set()
for nm, s in x.sections():
    if s.SizeOfRawData >= VM_MIN:
        continue
    raw = x.raw(s)
    for p in probe:
        found[p] += raw.count(p.encode())
    for m in re.finditer(rb'fRender[A-Za-z0-9_]*Node', raw):
        render_nodes.add(m.group().decode())
for p in probe:
    out(f'- `{p}`: {found[p]}')
out(f'- named render-graph nodes (`fRender...Node`): {len(render_nodes)}')
out()

out('## Lua stack-op counter (bridge.cpp scan)')
out()
s, craw, cva = x.code()
hits = Counter()
for m in re.finditer(rb'\x8b\x0d....\xff\xc1\x89\x0d....', craw, re.S):
    o = m.start()
    a = cva + o + 6 + struct.unpack_from('<i', craw, o + 2)[0]
    b = cva + o + 14 + struct.unpack_from('<i', craw, o + 10)[0]
    if a == b:
        hits[a] += 1
top = hits.most_common(3)
if top:
    for a, n in top:
        out(f'- {hex(a)}: {n} uses')
    dominant = len(top) == 1 or top[0][1] >= 2 * top[1][1]
    out(f'- dominant target: **{"yes" if dominant else "NO - check by hand"}** (JWE2: one target, 66 uses)')
else:
    out('- **no matches**: the counter pattern changed; the bridge pushes without it (verify the layout by hand)')
out()
out(f'Functions in .pdata: {len(x.functions())}')

open('survey.md', 'w', encoding='utf8').write('\n'.join(lines) + '\n')
