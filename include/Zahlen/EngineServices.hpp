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
struct SceneData;
class AssetManager;
class PhysicsContext;
class AudioContext;
class ArticulationSystem;
struct ModelPrefab;

namespace ECS {
class Registry;
}

// Transform storage is callback-scoped scratch and may be reused for the next
// animator. The callback may mutate worldTransforms, but must not retain either
// the span or references/pointers into the vector after it returns.
using BonePosePostProcessor = void (*)(
    ECS::Registry& registry, Entity rootEntity, const ModelPrefab& prefab, std::span<const JPH::Mat44> localTransforms, std::span<JPH::Mat44> worldTransforms
);

// The pose pipeline's extension point: every post-processor the animation system
// runs per skinned root, between pose evaluation and joint upload.
//
// It is a list rather than the single slot it replaces for two reasons. Two
// modifiers that both want to adjust a pose (an IK solve and a cloth
// approximation, say) had to fight over one function pointer, and whichever
// installed second silently won. And the slot was read *by value* when the graphs
// were built, so a processor installed after InitializeDefaultScene was copied
// into the services bundle before it existed and then never ran -- a silent
// no-op with no diagnostic. A list the animation system walks through a
// reference has neither failure mode.
//
// Why this is not a SimGraph node, which is what it looks like it wants to be:
// the callback runs *inside* AnimationSystem's parallel per-chunk evaluation,
// with chunk-scoped scratch, between the local -> world solve and the joint
// upload for one root. A later graph node cannot see that intermediate state
// without the system publishing every root's joint matrices, and the upload
// happens in the same pass. If the pose pipeline is ever split so evaluation and
// upload are separate nodes, this becomes an ordinary system and this type goes
// away.
class PosePostProcessorPipeline {
  public:
    void Add(BonePosePostProcessor processor) {
        if (processor != nullptr) {
            _processors.push_back(processor);
        }
    }

    // In insertion order. A processor that yields does not disturb the walk:
    // another task on the same worker cannot add to the list, because
    // installation is composition-time and this is the simulation phase.
    void Apply(
        ECS::Registry& registry, Entity rootEntity, const ModelPrefab& prefab, std::span<const JPH::Mat44> localTransforms,
        std::span<JPH::Mat44> worldTransforms
    ) const {
        for (const BonePosePostProcessor processor: _processors) {
            processor(registry, rootEntity, prefab, localTransforms, worldTransforms);
        }
    }

    [[nodiscard]] auto Count() const noexcept -> size_t { return _processors.size(); }
    [[nodiscard]] auto IsEmpty() const noexcept -> bool { return _processors.empty(); }
    void               Clear() noexcept { _processors.clear(); }

  private:
    std::vector<BonePosePostProcessor> _processors;
};

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
    // Engine-owned and bound by reference, so a processor installed after the
    // graphs were built still runs: only the list's address is captured.
    PosePostProcessorPipeline& posePostProcessors;

    // The simulation domain's channel *out*: poses it produces are pushed here
    // and uploaded by the renderer. Nothing in this bundle is a renderer, and no
    // renderer type appears in a simulation system's signature.
    PoseUploadQueue& poseUploads;
};

// What the render graph provides. `render` is here and *only* here: a
// simulation system cannot name it, which is the point.
struct RenderServices {
    RenderContext& render;
    SceneData&     scene;
    AssetManager&  assets;

    VisibleEntities       visible;
    VisibleShadowEntities shadow;
};

} // namespace ZHLN
