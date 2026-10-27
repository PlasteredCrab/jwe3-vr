#include "log.h"
#include "common.h"

#include <cstdio>

namespace jvr::log {

static FILE* g_file = nullptr;
static std::mutex g_lock;

void Open() {
    std::lock_guard<std::mutex> l(g_lock);
    if (g_file) return;
    std::wstring path = ModuleDir() + L"JWE3VR.log";
    g_file = _wfsopen(path.c_str(), L"w", _SH_DENYWR);
}

void Close() {
    std::lock_guard<std::mutex> l(g_lock);
    if (g_file) fclose(g_file);
    g_file = nullptr;
}

void Write(const char* level, const char* fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    SYSTEMTIME t;
    GetLocalTime(&t);
    std::lock_guard<std::mutex> l(g_lock);
    if (!g_file) return;
    fprintf(g_file, "%02d:%02d:%02d.%03d [%s] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, level, msg);
    fflush(g_file);
}

} // namespace jvr::log
