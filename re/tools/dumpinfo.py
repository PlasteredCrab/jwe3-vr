# Quick look at a minidump (e.g. JWE3VR_crash.dmp): exception, faulting module, and module addresses found on
# the crashing thread's stack (a heuristic call stack without unwind data).
#   E:\Modding\tools\re-venv\Scripts\python.exe -E dumpinfo.py <dump> [max frames]
import struct, sys

from minidump.minidumpfile import MinidumpFile

mf = MinidumpFile.parse(sys.argv[1])
limit = int(sys.argv[2]) if len(sys.argv) > 2 else 60
ex = mf.exception.exception_records[0]
rec = ex.ExceptionRecord
params = [hex(p) for p in rec.ExceptionInformation[:rec.NumberParameters]]
print("thread", hex(ex.ThreadId), "code", rec.ExceptionCode, "address", hex(rec.ExceptionAddress), "params", params)
mods = sorted((m.baseaddress, m.baseaddress + m.size, m.name.replace("/", "\\").split("\\")[-1]) for m in mf.modules.modules)


def where(a):
    for b, e, n in mods:
        if b <= a < e:
            return f"{n}+{a - b:#x}"
    return None


print("faulting address in", where(rec.ExceptionAddress))
th = [t for t in mf.threads.threads if t.ThreadId == ex.ThreadId][0]
reader = mf.get_reader()
start = th.Stack.StartOfMemoryRange
data = reader.read(start, th.Stack.MemoryLocation.DataSize)
shown = 0
for off in range(0, len(data) - 8, 8):
    v = struct.unpack_from("<Q", data, off)[0]
    m = where(v)
    if m:
        print(f"  stack+{off:#07x}: {m}")
        shown += 1
        if shown >= limit:
            break
print("modules of interest:", [n for b, e, n in mods if n.lower().startswith(("jwe3", "openxr", "sl.", "nvngx", "vrclient", "steamxr"))])
