// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <detail/Atomic.hpp>
#include <threading/Mutex.hpp>

namespace ZHLN {
struct alignas(64) BufferSync {
    ZHLN::Atomic<int> viewExportCount;
    ZHLN::Mutex       shadowLock;
};

static_assert((std::is_trivially_default_constructible_v<BufferSync> && std::is_trivially_copyable_v<BufferSync>) );
} // namespace ZHLN