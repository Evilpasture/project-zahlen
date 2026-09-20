// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/vulkan/pipeline/CatalogChecks.hpp
//
// The generated catalog, held to the modules it was generated from. ShaderBindings.hpp is
// written by tools/zshader out of SPIRV-Reflect's word about each cooked module, and nothing
// about that is taken on trust here: `ModuleMatchesBytes` walks the same module with the
// independent reader in SpirvBindings.hpp and fails the build when the two disagree, so a
// generator that reflects a module wrongly -- or a header a rebuild left stale -- cannot
// reach a descriptor write.
//
// A header of its own for one reason: cost. SpirvBindings.hpp plus the walking code below is
// ~600 lines that exactly one translation unit compiles (the generated ShaderBytecode.cpp,
// the only file with a module's bytes as a constant expression), while ShaderProgram.hpp is
// in every RHI and renderer include closure and in the PCH.

#pragma once
#include "ShaderProgram.hpp"

#include "SpirvBindings.hpp" // the independent reader

#include <cstdint>
#include <span>
#include <utility>

namespace ZHLN::Vk {

/// One module's list of slots, held to the module's own bytes: defined here because this is
/// the only place in the engine that names the reader's types (the declaration and the intent
/// live on `BindingList`). `[[maybe_unused]]` is what an empty pack costs GCC -- the fold is
/// the whole body, so a `BindingList<>` leaves `set` set and never read -- and it has to be
/// on the definition, which is the one GCC reads at instantiation.
template <typename... Slots>
constexpr auto BindingList<Slots...>::Spells(
    [[maybe_unused]] const SpirvBindings& declarations, [[maybe_unused]] const SpirvBinding& candidate, [[maybe_unused]] uint32_t set
) noexcept -> bool {
    return ((Slots::set == set && candidate.IsNamed(declarations.Bytes(), Slots::name)) || ...);
}

// Holding the generated catalog to the modules it was generated from

/// The execution model a stage is compiled to, as the number OpEntryPoint
/// carries (0 Vertex, 4 Fragment, 5 GLCompute, 5364 TaskEXT, 5365 MeshEXT).
[[nodiscard]] consteval auto ExecutionModelOf(VkShaderStageFlagBits stage) noexcept -> uint32_t {
    switch (stage) {
        case VK_SHADER_STAGE_VERTEX_BIT:
            return 0;
        case VK_SHADER_STAGE_FRAGMENT_BIT:
            return 4;
        case VK_SHADER_STAGE_COMPUTE_BIT:
            return 5;
        case VK_SHADER_STAGE_TASK_BIT_EXT:
            return 5364;
        case VK_SHADER_STAGE_MESH_BIT_EXT:
            return 5365;
        default:
            return 0xFFFFFFFFu;
    }
}

namespace TemplatedDetail {

/// True when the module's bytes declare this slot, in the half it belongs to.
/// A slot of another set is another set's parse to answer, so it is not this
/// one's to fail.
template <uint32_t Set, typename Slot, bool Sampler>
[[nodiscard]] consteval auto DeclaredIsInSet(const SpirvBindings& declarations) noexcept -> bool {
    if constexpr (Slot::set != Set) {
        return true;
    } else if constexpr (Sampler) {
        return declarations.DeclaresSampler(Slot::name);
    } else {
        return declarations.DeclaresResource(Slot::name);
    }
}

template <uint32_t Set, typename List, bool Sampler, size_t... Index>
[[nodiscard]] consteval auto EveryDeclaredSlotIsInSetAt(const SpirvBindings& declarations, std::index_sequence<Index...>) noexcept -> bool {
    return (DeclaredIsInSet<Set, std::tuple_element_t<Index, SlotsOfT<List>>, Sampler>(declarations) && ...);
}

/// The direction a stale or wrong generated list trips: every slot the tool
/// wrote down for this set has to be a binding the module's bytes declare in it.
template <uint32_t Set, typename List, bool Sampler>
[[nodiscard]] consteval auto EveryDeclaredSlotIsInSet(const SpirvBindings& declarations) noexcept -> bool {
    return EveryDeclaredSlotIsInSetAt<Set, List, Sampler>(declarations, std::make_index_sequence<std::tuple_size_v<SlotsOfT<List>>> {});
}

/// The highest set either of a module's lists names (0 when both are empty):
/// how far ModuleMatchesBytes has to walk.
template <typename List, size_t... Index>
[[nodiscard]] consteval auto HighestSetAt(std::index_sequence<Index...>) noexcept -> uint32_t {
    uint32_t highest = 0;
    ((highest = std::tuple_element_t<Index, SlotsOfT<List>>::set > highest ? std::tuple_element_t<Index, SlotsOfT<List>>::set : highest), ...);
    return highest;
}
template <typename List>
[[nodiscard]] consteval auto HighestSetIn() noexcept -> uint32_t {
    return HighestSetAt<List>(std::make_index_sequence<std::tuple_size_v<SlotsOfT<List>>> {});
}

/// The highest set either list of a module names: how far the walk goes.
template <ShaderProgram Module>
[[nodiscard]] consteval auto HighestSetInModule() noexcept -> uint32_t {
    const uint32_t resources = HighestSetIn<typename Module::Resources>();
    const uint32_t samplers  = HighestSetIn<typename Module::Samplers>();
    return resources > samplers ? resources : samplers;
}

/// One set of a module's declarations, held against the module's own bytes in
/// both directions: nothing the bytes declare in the set is missing from the
/// list, and nothing the list declares is missing from the bytes. The parse
/// arrives from the caller, so a module whose lists name one set is parsed once.
template <ShaderProgram Module, uint32_t Set>
[[nodiscard]] consteval auto SetMatchesBytes(const SpirvBindings& declarations) noexcept -> bool {
    if (!declarations.Complete()) {
        return false;
    }
    for (uint32_t i = 0; i < declarations.Count(); ++i) {
        const SpirvBinding& binding  = declarations[i];
        const bool          declared = binding.sampler ? Module::Samplers::Spells(declarations, binding, Set) :
                                                         Module::Resources::Spells(declarations, binding, Set);
        if (!declared) {
            return false;
        }
    }
    if (!EveryDeclaredSlotIsInSet<Set, typename Module::Resources, false>(declarations)) {
        return false;
    }
    return EveryDeclaredSlotIsInSet<Set, typename Module::Samplers, true>(declarations);
}

/// Every set above the first: set 0 came parsed from ModuleMatchesBytes, the rest are walked
/// here -- one extra parse for the one module family that spreads its bindings over two sets
/// (decal.slang), none for the other seventy. `bytes` is touched only when the pack is not
/// empty, so a set-0-only module instantiates to nothing more than `true`; `[[maybe_unused]]`
/// is what that costs under GCC's -Wunused-but-set-parameter.
template <ShaderProgram Module, size_t... Index>
[[nodiscard]] consteval auto HigherSetsMatch(
    const SpirvBindings& first, [[maybe_unused]] std::span<const uint8_t> bytes, std::index_sequence<Index...>
) noexcept -> bool {
    constexpr uint32_t kFirstOfTheRest = 1;
    return (SetMatchesBytes<Module, static_cast<uint32_t>(Index) + kFirstOfTheRest>(
                SpirvBindings::Parse(bytes, static_cast<uint32_t>(Index) + kFirstOfTheRest)
            ) &&
            ...);
}

} // namespace TemplatedDetail

/// True when everything the generated catalog says about `Module` -- entry point, stage,
/// bindings and their kinds -- is what its own bytes say, read by the independent parser
/// rather than by SPIRV-Reflect, which the tool used.
///
/// Called from the generated ShaderBytecode.cpp once per module with the `#embed`ded array in
/// hand: the only place a module's bytes are constant-expression data, and so the only place
/// this check can run.
template <ShaderProgram Module>
[[nodiscard]] consteval auto ModuleMatchesBytes(std::span<const uint8_t> bytes) noexcept -> bool {
    const SpirvBindings head = SpirvBindings::Parse(bytes, 0);
    if (!head.Complete() || head.EntryPointCount() != 1) {
        return false;
    }
    if (!head.IsEntryPoint(Module::EntryPoint)) {
        return false;
    }
    if (head.ExecutionModel() != ExecutionModelOf(Module::Stage)) {
        return false;
    }
    // Per set, both directions and both kinds: what the module declares in a set
    // and what the generated list says it declares in that set are the same set
    // of bindings, not merely overlapping ones. decal.slang is why this is not
    // "set 0": its vertex stage declares nothing at all in set 0 and its
    // fragment stage reads the scene block from set 1.
    constexpr uint32_t kHighest = TemplatedDetail::HighestSetInModule<Module>();
    // A set the module declares but no list names would go unchecked: the lists
    // have to reach at least as far as the module does.
    if (head.HighestDeclaredSet() > kHighest) {
        return false;
    }
    return TemplatedDetail::SetMatchesBytes<Module, 0>(head) &&
           TemplatedDetail::HigherSetsMatch<Module>(head, bytes, std::make_index_sequence<static_cast<size_t>(kHighest)> {});
}

} // namespace ZHLN::Vk
