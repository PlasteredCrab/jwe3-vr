#include "scan.h"
#include "common.h"
#include "log.h"

namespace jvr::scan {

static IMAGE_NT_HEADERS* Headers() {
    auto base = (uint8_t*)GetModuleHandleW(nullptr);
    auto dos = (IMAGE_DOS_HEADER*)base;
    return (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
}

uint32_t GameBuild() { return Headers()->FileHeader.TimeDateStamp; }

static bool Parse(const char* pattern, std::vector<int>& out) {
    out.clear();
    for (const char* p = pattern; *p;) {
        if (*p == ' ') {
            ++p;
            continue;
        }
        if (p[0] == '?') {
            out.push_back(-1);
            p += (p[1] == '?') ? 2 : 1;
            continue;
        }
        char hex[3] = {p[0], p[1], 0};
        if (!p[1]) return false;
        out.push_back((int)strtol(hex, nullptr, 16));
        p += 2;
    }
    return !out.empty();
}

void ForEachPattern(const char* pattern, const std::function<void(uint8_t*)>& fn) {
    std::vector<int> pat;
    if (!Parse(pattern, pat)) return;
    auto base = (uint8_t*)GetModuleHandleW(nullptr);
    IMAGE_NT_HEADERS* nt = Headers();
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        size_t size = sec->Misc.VirtualSize;
        if (size > (128u << 20) || size < pat.size()) continue;
        uint8_t* start = base + sec->VirtualAddress;
        size_t end = size - pat.size();
        int first = pat[0];
        for (size_t o = 0; o <= end; ++o) {
            if (first >= 0 && start[o] != (uint8_t)first) continue;
            bool ok = true;
            for (size_t k = 1; k < pat.size(); ++k)
                if (pat[k] >= 0 && start[o + k] != (uint8_t)pat[k]) {
                    ok = false;
                    break;
                }
            if (ok) fn(start + o);
        }
    }
}

uint8_t* FindPattern(const char* pattern) {
    uint8_t* hit = nullptr;
    ForEachPattern(pattern, [&](uint8_t* p) {
        if (!hit) hit = p;
    });
    return hit;
}

} // namespace jvr::scan
