// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <Animation/AnimationContext.hpp>
#include <Animation/Components.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/ECS/Entity.hpp>

namespace ZHLN::COM {
struct IEngine;
struct IEntities;
struct IAssetLibrary;
struct IGraphicsDevice;
struct ISimulation;
} // namespace ZHLN::COM

namespace ZHLN::Animation {

ZHLN_STATIC_API void Sync(COM::IEntities& entities, AnimationContext& anim);

ZHLN_STATIC_API void PublishResults(
    COM::IEntities&         entities,
    const AnimationContext& anim,
    bool (*skipRigidNodes)(const COM::IEntities&, Entity) = nullptr);

ZHLN_STATIC_API void Step(
    COM::IEntities&           entities,
    AnimationContext&         anim,
    float                     dt,
    const COM::IAssetLibrary* assets     = nullptr,
    COM::IGraphicsDevice*     graphics   = nullptr,
    COM::ISimulation*         simulation = nullptr,
    PoseUploadCallback        sink       = {});

ZHLN_STATIC_API void Step(COM::IEngine& engine, AnimationContext& anim, float dt);

struct Layer {
    AnimationContext context;

    void Tick(COM::IEngine& engine, float dt) {
        Step(engine, context, dt);
    }

    void Stop(COM::IEngine&) noexcept {
        context.Clear();
        context.InvalidateRigs();
    }
};

} // namespace ZHLN::Animation
