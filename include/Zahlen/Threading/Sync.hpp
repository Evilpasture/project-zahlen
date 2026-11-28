// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Threading/Mutex.hpp>
#include <atomic>

namespace ZHLN {
struct alignas(64) BufferSync {
    std::atomic<int> viewExportCount;
    ZHLN::Mutex      shadowLock;
};

static_assert(std::is_nothrow_default_constructible_v<BufferSync>);
static_assert(std::is_trivially_destructible_v<BufferSync>);
static_assert(std::is_standard_layout_v<BufferSync>);
} // namespace ZHLN
