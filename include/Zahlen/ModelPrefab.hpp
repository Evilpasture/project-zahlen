// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/ModelPrefab.hpp
#pragma once

#include <Zahlen/Core/String.hpp>
#include "SkeletalAnimation.hpp"
#include "Types.hpp"
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <vector>

namespace ZHLN {

/// Converts a glTF emissiveFactor into the engine's HDR units.
///
/// glTF says emissiveFactor is [0,1] and leaves anything brighter to
/// KHR_materials_emissive_strength, which most exporters never write. This
/// renderer, though, lives in absolute-ish HDR: the sun is 250 and blit.slang
/// tonemaps with `hdrColor *= 0.015`, so an emissiveFactor of 1.0 taken at
/// face value renders at about 10/255 -- a "neon" material comes out nearly
/// black, and nothing it emits ever reaches the bloom bright pass.
///
/// Babylon.js only looks right without this because it composites in LDR,
/// where 1.0 already means white. The scale is that unit conversion: it puts a
/// fully saturated emissiveFactor near the tonemapper's white point (ACES of
/// 100 * 0.015 is ~0.84), which is what the asset author saw in Babylon.
///
/// It applies to imported materials only. Material::emissiveFactor stays raw
/// HDR, so a programmatic CreateMaterial keeps meaning exactly what it says,
/// and KHR_materials_emissive_strength keeps its spec meaning -- a relative
/// multiplier on top of this.
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

} // namespace ZHLN
