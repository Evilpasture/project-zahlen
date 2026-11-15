// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Match the module's global-fragment dependencies before import; GCC's module
// reader requires these declarations to have the same global attachment.
#include <Zahlen/Common.h>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Render/RenderIDs.hpp>
#include <cstdint>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/ECS/SystemParameters.hpp>
#include <Zahlen/Engine/Capabilities/AudioEngine.hpp>
#include <Zahlen/Engine/Capabilities/GraphicsDevice.hpp>
#include <Zahlen/Engine/Capabilities/Entities.hpp>

import ZHLN.Lightning;

namespace ZHLN::Lightning {

// Despawn expands PendingDestroy across the hierarchy immediately, so the
// entire world is a write hazard, and graphics/audio work here has no place in
// a simulation node's hazard graph. The update therefore runs at an externally
// ordered phase -- Step() below -- which hands it the capabilities directly
// rather than routing them through the extension store. A SimGraph node cannot
// obtain GraphicsDevice or AudioEngine: they are bundle members of other
// domains, and SimServices carries them for rendering, not for systems to pull.
using LightningUpdateQuery = ECS::ExclusiveQuery<LightningComponent&, const Components::PendingDestroy,
                                                const Components::CameraComponent, Components::LightComponent&>;

void Update(LightningUpdateQuery query, ECS::ResMut<GraphicsDevice> graphics, ECS::ResMut<AudioEngine> audio,
            ECS::ResMut<Entities> entities, FrameDt dt);

void Step(Engine& engine, float dt);

} // namespace ZHLN::Lightning
