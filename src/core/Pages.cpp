// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Core/Pages.hpp>
#include <Zahlen/Log.hpp>
#include <algorithm>
#include <memory_resource>

namespace ZHLN {

namespace {

class OsPageResource final : public std::pmr::memory_resource {
    auto do_allocate(size_t bytes, size_t alignment) -> void* override {
        if (alignment > GetPageSize()) {
            Panic("Page resource cannot satisfy alignment {} (page size {})", alignment, GetPageSize());
        }
        // A memory_resource is also permitted a zero-byte request. Reserve a
        // page so that its result is still a valid, aligned allocation.
        const size_t mappedBytes = std::max(bytes, GetPageSize());
        void* const  memory      = AllocatePages(mappedBytes);
        if (memory == nullptr) {
            Panic("Could not map {} bytes of backing pages", mappedBytes);
        }
        return memory;
    }

    void do_deallocate(void* memory, size_t bytes, size_t) override {
        FreePages(memory, std::max(bytes, GetPageSize()));
    }

    [[nodiscard]] auto do_is_equal(const std::pmr::memory_resource& other) const noexcept -> bool override {
        return this == &other;
    }
};

} // namespace

auto OsPageMemoryResource() noexcept -> std::pmr::memory_resource* {
    static OsPageResource pages;
    return &pages;
}

auto AllocateGuardedRegion(size_t bytes) noexcept -> GuardedRegion {
    if (bytes == 0) {
        return {};
    }

    const size_t page   = GetPageSize();
    const size_t usable = AlignUpToPage(bytes);
    const size_t total  = usable + (page * 2);

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

void FreeGuardedRegion(GuardedRegion region) noexcept {
    FreePages(region.base, region.size);
}

} // namespace ZHLN
