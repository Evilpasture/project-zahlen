// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderConstants.hpp"
#include "graph/PassExecution.hpp"
#include <vkd3d/graph/PassDecl.hpp>
#include <Zahlen/Core/Reflection/Enums.hpp>

namespace ZHLN::Passes::TemplatedDetail {

// Concatenates VkD3D::TypeList usage lists so templated pass wrappers can assemble
// conditional resource packs into one flat VkD3D::RenderPass base.
template <typename... Lists>
struct Concat;
template <>
struct Concat<> {
    using type = VkD3D::TypeList<>;
};
template <typename... Usages>
struct Concat<VkD3D::TypeList<Usages...>> {
    using type = VkD3D::TypeList<Usages...>;
};
template <typename... A, typename... B, typename... Rest>
struct Concat<VkD3D::TypeList<A...>, VkD3D::TypeList<B...>, Rest...> {
    using type = typename Concat<VkD3D::TypeList<A..., B...>, Rest...>::type;
};

// VkD3D::RenderPass<Name, Usages...> given the usages already collected in a
// TypeList, so a template can assemble its pack from conditional pieces and
// still inherit the one base the frame graph recognises.
template <VkD3D::ResourceName Name, typename UsageList>
struct RenderPassOf;
template <VkD3D::ResourceName Name, typename... Usages>
struct RenderPassOf<Name, VkD3D::TypeList<Usages...>>: VkD3D::RenderPass<Name, Usages...> {
};

// CompileTimeFrameGraph::WriteScopeStart times a pass by looking its name up
// with Reflect::StringToEnum<Stage>; a name the enum does not carry runs with
// no query and no row, silently. Every templated pass asserts its name here so
// a renamed instantiation fails to compile instead of disappearing from the
// profiler. The no-reflection fallback of StringToEnum always misses, so the
// check is native-reflection only.
template <VkD3D::ResourceName Name>
inline constexpr bool kNameProfilesAsStage = Reflect::ReflectionAvailable || Reflect::StringToEnum<Stage>(Name.string_view()).has_value();

} // namespace ZHLN::Passes::TemplatedDetail
