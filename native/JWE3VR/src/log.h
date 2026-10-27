// Plain-text log next to the DLL. Thread-safe, flushed per line so a crash keeps the tail.
#pragma once

#include <cstdarg>

namespace jvr::log {

void Open();
void Close();
void Write(const char* level, const char* fmt, ...);

// Logs a message only the first time a given call site reaches it (per process).
#define JVR_LOG_ONCE(...)                         \
    do {                                          \
        static bool jvr_once_ = false;            \
        if (!jvr_once_) {                         \
            jvr_once_ = true;                     \
            ::jvr::log::Write("info", __VA_ARGS__); \
        }                                         \
    } while (0)

} // namespace jvr::log

#define LOGI(...) ::jvr::log::Write("info", __VA_ARGS__)
#define LOGW(...) ::jvr::log::Write("warn", __VA_ARGS__)
#define LOGE(...) ::jvr::log::Write("error", __VA_ARGS__)
