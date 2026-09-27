// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once
#include "ShaderProgram.hpp"

#include "SpirvBindings.hpp"

#include <cstdint>
#include <span>
#include <utility>

namespace ZHLN::Vk {

template <typename... Slots>
constexpr auto BindingList<Slots...>::Spells(
    [[maybe_unused]] const SpirvBindings& declarations, [[maybe_unused]] const SpirvBinding& candidate, [[maybe_unused]] uint32_t set
) noexcept -> bool {
    return ((Slots::set == set && candidate.IsNamed(declarations.Bytes(), Slots::name)) || ...);
}


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

template <ShaderProgram Module>
[[nodiscard]] consteval auto HighestSetInModule() noexcept -> uint32_t {
    const uint32_t resources = HighestSetIn<typename Module::Resources>();
    const uint32_t samplers  = HighestSetIn<typename Module::Samplers>();
    return resources > samplers ? resources : samplers;
}

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

}

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
    constexpr uint32_t kHighest = TemplatedDetail::HighestSetInModule<Module>();
    if (head.HighestDeclaredSet() > kHighest) {
        return false;
    }
    return TemplatedDetail::SetMatchesBytes<Module, 0>(head) &&
           TemplatedDetail::HigherSetsMatch<Module>(head, bytes, std::make_index_sequence<static_cast<size_t>(kHighest)> {});
}

}
