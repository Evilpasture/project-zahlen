// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Atomic.hpp>
#include <Zahlen/Threading/Mutex.hpp>

namespace ZHLN {
/**
 * C/FFI synchronization header. Its all-zero representation is the initialized
 * state, so native owners use `BufferSync sync {}` and foreign allocations must
 * be zero-filled before first use.
 */
struct alignas(64) BufferSync {
    ZHLN::Atomic<int> viewExportCount;
    ZHLN::Mutex       shadowLock;
};

static_assert(std::is_trivially_default_constructible_v<BufferSync>);
static_assert(std::is_trivially_copyable_v<BufferSync>);
static_assert(std::is_standard_layout_v<BufferSync>);
} // namespace ZHLN