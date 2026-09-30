// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Render/Handles.hpp>
#include <Zahlen/Render/Types.hpp>
#include "SkeletalAnimation.hpp"
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <vector>

namespace ZHLN {

// Artistic boost for the engine's low-exposure scene preset, not a glTF
// requirement. Conformance imports use an explicit scale of 1 instead.
inline constexpr float kGLTFEmissiveDisplayScale = 100.0f;

struct ModelNode {
    String64   name;
    int32_t    parentIndex    = -1;
    JPH::Mat44 localTransform = JPH::Mat44::sIdentity();
    bool       hasMesh        = false;
};

struct ModelPart {
    String64   name;
    AssetID    meshAsset     = InvalidAssetID;
    MaterialID materialAsset = InvalidMaterialID;
    Mesh       mesh;
    Material   defaultMaterial;

    JPH::Mat44 localTransform = JPH::Mat44::sIdentity();

    uint32_t jointOffset   = 0;
    bool     isSkinned     = false;
    int32_t  nodeIndex     = -1;
    int32_t  skeletonIndex = -1;

    uint32_t morphOffset            = 0;
    uint32_t activeMorphCount       = 0;
    float    defaultMorphWeights[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    float boundingRadius = 1.0f;
    float localMin[3]    = {0.0f, 0.0f, 0.0f};
    float localMax[3]    = {0.0f, 0.0f, 0.0f};

    JPH::ShapeRefC meshCollider = nullptr;
    JPH::ShapeRefC boxCollider  = nullptr;

    std::vector<CSGModifier> csgModifiers;
};

struct ModelPrefab {
    String256 virtualPath;

    // Import-time emissive conversion, retained so a GPU rebuild uses the
    // same material units and opt-in virtual lights can recover authored
    // emission. Procedural prefabs keep the engine's presentation default.
    float emissiveFactorScale = kGLTFEmissiveDisplayScale;

    std::vector<ModelPart>     parts;
    std::vector<ModelNode>     nodes;
    std::vector<Skeleton>      skeletons;
    std::vector<AnimationClip> animations;

    ModelPrefab()  = default;
    ~ModelPrefab() = default;

    ModelPrefab(const ModelPrefab&)            = delete;
    ModelPrefab& operator=(const ModelPrefab&) = delete;
    ModelPrefab(ModelPrefab&&)                 = default;
    ModelPrefab& operator=(ModelPrefab&&)      = default;
};

}
