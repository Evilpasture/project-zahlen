// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/vulkan/pipeline/CatalogChecks.hpp
//
// The generated catalog, held to the modules it was generated from. ShaderBindings.hpp is
// written by tools/zshader out of Slang's own reflection -- the linked layout plus the
// entry point's usage table -- and nothing about that is taken on trust here:
// `ModuleMatchesBytes` walks the same module with the independent reader in
// SpirvBindings.hpp and fails the build when the two disagree in any way the runtime cannot
// absorb, so a generator that drops a module's binding -- or a header a rebuild left stale
// -- cannot reach a descriptor write.
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

// One module's list of slots, held to the module's own bytes: defined here because this is
// the only place in the engine that names the reader's types (the declaration and the intent
// live on `BindingList`). `[[maybe_unused]]` is what an empty pack costs GCC -- the fold is
// the whole body, so a `BindingList<>` leaves `set` set and never read -- and it has to be
// on the definition, which is the one GCC reads at instantiation.
template <typename... Slots>
constexpr auto BindingList<Slots...>::Spells(
    [[maybe_unused]] const SpirvBindings& declarations, [[maybe_unused]] const SpirvBinding& candidate, [[maybe_unused]] uint32_t set
) noexcept -> bool {
    return ((Slots::set == set && candidate.IsNamed(declarations.Bytes(), Slots::name)) || ...);
}

// Holding the generated catalog to the modules it was generated from

// The execution model a stage is compiled to, as the number OpEntryPoint
// carries (0 Vertex, 4 Fragment, 5 GLCompute, 5364 TaskEXT, 5365 MeshEXT).
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

// The highest set either of a module's lists names (0 when both are empty):
// how far ModuleMatchesBytes has to walk.
template <typename List, size_t... Index>
[[nodiscard]] consteval auto HighestSetAt(std::index_sequence<Index...>) noexcept -> uint32_t {
    uint32_t highest = 0;
    [&]<typename... Slots>(std::type_identity<std::tuple<Slots...>>) {
        ((highest = Slots...[Index]::set > highest ? Slots...[Index]::set : highest), ...);
    }(std::type_identity<SlotsOfT<List>> {});
    return highest;
}
template <typename List>
[[nodiscard]] consteval auto HighestSetIn() noexcept -> uint32_t {
    return HighestSetAt<List>(std::make_index_sequence<std::tuple_size_v<SlotsOfT<List>>> {});
}

// The highest set either list of a module names: how far the walk goes.
template <ShaderProgram Module>
[[nodiscard]] consteval auto HighestSetInModule() noexcept -> uint32_t {
    const uint32_t resources = HighestSetIn<typename Module::Resources>();
    const uint32_t samplers  = HighestSetIn<typename Module::Samplers>();
    return resources > samplers ? resources : samplers;
}

// One set of a module's declarations, held against the module's own bytes:
// nothing the bytes declare in the set is missing from the list. The parse
// arrives from the caller, so a module whose lists name one set is parsed
// once. The list may name more than the bytes -- see ModuleMatchesBytes.
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
    return true;
}

// Every set above the first: set 0 came parsed from ModuleMatchesBytes, the rest are walked
// here -- one extra parse for the one module family that spreads its bindings over two sets
// (decal.slang), none for the other seventy. `bytes` is touched only when the pack is not
// empty, so a set-0-only module instantiates to nothing more than `true`; `[[maybe_unused]]`
// is what that costs under GCC's -Wunused-but-set-parameter.
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

// True when the generated catalog covers what the module's own bytes say -- entry point,
// stage, and every binding the parser sees listed with its name under the same set -- read
// by the independent parser rather than by anything the tool consulted (the tool only asks
// Slang).
//
// The catalog may hold more slots than the module binds, and does: the generator keeps what
// Slang's usage table marks used, and that table is collected before the SPIR-V emitter's
// last pass, which can still drop a parameter fed only to legalized-away code
// (basic_mesh.slang's forward entry keeps scene.g_prevJoints listed and not bound). Such a
// slot costs nothing to keep -- the descriptor heaps are written from the full declaration
// set regardless, the pipeline layouts come from these bytes at runtime, and a binding no
// code reads is never read. A slot the bytes declare and the list lost is the hole this
// check exists for: it would leave the shader reading a heap entry no write was planned
// for, and it fails the build.
//
// Called from the generated ShaderBytecode.cpp once per module with the `#embed`ded array in
// hand: the only place a module's bytes are constant-expression data, and so the only place
// this check can run.
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
    // Per set and per kind: everything the module declares in a set is in the
    // generated list for that set, named the same way. decal.slang is why this
    // is not "set 0": its vertex stage declares nothing at all in set 0 and
    // its fragment stage reads the scene block from set 1.
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
