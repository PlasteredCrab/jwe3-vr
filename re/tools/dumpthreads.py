# For every thread in a minidump, list the modules of interest found on its stack (heuristic: values that
# fall inside those modules), to see which threads were inside the mod, OpenXR or the VR runtime.
#   E:\Modding\tools\re-venv\Scripts\python.exe -E dumpthreads.py <dump> [module-prefix ...]
import struct, sys

from minidump.minidumpfile import MinidumpFile

mf = MinidumpFile.parse(sys.argv[1])
want = [w.lower() for w in (sys.argv[2:] or ["jwe3vr", "openxr", "vrclient", "steamxr", "d3d12", "dxgi"])]
mods = sorted((m.baseaddress, m.baseaddress + m.size, m.name.replace("/", "\\").split("\\")[-1]) for m in mf.modules.modules)
reader = mf.get_reader()


def where(a):
    for b, e, n in mods:
        if b <= a < e:
            return n, a - b
    return None, 0


crash_tid = mf.exception.exception_records[0].ThreadId
for t in mf.threads.threads:
    try:
        data = reader.read(t.Stack.StartOfMemoryRange, t.Stack.MemoryLocation.DataSize)
    except Exception:
        continue
    found = []
    for off in range(0, len(data) - 8, 8):
        n, o = where(struct.unpack_from("<Q", data, off)[0])
        if n and any(n.lower().startswith(w) for w in want):
            found.append(f"{n}+{o:#x}")
    if found:
        tag = " (crashing thread)" if t.ThreadId == crash_tid else ""
        print(f"thread {t.ThreadId:#x}{tag}: " + ", ".join(found[:14]))
