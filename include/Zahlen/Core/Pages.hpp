// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Platform.hpp"

namespace ZHLN {

// ============================================================================
// Virtual Memory & OS Page Primitives
// ============================================================================

enum class PageProtection : std::uint8_t {
    NoAccess,  // PROT_NONE / PAGE_NOACCESS
    ReadWrite, // PROT_READ | PROT_WRITE / PAGE_READWRITE
    Guard,     // PROT_NONE on POSIX / PAGE_READWRITE | PAGE_GUARD on Windows
};

[[nodiscard]] inline auto GetPageSize() noexcept -> size_t {
    static const size_t pageSize = []() noexcept -> size_t {
#if defined(_WIN32)
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        return static_cast<size_t>(si.dwPageSize);
#else
        return static_cast<size_t>(sysconf(_SC_PAGESIZE));
#endif
    }();
    return pageSize;
}

[[nodiscard]] inline auto AlignUpToPage(size_t bytes) noexcept -> size_t {
    const size_t page = GetPageSize();
    return (bytes + page - 1) & ~(page - 1);
}

[[nodiscard]] inline auto AllocatePages(size_t bytes, PageProtection prot = PageProtection::ReadWrite) noexcept -> void* {
    if (bytes == 0) {
        return nullptr;
    }
    const size_t alignedBytes = AlignUpToPage(bytes);

#if defined(_WIN32)
    DWORD winProt = PAGE_READWRITE;
    if (prot == PageProtection::NoAccess) {
        winProt = PAGE_NOACCESS;
    } else if (prot == PageProtection::Guard) {
        winProt = PAGE_READWRITE | PAGE_GUARD;
    }
    return VirtualAlloc(nullptr, alignedBytes, MEM_COMMIT | MEM_RESERVE, winProt);
#else
    int posixProt = PROT_READ | PROT_WRITE;
    if (prot == PageProtection::NoAccess || prot == PageProtection::Guard) {
        posixProt = PROT_NONE;
    }
    void* ptr = mmap(nullptr, alignedBytes, posixProt, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (ptr == MAP_FAILED) ? nullptr : ptr;
#endif
}

inline auto ProtectPages(void* address, size_t bytes, PageProtection prot) noexcept -> bool {
    if (address == nullptr || bytes == 0) {
        return false;
    }
    const size_t alignedBytes = AlignUpToPage(bytes);

#if defined(_WIN32)
    DWORD winProt = PAGE_READWRITE;
    if (prot == PageProtection::NoAccess) {
        winProt = PAGE_NOACCESS;
    } else if (prot == PageProtection::Guard) {
        winProt = PAGE_READWRITE | PAGE_GUARD;
    }
    DWORD oldProtect = 0;
    return VirtualProtect(address, alignedBytes, winProt, &oldProtect) != 0;
#else
    int posixProt = PROT_READ | PROT_WRITE;
    if (prot == PageProtection::NoAccess || prot == PageProtection::Guard) {
        posixProt = PROT_NONE;
    }
    return mprotect(address, alignedBytes, posixProt) == 0;
#endif
}

inline void FreePages(void* address, [[maybe_unused]] size_t bytes) noexcept {
    if (address == nullptr) {
        return;
    }

#if defined(_WIN32)
    // Note: 'bytes' must be 0 when using MEM_RELEASE on Windows.
    // Address must be the base address from VirtualAlloc.
    VirtualFree(address, 0, MEM_RELEASE);
#else
    munmap(address, AlignUpToPage(bytes));
#endif
}

// ============================================================================
// Guarded Regions
// ============================================================================

/**
 * @brief A page-aligned allocation walled in by one inaccessible page on each
 * side, so that stepping off either end faults instead of silently corrupting
 * whatever is mapped next to it.
 */
struct GuardedRegion {
    void*  base  = nullptr; // Base of the whole mapping; hand this to FreeGuardedRegion().
    size_t size  = 0;       // Size of the whole mapping, guard pages included.
    void*  begin = nullptr; // First usable byte, just above the low guard page.
    void*  end   = nullptr; // One past the last usable byte; stacks grow down from here.

    [[nodiscard]] constexpr auto valid() const noexcept -> bool {
        return base != nullptr;
    }
};

/**
 * @brief Reserves `bytes` of read/write memory with a guard page on both ends.
 *
 * The usable payload is rounded up to a whole page, so `end - begin` may be
 * larger than requested. Returns an invalid region (`valid() == false`) if the
 * mapping or either guard page could not be set up.
 */
[[nodiscard]] inline auto AllocateGuardedRegion(size_t bytes) noexcept -> GuardedRegion {
    if (bytes == 0) {
        return {};
    }

    const size_t page   = GetPageSize();
    const size_t usable = AlignUpToPage(bytes);
    const size_t total  = usable + (page * 2); // Payload + 2 guard pages

    void* const base = AllocatePages(total, PageProtection::ReadWrite);
    if (base == nullptr) {
        return {};
    }

    auto* const low  = static_cast<std::byte*>(base);
    auto* const high = low + page + usable;

    if (!ProtectPages(low, page, PageProtection::Guard) || !ProtectPages(high, page, PageProtection::Guard)) {
        FreePages(base, total);
        return {};
    }

    return {.base = base, .size = total, .begin = low + page, .end = high};
}

/**
 * @brief Releases a region previously handed out by AllocateGuardedRegion().
 */
inline void FreeGuardedRegion(GuardedRegion region) noexcept {
    FreePages(region.base, region.size);
}

} // namespace ZHLN
