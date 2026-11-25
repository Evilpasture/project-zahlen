// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The hooks Jolt reports through. They live here rather than in
// include/Zahlen/Log.hpp because Layer 0 knows no subsystem: a physics engine's
// signature in the logging header is the wrong edge.
//
// Jolt installs them as bare C function pointers, so there is no userdata slot
// and no context to thread through: whatever these two need, they keep in the
// translation unit that defines them.

#include <cstdint>

namespace ZHLN {

auto JoltTraceBridge(const char* inFMT, ...) noexcept -> void;
auto JoltAssertBridge(const char* inExpression, const char* inMessage, const char* inFile, uint32_t inLine) noexcept -> bool;

} // namespace ZHLN
