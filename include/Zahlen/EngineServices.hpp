// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The engine's two service bundles, and the one door between them.
//
// A graph is built with the services of its domain: the simulation graph gets
// SimServices, the render graph gets RenderServices. A system's signature names
// the services it uses, and a system that asks for something its graph does not
// provide fails to *compile* (see SystemGraph<Services>::AddSystem). That is
// what stops this header's types from ending up on a shared, untyped carrier:
// there is nothing to put them on.

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Mat44.h>
#include <Zahlen/Common.h>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/PoseUploads.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ZHLN {

class RenderContext;
class AssetManager;
class PhysicsContext;
class AudioContext;
class ArticulationSystem;
struct ModelPrefab;

namespace ECS {
class Registry;
}

using BonePosePostProcessor = void (*)(
    ECS::Registry& registry, Entity rootEntity, const ModelPrefab& prefab, std::span<const JPH::Mat44> localTransforms, std::vector<JPH::Mat44>& worldTransforms
);

// Distinct parameter types for the two otherwise identically typed output
// lists. They are bundle members of the render domain, so a system resolves one
// by its type rather than by position.
struct VisibleEntities {
    JPH::Array<Entity>& values;
};
struct VisibleShadowEntities {
    JPH::Array<Entity>& values;
};

// What the simulation graph provides. References: the engine owns these for its
// whole life, so a system never sees a null service and a resolver never checks
// for one. The simulation domain has no renderer -- see PoseUploadQueue for the
// one channel it has out.
struct SimServices {
    PhysicsContext& physics;
    AudioContext&   audio;
    AssetManager&   assets;

    ArticulationSystem&    articulation;
    BonePosePostProcessor& bonePosePostProcessor;

    // The simulation domain's channel *out*: poses it produces are pushed here
    // and uploaded by the renderer. Nothing in this bundle is a renderer, and no
    // renderer type appears in a simulation system's signature.
    PoseUploadQueue& poseUploads;
};

// What the render graph provides. `render` is here and *only* here: a
// simulation system cannot name it, which is the point.
struct RenderServices {
    RenderContext& render;
    AssetManager&  assets;

    VisibleEntities       visible;
    VisibleShadowEntities shadow;
};

} // namespace ZHLN
