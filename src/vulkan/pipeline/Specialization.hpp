// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

// src/vulkan/pipeline/Specialization.hpp
//
// Specialization constants without the hand-written table.
//
// Vulkan wants two things that are easy to get subtly wrong by hand: a map entry
// per constant -- constant_id, byte offset, byte size -- and a data blob the
// offsets address. Written by hand, every site repeats the offsets (offsetof),
// the ids (a counter that has to agree with the shader's declaration order) and
// the sizes (the field's type spelled a second time), and nothing connects the
// three to the struct the values actually live in.
//
// So the struct is the source and the table is derived: `T`'s fields, in
// declaration order, are constant_id 0, 1, 2... and each entry carries that
// field's offset and size. What is left for a call site is the struct, one walk,
// the variants, and the infos the builders take.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ZHLN::Vk {

/// The specialization constants one pipeline variant is built with, for the
/// struct `T` whose fields are the constants.
///
///     // reflection.slang declares ENABLE_SSR as constant_id 0 and ENABLE_RTR as
///     // constant_id 1, in that order:
///     struct ReflectionSpec {
///         int enableSSR = 0;
///         int enableRTR = 0;
///     };
///
///     Vk::Specialization<ReflectionSpec> spec;
///     Reflect::ForEachFieldInfo<ReflectionSpec>(spec);       // one entry per field
///
///     const std::array variants = {ReflectionSpec {.enableSSR = 0, .enableRTR = 0}, /* ... */};
///     const auto       infos    = spec.Infos(variants);      // one info per variant
///
/// The walk is a line at the call site rather than this header's constructor
/// body, and that is the load-bearing part of the design. A compiler without
/// -freflection compiles the renderer's sources through
/// `zahlen_transpile_sources`, which rewrites a `ForEachFieldInfo` call it can
/// *see* into the per-field calls it stands for -- so the walk has to be where
/// the transpiler looks. Hidden in a header, it would compile against the no-op
/// stand-in instead, and the result is not a build failure: the map would simply
/// be empty, every shader would keep its `= 1` default, and a variant toggle
/// would stop toggling. The same reasoning is why `PushConstantLayoutMatches`
/// (ShaderProgram.hpp) puts its walk behind `#if ZHLN_REFLECTION_AVAILABLE` and
/// says what such a build can honestly claim.
///
/// Two contracts, both of which the shader modules and the struct spell once:
///
///   * constant ids are positional. Field N is constant_id N, so the struct's
///     declaration order has to match the module's, which is the order its
///     `[[vk::constant_id(N)]]` declarations appear in;
///   * an entry whose id a module does not declare is ignored by the driver, so
///     a module with fewer constants than `T` has fields is fine -- which is how
///     the NoRT modules of lighting.slang and reflection.slang are built with
///     the same table as their RT counterparts.
///
/// What this does not do is keep the entries alive for you: `Info`/`Infos` point
/// at the values passed in and at this object's own entry table, so both outlive
/// the pipeline build they are handed to (they are stack values in the build
/// function, as the arrays they replace were).
template <typename T>
class Specialization {
    // The offsets the walk hands over are ABI offsets, so the struct has to be
    // one whose layout is its declaration order; and a struct with no fields
    // would build the empty map this type exists to make impossible.
    static_assert(std::is_standard_layout_v<T>, "Specialization<T> maps a field by its ABI offset: make T standard-layout");
    static_assert(!std::is_empty_v<T>, "Specialization<T> of a fieldless struct maps nothing, which silently keeps every default");

  public:
    Specialization() noexcept = default;

    /// Records one field as one constant, in field order. The walk calls this;
    /// `name` is the field's own and is not read, because Vulkan addresses these
    /// by id (the order) and has no name to check it against.
    template <typename FieldT>
    void operator()(std::string_view /*name*/, std::size_t offset) noexcept {
        Record(offset, static_cast<std::uint32_t>(sizeof(FieldT)));
    }

    /// The same one entry, for a caller that has an offset and a size and no
    /// field to walk through: a map filled by hand, or a test.
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

    /// The info for every variant in an array, which is the shape the pipeline
    /// builders take (`std::span<const VkSpecializationInfo>`): one table, one
    /// info per variant, and no loop at the call site.
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
