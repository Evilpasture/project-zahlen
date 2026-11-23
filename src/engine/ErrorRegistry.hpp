// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace ZHLN {

// One category entry as callers hand it in. The registry copies every string:
// what it stores is engine-owned, never a view into the registering DSO, so
// unloading a plugin cannot leave dangling names behind.
struct CategoryEntryInput {
    uint32_t         value   = 0;
    std::string_view name    {};
    std::string_view message {};
};

// The engine's error registry as a plain C++ class -- the same shape as the
// other engine subsystems (AudioContext, RenderContext, InputContext): owned
// and used natively, adapted to COM::IErrorRegistry only at src/engine/com/.
//
// The category table itself is process-wide and engine-owned (defined in
// ErrorRegistry.cpp), because the compile-time anchoring path writes into it
// too; what is per-instance is only the "last recorded error" slot.
class ErrorRegistry {
  public:
    // Copies every string. Re-registering the same hash under the same name
    // replaces the entry table (plugin reload); the same hash under a
    // *different* name is a collision and fails, instead of silently dropping
    // one of the two categories.
    auto RegisterCategory(uint32_t categoryHash, std::string_view categoryName, std::span<const CategoryEntryInput> entries) noexcept
        -> bool;
    bool UnregisterCategory(uint32_t categoryHash) noexcept;

    // True when the category (and the anchoring enum entry) is known.
    auto Describe(ErrorCode code, std::string& outCategory, std::string& outName, std::string& outMessage) const noexcept -> bool;

    void RecordDetail(ErrorCode code, LogSource source, std::string_view detail) noexcept;

    struct Detail {
        ErrorCode   code {};
        LogSource   source {};
        std::string detail {};
    };

    // nullptr until something recorded a detail. The pointer stays valid until
    // the next RecordDetail on this registry.
    [[nodiscard]] auto LastDetail() const noexcept -> const Detail*;

  private:
    mutable Mutex _mutex;
    Detail        _last {};
    bool          _hasLast = false;
};

} // namespace ZHLN
