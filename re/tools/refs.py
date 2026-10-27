# Which functions reference a string:  python -E <tools>/refs.py [--exe <path>] "<string>" ...
# (after strings.py and leascan.py). Function bounds come from the exe's .pdata.
import bisect, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe import Exe, exe_path, load_leas

x = Exe(exe_path())
funcs = x.functions()
starts = [f[0] for f in funcs]


def fn(a):
    i = bisect.bisect_right(starts, a) - 1
    return hex(starts[i]) if i >= 0 and funcs[i][0] <= a < funcs[i][1] else '?'


strs = {}
for line in open('strings.tsv', encoding='utf8'):
    sec, a, s = line.rstrip('\n').split('\t', 2)
    strs.setdefault(s, []).append(int(a, 16))
byt = {}
for l in load_leas():
    byt.setdefault(l[1], []).append(l[0])
for w in sys.argv[1:]:
    for sa in strs.get(w, []):
        print(f"{w}: str {hex(sa)} -> funcs", sorted(set(fn(a) for a in byt.get(sa, []))))
