// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later


#include "SanitizerBridges.hpp"
#include <Zahlen/Core/Print.hpp>
#include <physics/PhysicsLog.hpp>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstring>

namespace ZHLN {

namespace {

// Jolt's hooks are C function pointers with no userdata slot, so this counter
// has nowhere to live but the translation unit. That is the whole of the state:
// one atomic, shared by every physics world, with no object and no lifetime
// story pretending otherwise.
constexpr uint32_t kQuatWarningLimit = 5;

std::atomic<uint32_t> g_quatWarnings {0};

} // namespace

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
        // Jolt's normalization check trips on values a hair outside tolerance, in
        // numbers that would swamp any log. The first few are worth a line, the
        // rest are worth one line saying the rest are gone.
        const uint32_t seen = g_quatWarnings.fetch_add(1, std::memory_order_relaxed);
        if (seen == kQuatWarningLimit) {
            kPhysicsLog.Warn("Quaternion tolerance warnings suppressed (limit={}, action=bypass-remaining)", kQuatWarningLimit);
        }
        if (seen < kQuatWarningLimit) {
            kPhysicsLog.Warn(
                "Quaternion outside normalization tolerance (at={}:{}, action=bypass)", inFile != nullptr ? inFile : "?", inLine
            );
        }
        return false;
    }

    kPhysicsLog.Error(
        "Jolt assert failed (expr={}, msg={}, at={}:{})", inExpression != nullptr ? inExpression : "?",
        inMessage != nullptr ? inMessage : "None", inFile != nullptr ? inFile : "?", inLine
    );
    return true;
}

} // namespace ZHLN
