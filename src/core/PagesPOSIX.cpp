// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Core/Pages.hpp>
#include <sys/mman.h>
#include <unistd.h>

namespace ZHLN {

auto GetPageSize() noexcept -> size_t {
    static const size_t pageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    return pageSize;
}

auto AllocatePages(size_t bytes, PageProtection prot) noexcept -> void* {
    if (bytes == 0) {
        return nullptr;
    }
    const int posixProt = prot == PageProtection::ReadWrite ? PROT_READ | PROT_WRITE : PROT_NONE;
    void* const memory = mmap(nullptr, AlignUpToPage(bytes), posixProt, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return memory == MAP_FAILED ? nullptr : memory;
}

auto ProtectPages(void* address, size_t bytes, PageProtection prot) noexcept -> bool {
    if (address == nullptr || bytes == 0) {
        return false;
    }
    const int posixProt = prot == PageProtection::ReadWrite ? PROT_READ | PROT_WRITE : PROT_NONE;
    return mprotect(address, AlignUpToPage(bytes), posixProt) == 0;
}

void FreePages(void* address, size_t bytes) noexcept {
    if (address != nullptr) {
        munmap(address, AlignUpToPage(bytes));
    }
}

} // namespace ZHLN
