// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <Animation/Context.hpp>
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

ZHLN_STATIC_API void Sync(COM::IEntities& entities, Context& anim);

ZHLN_STATIC_API void PublishResults(
    COM::IEntities&         entities,
    const Context& anim,
    bool (*skipRigidNodes)(const COM::IEntities&, Entity) = nullptr);

ZHLN_STATIC_API void Step(
    COM::IEntities&           entities,
    Context&         anim,
    float                     dt,
    const COM::IAssetLibrary* assets     = nullptr,
    COM::IGraphicsDevice*     graphics   = nullptr,
    COM::ISimulation*         simulation = nullptr,
    PoseUploadCallback        sink       = {});

ZHLN_STATIC_API void Step(COM::IEngine& engine, Context& anim, float dt);

struct Layer {
    Context context;

    void Tick(COM::IEngine& engine, float dt) {
        Step(engine, context, dt);
    }

    void Stop(COM::IEngine&) noexcept {
        context.Clear();
        context.InvalidateRigs();
    }
};

} // namespace ZHLN::Animation
