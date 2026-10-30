// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
#include <cstddef>
#include <cstdint>
#include <memory_resource>

namespace ZHLN {

enum class PageProtection : std::uint8_t {
    NoAccess,
    ReadWrite,
    Guard,
};

// Implemented in PagesWin32.cpp or PagesPOSIX.cpp, not in public headers.
[[nodiscard]] ZHLN_API auto GetPageSize() noexcept -> size_t;

[[nodiscard]] inline auto AlignUpToPage(size_t bytes) noexcept -> size_t {
    const size_t page = GetPageSize();
    return (bytes + page - 1) & ~(page - 1);
}

[[nodiscard]] ZHLN_API auto AllocatePages(size_t bytes, PageProtection prot = PageProtection::ReadWrite) noexcept -> void*;
ZHLN_API auto ProtectPages(void* address, size_t bytes, PageProtection prot) noexcept -> bool;
ZHLN_API void FreePages(void* address, size_t bytes) noexcept;

struct GuardedRegion {
    void*  base  = nullptr;
    size_t size  = 0;
    void*  begin = nullptr;
    void*  end   = nullptr;

    [[nodiscard]] constexpr auto valid() const noexcept -> bool {
        return base != nullptr;
    }
};

[[nodiscard]] ZHLN_API auto AllocateGuardedRegion(size_t bytes) noexcept -> GuardedRegion;
ZHLN_API void FreeGuardedRegion(GuardedRegion region) noexcept;

// Shared OS-backed resource used by page-owning containers and the EventBus.
// It is process-lifetime and accepts alignments up to the OS page size.
[[nodiscard]] ZHLN_API auto OsPageMemoryResource() noexcept -> std::pmr::memory_resource*;

} // namespace ZHLN
