# Which extracted Lua bytecode modules contain a string (constants, field and function names):
#   python -E <tools>/luagrep.py <dir with *.lua.bin> [<dir> ...] -- <string> [<string> ...]
# Reads every module once; prints, per string, the modules that contain it (first 8, then a count).
import os, sys

if '--' not in sys.argv:
    sys.exit('usage: luagrep.py <dir>... -- <string>...')
i = sys.argv.index('--')
dirs, words = sys.argv[1:i], sys.argv[i + 1:]
hits = {w: [] for w in words}
for d in dirs:
    for f in sorted(os.listdir(d)):
        if not f.endswith('.lua.bin'):
            continue
        data = open(os.path.join(d, f), 'rb').read()
        for w in words:
            if w.encode() in data:
                hits[w].append(f[:-8])
for w in words:
    h = hits[w]
    more = f' (+{len(h) - 8} more)' if len(h) > 8 else ''
    print(f'{w}: {", ".join(h[:8]) or "-"}{more}')
