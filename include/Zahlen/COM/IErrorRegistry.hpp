// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/COM/IErrorRegistry.hpp
//
// The error registry is a standalone refcounted COM object, created via
// CreateErrorRegistryRaw *before* CreateEngineRaw / CreateLogicWorldRaw and
// handed to them through an ErrorConfig chain node. That ordering is the
// point: when engine creation fails, the caller still holds the registry and
// can Describe the returned ErrorCode. Registration copies every string into
// registry-owned storage, so a plugin may register its enums, be dlclosed,
// and leave no dangling pointers behind.

#include <Zahlen/Core/ErrorDescribe.hpp>
#include <Zahlen/COM/ErrorTypes.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/IObject.hpp>
#include <Zahlen/Core/COM/Ref.hpp>
#include <Zahlen/Core/COM/Types.hpp>
#include <Zahlen/Core/ErrorCode.hpp>
#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/Core/Reflection/Core.hpp>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <array>
#include <cstdint>
#include <expected>
#include <type_traits>
#include <format>
#include <string>

namespace ZHLN::COM {

struct IErrorRegistry : IObject {
    // Copies categoryName and all entry strings. Re-registering the same
    // hash under the same name replaces the entry table (plugin reload);
    // the same hash under a *different* name is a hash collision and fails
    // with Error::InvalidArgument instead of silently dropping the category.
    virtual auto RegisterCategory(uint32_t categoryHash, StringView categoryName, Span<const ErrorEnumEntry> entries) noexcept
        -> Result<void>                                                                                  = 0;
    virtual bool UnregisterCategory(uint32_t categoryHash) noexcept                                      = 0;
    virtual auto Describe(ErrorCode code, ChainOutNode* outDesc) const noexcept -> Result<void>          = 0;
    virtual void RecordDetail(ErrorCode code, LogSource source, StringView detail) noexcept              = 0;
    virtual auto LastError(ChainOutNode* outRecord) const noexcept -> Result<void>                       = 0;

  protected:
    ~IErrorRegistry() = default;
};

// Builds the wire table for an annotated enum in the caller's translation
// unit; only the finished table crosses the boundary.
template <typename E>
    requires std::is_enum_v<E>
consteval auto MakeEnumErrorTable() -> std::array<ErrorEnumEntry, Reflect::kEnumEntries<E>.size()> {
    constexpr auto&                                           src = Reflect::kEnumEntries<E>;
    std::array<ErrorEnumEntry, Reflect::kEnumEntries<E>.size()> out {};
    for (std::size_t i = 0; i < src.size(); ++i) {
        out[i] = ErrorEnumEntry {
            .value   = static_cast<uint32_t>(src[i].value),
            .name    = StringView(src[i].name),
            .message = StringView(!src[i].message.empty() ? src[i].message : src[i].name),
        };
    }
    return out;
}

// Registers enum E under the same category hash ErrorCode(E) computes.
template <typename E>
    requires std::is_enum_v<E>
[[nodiscard]] inline auto RegisterEnum(IErrorRegistry& registry) noexcept -> Result<void> {
    (void) RegisterStaticErrorCategory<E>();
    static constexpr auto table = MakeEnumErrorTable<E>();
    return registry.RegisterCategory(
        Hash32(Reflect::ScopedTypeName<E>()),
        StringView(Reflect::ScopedTypeName<E>()),
        Span<const ErrorEnumEntry>(table.data(), table.size())
    );
}

// Exported by libzahlen_engine. The returned registry has one reference,
// already taken -- adopt it, do not AddRef it. Accepts ErrorConfig (to seed
// from an existing registry) or nullptr for a fresh, engine-pre-registered
// registry.
ZHLN_API auto CreateErrorRegistryRaw(const ChainInNode* config = nullptr) noexcept -> Result<IErrorRegistry*>;

// One-shot human-readable rendering of an ErrorCode for CLI tools and
// test reports: "Category::Name: message", falling back to the raw code
// when the category is not registered. Prefer IErrorRegistry::Describe
// when a description struct is needed.
[[nodiscard]] inline auto DescribeError(const IErrorRegistry& registry, const ErrorCode code) -> std::string {
    ErrorDescription desc;
    auto             pack = OutChain(desc);
    if (registry.Describe(code, pack.Head()).HasValue()) {
        return std::format("{}::{}: {}", std::string_view(desc.category), std::string_view(desc.name), std::string_view(desc.message));
    }
    return std::format("error {:#010x}:{}", code.CategoryHash(), code.Value());
}

[[nodiscard]] inline auto CreateErrorRegistry(const ChainInNode* config = nullptr) noexcept
    -> std::expected<Ref<IErrorRegistry>, ErrorCode> {
    const Result<IErrorRegistry*> created = CreateErrorRegistryRaw(config);
    if (!created.HasValue()) return std::unexpected(created.error);
    return Ref<IErrorRegistry>::Adopt(created.value);
}

} // namespace ZHLN::COM
