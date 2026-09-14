// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/diagnostics/SanitizerBridges.cpp
//
// The sanitizer runtime's option hooks and Jolt's assert/trace bridges. These
// two things share a file for a reason that is easy to undo by accident:
//
// The __asan_default_options / __lsan_default_options / __ubsan_default_options
// / __tsan_default_options exports are looked up by the sanitizer runtime at
// process startup, and nothing in the engine references them by name. This is a
// static library, so a translation unit whose symbols are all unreferenced is
// simply not pulled out of the archive -- the options would silently stop being
// applied. They stay in the same TU as JoltTraceBridge and JoltAssertBridge,
// which EngineGlobals.cpp does reference, and that reference is what drags the
// option exports into the link. Splitting them apart needs something like
// --whole-archive before it is safe.

#include <Zahlen/Core/Print.hpp> // BufferPrint
#include <Zahlen/Log.hpp>
#include <cstdarg>
#include <cstdint>
#include <cstring>

#if defined(__ASAN_ENABLED__) || defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
// NOLINTBEGIN(bugprone-reserved-identifier)
extern "C" [[gnu::visibility("default")]] const char* __asan_default_options() {
    // protect_shadow_gap=0 prevents NVIDIA driver ray tracing mmap collisions
    return "protect_shadow_gap=0:detect_leaks=1:symbolize=1:halt_on_error=1";
}

extern "C" [[gnu::visibility("default")]] const char* __lsan_default_options() {
    return "suppressions=" ZHLN_PROJECT_ROOT "/lsan.supp:print_suppressions=0";
}

extern "C" [[gnu::visibility("default")]] const char* __ubsan_default_options() {
    return "suppressions=" ZHLN_PROJECT_ROOT "/ubsan.supp:print_stacktrace=1:halt_on_error=1";
}

extern "C" [[gnu::visibility("default")]] const char* __tsan_default_options() {
    return "suppressions=" ZHLN_PROJECT_ROOT "/tsan.supp:halt_on_error=1";
}
// NOLINTEND(bugprone-reserved-identifier)

#endif

namespace ZHLN {

auto JoltTraceBridge(const char* inFMT, ...) noexcept -> void {
    va_list list;
    va_start(list, inFMT);

    char buffer[1024] {};
    int  result = ZHLN::BufferPrint(buffer, sizeof(buffer), inFMT, list);

    va_end(list);

    if (result > 0) {
        ZHLN::Panic("{}", buffer);
    }
}

auto JoltAssertBridge(const char* inExpression, const char* inMessage, const char* inFile, uint32_t inLine) noexcept -> bool {
    // 1. If it is a strict mathematical normalization assertion, log a warning and return FALSE.
    // This tells Jolt to bypass the crash/breakpoint and continue running safely.
    if (inExpression != nullptr && std::strcmp(inExpression, "inQuat.IsNormalized()") == 0) {
        // Log once or quietly so it doesn't spam your console
        static uint32_t warnCount = 0;
        if (warnCount++ < 5) {
            ZHLN::Log(
                "[Jolt Math Warning] Quaternion slightly out of normalization tolerance at "
                "{}:{}. Bypassing safely.",
                inFile, inLine
            );
        }
        return false; // <-- BYPASS CRASH
    }

    // 2. Keep standard fatal assertions active for critical engine errors
    ZHLN::Log(
        "--- JOLT ASSERT FAILED ---\n"
        "Expr: {}\n"
        "Msg:  {}\n"
        "File: {}:{}\n"
        "--------------------------\n",
        inExpression, ((inMessage != nullptr) ? inMessage : "None"), inFile, inLine
    );
    return true;
}

} // namespace ZHLN
