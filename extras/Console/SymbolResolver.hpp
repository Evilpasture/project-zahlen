// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/String.hpp>
#include <cstdint>
#include <string>

namespace ZHLN {

struct ResolvedSymbol {
    std::string name       = "??";
    std::string moduleName = "??";
    uintptr_t   address    = 0;
    uintptr_t   offset     = 0;
    bool        valid      = false;
};

class SymbolResolver {
  public:
    static void Initialize() noexcept;
    static void Shutdown() noexcept;

    /// Resolves an arbitrary code or function pointer to its demangled C++ symbol
    [[nodiscard]] static auto Resolve(const void* address) noexcept -> ResolvedSymbol;
    [[nodiscard]] static auto Resolve(uintptr_t address) noexcept -> ResolvedSymbol {
        return Resolve(reinterpret_cast<const void*>(address));
    }
};

} // namespace ZHLN
