// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include "EngineErrors.hpp"
#include "ErrorRegistry.hpp"
#include "FramePlan.hpp"
#include <Zahlen/Core/ErrorDescribe.hpp>
#include <Zahlen/Input/ActionValues.hpp>
#include <Zahlen/Render/FrameResult.hpp>
#include <Zahlen/Render/Info.hpp>
#include <format>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ZHLN {
namespace {
// THE one error-category table for the whole process. The compile-time
// path (TemplatedDetail::AnchorEnumCategory, called from ErrorCode(E))
// writes into it so std::format("{}", err) works anywhere; ZHLN::ErrorRegistry
// (and the COM::IErrorRegistry adapter over it) reads/writes the same table.
// One store, owned strings, no function pointers or views into registrant
// DSOs -- safe across dlclose. Lives in libzahlen_engine so every DSO shares
// it.
struct OwnedEntry {
    uint32_t    value = 0;
    std::string name;
    std::string message;
};

struct OwnedCategory {
    std::string             name;
    std::vector<OwnedEntry> entries;
};

auto Categories() -> std::unordered_map<uint32_t, OwnedCategory>& {
    static std::unordered_map<uint32_t, OwnedCategory> categories;
    return categories;
}

auto CategoriesMutex() -> std::mutex& {
    static std::mutex mutex;
    return mutex;
}

auto CopyEntries(const std::span<const CategoryEntryInput> entries) -> std::vector<OwnedEntry> {
    std::vector<OwnedEntry> out;
    out.reserve(entries.size());
    for (const auto& entry: entries) {
        out.push_back(OwnedEntry {
            .value   = entry.value,
            .name    = std::string(entry.name),
            .message = std::string(entry.message.empty() ? entry.name : entry.message),
        });
    }
    return out;
}
} // namespace

// ---- The plain engine-first registry ---------------------------------------

auto ErrorRegistry::RegisterCategory(
    const uint32_t categoryHash, const std::string_view categoryName, const std::span<const CategoryEntryInput> entries
) noexcept -> bool {
    const std::scoped_lock lock(CategoriesMutex());
    auto&                  categories = Categories();
    if (const auto existing = categories.find(categoryHash); existing != categories.end()) {
        if (existing->second.name != categoryName) {
            // Two different categories hashed to the same word. The old global
            // registry silently dropped the second one; fail loud instead.
            return false;
        }
        existing->second.entries = CopyEntries(entries);
        return true;
    }
    OwnedCategory category {.name = std::string(categoryName), .entries = CopyEntries(entries)};
    categories.emplace(categoryHash, std::move(category));
    return true;
}

bool ErrorRegistry::UnregisterCategory(const uint32_t categoryHash) noexcept {
    const std::scoped_lock lock(CategoriesMutex());
    return Categories().erase(categoryHash) != 0;
}

auto ErrorRegistry::Describe(ErrorCode code, std::string& outCategory, std::string& outName, std::string& outMessage) const noexcept
    -> bool {
    const std::scoped_lock lock(CategoriesMutex());
    const auto&            categories = Categories();
    const auto             it         = categories.find(code.CategoryHash());
    if (it == categories.end()) {
        return false;
    }
    for (const auto& entry: it->second.entries) {
        if (entry.value == code.Value()) {
            outCategory = it->second.name;
            outName     = entry.name;
            outMessage  = entry.message;
            return true;
        }
    }
    // A known category with an unknown enumerator is still a miss: the outputs
    // are left untouched so a caller that ignores the result cannot mistake a
    // stale description for a real one.
    return false;
}

void ErrorRegistry::RecordDetail(const ErrorCode code, const LogSource source, const std::string_view detail) noexcept {
    // Everything is copied here, while the recording DSO is still mapped: the
    // source views point into its .rodata and the detail into its stack.
    Detail recorded {
        .code     = code,
        // Base name, not the full path: ErrorRecord::file is a String64 and an
        // absolute build path is longer than that, so truncating it cut the
        // filename off the end -- the one part a reader needs.
        .file     = std::string(LogDetail::BaseName(source.file)),
        .function = std::string(source.function),
        .line     = source.line,
        .channel  = String32 {source.channel},
        .detail   = std::string(detail),
    };
    const std::scoped_lock lock(_mutex);
    _last    = std::move(recorded);
    _hasLast = true;
}

auto ErrorRegistry::LastDetail() const noexcept -> std::optional<Detail> {
    const std::scoped_lock lock(_mutex);
    if (!_hasLast) {
        return std::nullopt;
    }
    return _last;
}

} // namespace ZHLN


// ---- Static (compile-time anchored) error categories ----------------------
// The same table, reached without a registry instance.
namespace ZHLN {

auto TemplatedDetail::RegisterStaticErrorCategoryRaw(const uint32_t categoryHash, const std::string_view categoryName, const std::span<const StaticErrorEntry> entries) noexcept -> bool {
    const std::scoped_lock lock(CategoriesMutex());
    auto&                  categories = Categories();
    if (const auto it = categories.find(categoryHash); it != categories.end()) {
        return it->second.name == categoryName;
    }
    OwnedCategory owned {.name = std::string(categoryName), .entries = {}};
    owned.entries.reserve(entries.size());
    for (const auto& entry: entries) {
        owned.entries.push_back(OwnedEntry {
            .value   = entry.value,
            .name    = std::string(entry.name),
            .message = std::string(entry.message.empty() ? entry.name : entry.message),
        });
    }
    categories.emplace(categoryHash, std::move(owned));
    return true;
}

auto DescribeStaticError(const ErrorCode code) -> std::string {
    const std::scoped_lock lock(CategoriesMutex());
    const auto&            categories = Categories();
    if (const auto it = categories.find(code.CategoryHash()); it != categories.end()) {
        for (const auto& entry: it->second.entries) {
            if (entry.value == code.Value()) {
                return std::format("{}::{}: {}", it->second.name, entry.name, entry.message);
            }
        }
    }
    return std::format("error {:#010x}:{}", code.CategoryHash(), code.Value());
}

} // namespace ZHLN
