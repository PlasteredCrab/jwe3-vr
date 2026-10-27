# Shared helpers for the RE scripts: open the game exe, find its real code, read strings and
# the function table. The exe comes from the command line (--exe), the JWE3_EXE environment
# variable, or the default Steam path. Run every script with the RE venv on E: (pefile + capstone):
#   E:\Modding\tools\re-venv\Scripts\python.exe -E <tools>\<script>.py
# from re\work (git-ignored); outputs (strings.tsv, leas.pkl, ...) land in the current directory.
import glob, math, os, pickle, struct, sys

import pefile

DEFAULT_GAME = r"E:\SteamLibrary\steamapps\common\Jurassic World Evolution 3"
VM_MIN = 128 << 20  # executable sections at least this big are treated as the Denuvo VM blob


def exe_path(argv=None):
    argv = sys.argv if argv is None else argv
    if "--exe" in argv:
        i = argv.index("--exe")
        p = argv[i + 1]
        del argv[i:i + 2]
        return p
    if os.environ.get("JWE3_EXE"):
        return os.environ["JWE3_EXE"]
    hits = sorted(glob.glob(os.path.join(DEFAULT_GAME, "JWE3*.exe")))
    if not hits:
        sys.exit(f"no JWE3*.exe in {DEFAULT_GAME}; pass --exe <path> or set JWE3_EXE")
    return hits[0]


class Exe:
    def __init__(self, path):
        self.path = path
        self.data = open(path, "rb").read()
        self.pe = pefile.PE(data=self.data, fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase

    def sections(self):
        for s in self.pe.sections:
            yield s.Name.rstrip(b"\0").decode(errors="replace"), s

    def raw(self, s):
        return self.data[s.PointerToRawData:s.PointerToRawData + s.SizeOfRawData]

    def va(self, s, off=0):
        return self.base + s.VirtualAddress + off

    def code_sections(self):
        """Executable sections smaller than the VM blob, largest first (the game code)."""
        out = [(n, s) for n, s in self.sections()
               if s.Characteristics & 0x20000000 and 0 < s.Misc_VirtualSize < VM_MIN]
        return sorted(out, key=lambda t: -t[1].Misc_VirtualSize)

    def code(self):
        n, s = self.code_sections()[0]
        return s, self.raw(s), self.va(s)

    def functions(self):
        """(start, end) VAs from the exception directory (.pdata), sorted."""
        d = self.pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
        if not d.VirtualAddress:
            return []
        buf = self.pe.get_data(d.VirtualAddress, d.Size)
        return sorted({(self.base + struct.unpack_from("<I", buf, i)[0], self.base + struct.unpack_from("<I", buf, i + 4)[0])
                       for i in range(0, len(buf) - 11, 12)})

    def read_va(self, va, n):
        return self.pe.get_data(va - self.base, n)


def entropy(b):
    if not b:
        return 0.0
    counts = [0] * 256
    step = max(1, len(b) // (4 << 20))  # sample big sections
    for x in b[::step]:
        counts[x] += 1
    total = sum(counts)
    return -sum(c / total * math.log2(c / total) for c in counts if c)


def load_strings(path="strings.tsv"):
    """{va: string} from strings.py output."""
    out = {}
    for line in open(path, encoding="utf8"):
        sec, a, s = line.rstrip("\n").split("\t", 2)
        out[int(a, 16)] = s
    return out


def load_leas(path="leas.pkl"):
    return pickle.load(open(path, "rb"))
