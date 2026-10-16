// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderConstants.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes::TemplatedDetail {

// Concatenates Vk::TypeList usage lists. The pass templates below build their
// usage list conditionally (a history/velocity pair that exists only for the
// temporal denoisers), and Vk::RenderPass wants the usages as one flat pack.
template <typename... Lists>
struct Concat;
template <>
struct Concat<> {
    using type = Vk::TypeList<>;
};
template <typename... Usages>
struct Concat<Vk::TypeList<Usages...>> {
    using type = Vk::TypeList<Usages...>;
};
template <typename... A, typename... B, typename... Rest>
struct Concat<Vk::TypeList<A...>, Vk::TypeList<B...>, Rest...> {
    using type = typename Concat<Vk::TypeList<A..., B...>, Rest...>::type;
};

// Vk::RenderPass<Name, Usages...> given the usages already collected in a
// TypeList, so a template can assemble its pack from conditional pieces and
// still inherit the one base the frame graph recognises.
template <Vk::ResourceName Name, typename UsageList>
struct RenderPassOf;
template <Vk::ResourceName Name, typename... Usages>
struct RenderPassOf<Name, Vk::TypeList<Usages...>>: Vk::RenderPass<Name, Usages...> {
};

// CompileTimeFrameGraph::WriteScopeStart times a pass by looking its name up
// with Reflect::StringToEnum<Stage>; a name the enum does not carry runs with
// no query and no row, silently. Every templated pass asserts its name here so
// a renamed instantiation fails to compile instead of disappearing from the
// profiler. The no-reflection fallback of StringToEnum always misses, so the
// check is native-reflection only.
template <Vk::ResourceName Name>
inline constexpr bool kNameProfilesAsStage = Reflect::ReflectionAvailable || Reflect::StringToEnum<Stage>(Name.string_view()).has_value();

} // namespace ZHLN::Passes::TemplatedDetail
