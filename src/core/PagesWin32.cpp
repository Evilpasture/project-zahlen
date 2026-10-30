// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Core/Pages.hpp>
#include <Zahlen/Core/Platform.hpp>

namespace ZHLN {

auto GetPageSize() noexcept -> size_t {
    static const size_t pageSize = []() noexcept -> size_t {
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        return static_cast<size_t>(info.dwPageSize);
    }();
    return pageSize;
}

auto AllocatePages(size_t bytes, PageProtection prot) noexcept -> void* {
    if (bytes == 0) {
        return nullptr;
    }
    DWORD winProt = PAGE_READWRITE;
    if (prot == PageProtection::NoAccess) {
        winProt = PAGE_NOACCESS;
    } else if (prot == PageProtection::Guard) {
        winProt = PAGE_READWRITE | PAGE_GUARD;
    }
    return VirtualAlloc(nullptr, AlignUpToPage(bytes), MEM_COMMIT | MEM_RESERVE, winProt);
}

auto ProtectPages(void* address, size_t bytes, PageProtection prot) noexcept -> bool {
    if (address == nullptr || bytes == 0) {
        return false;
    }
    DWORD winProt = PAGE_READWRITE;
    if (prot == PageProtection::NoAccess) {
        winProt = PAGE_NOACCESS;
    } else if (prot == PageProtection::Guard) {
        winProt = PAGE_READWRITE | PAGE_GUARD;
    }
    DWORD oldProtect = 0;
    return VirtualProtect(address, AlignUpToPage(bytes), winProt, &oldProtect) != 0;
}

void FreePages(void* address, [[maybe_unused]] size_t bytes) noexcept {
    if (address != nullptr) {
        VirtualFree(address, 0, MEM_RELEASE);
    }
}

} // namespace ZHLN
