// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

// src/vulkan/pipeline/Specialization.hpp
//
// Specialization constants without the hand-written table. Vulkan wants a map entry per
// constant (constant_id, byte offset, byte size) plus a data blob the offsets address; by
// hand every site repeats the offsets, the ids and the sizes, with nothing connecting them
// to the struct the values live in. So the struct is the source and the table is derived:
// `T`'s fields in declaration order are constant_id 0, 1, 2..., each entry carrying that
// field's offset and size.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ZHLN::Vk {

/// The specialization constants one pipeline variant is built with, for the struct `T`
/// whose fields are the constants.
///
///     // reflection.slang declares ENABLE_SSR as constant_id 0, ENABLE_RTR as 1:
///     struct ReflectionSpec { int enableSSR = 0; int enableRTR = 0; };
///
///     Vk::Specialization<ReflectionSpec> spec;
///     Reflect::ForEachFieldInfo<ReflectionSpec>(spec);       // one entry per field
///     const std::array variants = {ReflectionSpec {.enableSSR = 0, .enableRTR = 0}, /* ... */};
///     const auto       infos    = spec.Infos(variants);      // one info per variant
///
/// The walk is a line at the call site rather than this header's constructor body, and that
/// is load-bearing: without -freflection the renderer's sources go through
/// `zahlen_transpile_sources`, which rewrites a `ForEachFieldInfo` call it can *see*. Hidden
/// in a header it would compile against the no-op stand-in, and the failure is silent -- an
/// empty map, every shader keeping its default, a variant toggle that stops toggling.
///
/// Two contracts, spelled once each by the modules and the struct:
///
///   * constant ids are positional: field N is constant_id N, so the struct's declaration
///     order must match the module's `[[vk::constant_id(N)]]` order;
///   * an entry whose id a module does not declare is ignored by the driver, so a module
///     with fewer constants than `T` has fields is fine -- how lighting.slang's and
///     reflection.slang's NoRT modules are built with their RT counterparts' table.
///
/// `Info`/`Infos` point at the values passed in and at this object's entry table, so both
/// must outlive the pipeline build they are handed to.
template <typename T>
class Specialization {
    // The offsets the walk hands over are ABI offsets, so the struct's layout must be its
    // declaration order; a fieldless struct would build the empty map this type exists to
    // make impossible.
    static_assert(std::is_standard_layout_v<T>, "Specialization<T> maps a field by its ABI offset: make T standard-layout");
    static_assert(!std::is_empty_v<T>, "Specialization<T> of a fieldless struct maps nothing, which silently keeps every default");

  public:
    Specialization() noexcept = default;

    /// Records one field as one constant, in field order; called by the walk. `name` is not
    /// read: Vulkan addresses these by id (the order) and has no name to check it against.
    template <typename FieldT>
    void operator()(std::string_view /*name*/, std::size_t offset) noexcept {
        Record(offset, static_cast<std::uint32_t>(sizeof(FieldT)));
    }

    /// The same single entry for a caller with an offset and a size but no field to walk: a
    /// map filled by hand, or a test.
    void Record(std::size_t offset, std::uint32_t size) noexcept {
        entries.push_back(
            VkSpecializationMapEntry {
                .constantID = static_cast<std::uint32_t>(entries.size()),
                .offset     = static_cast<std::uint32_t>(offset),
                .size       = size,
            }
        );
    }

    /// The info for one variant: this entry table, and that variant's bytes.
    [[nodiscard]] auto Info(const T& value) const noexcept -> VkSpecializationInfo {
        return {
            .mapEntryCount = static_cast<std::uint32_t>(entries.size()),
            .pMapEntries   = entries.data(),
            .dataSize      = sizeof(T),
            .pData         = &value,
        };
    }

    /// The info for every variant in an array, the shape the pipeline builders take: one
    /// table, one info per variant, no loop at the call site.
    template <std::size_t N>
    [[nodiscard]] auto Infos(const std::array<T, N>& values) const noexcept -> std::array<VkSpecializationInfo, N> {
        std::array<VkSpecializationInfo, N> infos {};
        for (std::size_t i = 0; i < N; ++i) {
            infos[i] = Info(values[i]);
        }
        return infos;
    }

    [[nodiscard]] auto Entries() const noexcept -> std::span<const VkSpecializationMapEntry> {
        return entries;
    }
    [[nodiscard]] auto Empty() const noexcept -> bool {
        return entries.empty();
    }
    void Clear() noexcept {
        entries.clear();
    }

  private:
    std::vector<VkSpecializationMapEntry> entries;
};

} // namespace ZHLN::Vk
