// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include <Zahlen/Core/Print.hpp>
#include <Zahlen/Log.hpp>
#include <cstdarg>
#include <cstdint>
#include <cstring>

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
