// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/Engine/SpawnParams.hpp
//
// How a prefab is placed when it is instantiated. Split out of
// PrefabFactory.hpp so describing a spawn does not require the factory's
// Jolt shapes, ModelPrefab or font loader; PrefabFactory.hpp includes this
// file, so every existing spelling still resolves.
//
// A fixed layout of native values, which is what lets ZHLN::COM::IAssetLibrary take
// it by const reference across the shared-library boundary.

#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Render/Types.hpp>
#include <cstdint>
#include <type_traits>

namespace ZHLN::PrefabFactory {

struct SpawnParams {
    ZHLN::DVec3 position {};
    ZHLN::Quat   rotation = ZHLN::Quat::Identity();
    ZHLN::FVec3  scale {1.0f, 1.0f, 1.0f};

    bool     createPhysics   = false;
    bool     useBoxColliders = false;
    bool     isStaticPhysics = true;
    // Static bodies that are scripted per-frame by gameplay systems (doors,
    // buttons): created as Jolt kinematic bodies in the MOVING layer so their
    // pose can be teleported via SetKinematicPose. isStaticPhysics must stay
    // true so VisualInterpolationSystem leaves their TransformComponent alone.
    bool isKinematicPhysics = false;
    // Dynamic bodies are added to the physics system asleep (Jolt
    // EActivation::DontActivate). They wake when an active body or an
    // impulse/velocity change disturbs them. Static/kinematic bodies are
    // unaffected (they never activate anyway).
    bool startAsleep        = false;
    bool isAnimated         = false;
    uint32_t physicsCategory = 0xFFFFFFFF;
    uint32_t physicsMask     = 0xFFFFFFFF;

    bool emissiveVirtualLights = false;

    // OR'd into every part's MeshComponent flags on spawn: lets a scene
    // description route a whole prefab into a draw layer (e.g. the BSP
    // skybox's props into DrawFlags::Background) without forking the asset.
    DrawFlags drawFlags = DrawFlags::None;

    float        roughness = 0.5f;
    float        metallic  = 0.0f;
    ZHLN::FVec4 color {0.8f, 0.4f, 0.2f, -1.0f};

    Material materialOverride = {.pipeline = PipelineID::Invalid};
};

static_assert(std::is_standard_layout_v<SpawnParams> && std::is_trivially_copyable_v<SpawnParams>,
              "SpawnParams crosses the COM boundary by reference and must stay a fixed-layout value.");

} // namespace ZHLN::PrefabFactory
