// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Public Engine & Jolt Headers
// clang-format off
#include <Zahlen/Physics/Jolt.hpp>
#include <Zahlen/Engine/Internal/EngineAccess.hpp>
#include <Zahlen/Engine/Capabilities/AudioEngine.hpp>
#include <Zahlen/Engine/Capabilities/DebugDraw.hpp>
#include <Zahlen/Engine/Capabilities/GraphicsDevice.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
// clang-format on
#include <Zahlen/Audio/Audio.hpp>
#include <Zahlen/Audio/AudioTypes.hpp>
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/ECS/EntityCommandBuffer.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Engine/PrefabFactory.hpp>
#include <Zahlen/Math/Math3D.hpp>
#include <Zahlen/Math/Transform.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Math/Angles.hpp>
#include <Zahlen/Render/GpuEnums.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Render/Types.hpp>

// Standard Library Headers
#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <span>
#include <utility>
#include <vector>

#include <Zahlen/ECS/SystemParameters.hpp>

import ZHLN.Explosions;

namespace ZHLN {


// Immediate bare-entity creation requires exclusive world access. The caller
// prepares ExplosionSystem::Init at its phase boundary and supplies the
// world's existing command buffer; this system does not play it back.
using ExplosionUpdateQuery = ECS::ExclusiveQuery<ExplosionComponent&, CraterDecalComponent&, const Components::PendingDestroy,
                                                Components::LightComponent&, Components::TransformComponent&, Components::WorldTransformComponent&>;

void UpdateExplosions(ExplosionUpdateQuery query, ECS::ResMut<SceneData> scene, ECS::EntityCommandBuffer& commands, FrameDt dt);

} // namespace ZHLN
