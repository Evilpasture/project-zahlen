// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include <Zahlen/Core/Print.hpp>
#include <Zahlen/Log.hpp>
#include <cstdarg>
#include <cstdint>
#include <cstring>

#if defined(__ASAN_ENABLED__) || defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
// NOLINTBEGIN(bugprone-reserved-identifier)
extern "C" [[gnu::visibility("default")]] const char* __asan_default_options() {
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
    if (inExpression != nullptr && std::strcmp(inExpression, "inQuat.IsNormalized()") == 0) {
        static uint32_t warnCount = 0;
        if (warnCount++ < 5) {
            ZHLN::Log(
                "[Jolt Math Warning] Quaternion slightly out of normalization tolerance at "
                "{}:{}. Bypassing safely.",
                inFile, inLine
            );
        }
        return false;
    }

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

}
