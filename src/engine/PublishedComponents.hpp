// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Zahlen/ECS/ComponentPolicy.hpp>
#include <ecs/ECS.hpp>
#include <engine/SceneResources.hpp>
#include <type_traits>

namespace ZHLN {

// Marks a fixed-layout native component as published on a Registry's COM
// boundary with its reflection read-only policy. Decoupled from resource
// retirement hooks (see BindRetireHooks).
//
// Membership is an explicit ABI commitment: every published component must be
// trivially copyable and standard layout so COM consumers share its offsets
// and LayoutHash<T>().
template <typename T>
void PublishComponent(ECS::Registry& registry) {
    static_assert(std::is_trivially_copyable_v<T>,
                  "a published component must be trivially copyable: the boundary copies it as bytes");
    static_assert(std::is_standard_layout_v<T>,
                  "a published component must be standard layout: the consumer models it from the same offsets");
    registry.SetPolicy<T>(ECS::ComponentPolicy {
        .published = true,
        .readOnly  = SceneResources::ReadOnlyReflective<T>,
    });
}

// Binds SceneResources::Release(ctx, component) as the retirement hook for
// every resource-owning component in Ts... without coupling PublishComponent
// to Engine or PhysicsContext.
template <typename... Ts, typename HookCtx>
void BindRetireHooks(ECS::Registry& registry, HookCtx& ctx) {
    (registry.SetRetireHook<Ts>(
         +[](void* rawCtx, Entity, void* component) noexcept {
             SceneResources::Release(*static_cast<HookCtx*>(rawCtx), *static_cast<Ts*>(component));
         },
         &ctx),
     ...);
}

} // namespace ZHLN
