// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Render/GpuEnums.hpp>
#include <Zahlen/Render/RenderIDs.hpp>
#include <Zahlen/Render/SkeletalAnimation.hpp>
#include <Zahlen/Render/Types.hpp>
#include <Zahlen/Render/Vertex.hpp>
#include <Zahlen/Physics/PhysicsShapes.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace ZHLN {

// Retained as a source-compatible helper now that prefab matrices are native.
[[nodiscard]] inline auto PrefabMatrixToNative(const Mat4x4& source) noexcept -> Mat4x4 { return source; }

// Artistic boost for the engine's low-exposure scene preset, not a glTF
// requirement. Conformance imports use an explicit scale of 1 instead.
inline constexpr float    kGLTFEmissiveDisplayScale       = 100.0f;
inline constexpr uint32_t kGLTFDefaultMaxTextureDimension = 1024;

struct ModelNode {
    String64   name;
    int32_t    parentIndex    = -1;
    Mat4x4 localTransform = Mat4x4::Identity();
    bool       hasMesh        = false;
};

// Importer-owned collision data survives the temporary import buffers and is
// shared when model parts share a source primitive.
[[nodiscard]] inline auto MakePrefabMeshCollider(std::span<const VertexPosition> vertices, std::span<const uint32_t> indices)
    -> std::optional<Physics::AnyShape> {
    if (vertices.size() < 3 || indices.size() < 3 || indices.size() % 3 != 0) return {};
    bool nondegenerate = false;
    for (size_t i = 0; i < indices.size(); i += 3) {
        if (indices[i] >= vertices.size() || indices[i + 1] >= vertices.size() || indices[i + 2] >= vertices.size()) return {};
        nondegenerate |= indices[i] != indices[i + 1] && indices[i + 1] != indices[i + 2] && indices[i] != indices[i + 2];
    }
    if (!nondegenerate) return {};
    auto data = std::make_shared<Physics::MeshGeometry>();
    data->vertices.reserve(vertices.size());
    for (const auto& v: vertices) data->vertices.push_back({v.position[0], v.position[1], v.position[2]});
    data->indices.assign(indices.begin(), indices.end());
    return Physics::AnyShape(Physics::Shape<Physics::Mesh>(std::move(data)));
}

[[nodiscard]] inline auto MakePrefabBoxCollider(FVec3 halfExtents, FVec3 center = {}) -> Physics::AnyShape {
    Physics::Shape<Physics::Compound> compound;
    compound.Add(Physics::Shape<Physics::Box>(halfExtents), center);
    return Physics::AnyShape(std::move(compound));
}

inline void AppendPrefabCollider(Physics::Shape<Physics::Compound>& compound, const Physics::AnyShape& source) {
    std::visit([&](const auto& shape) {
        using S = std::remove_cvref_t<decltype(shape)>;
        if constexpr (std::is_same_v<S, Physics::Shape<Physics::Compound>>) {
            compound.children.insert(compound.children.end(), shape.children.begin(), shape.children.end());
        } else {
            compound.Add(shape);
        }
    }, source);
}

struct ModelPart {
    String64   name;
    AssetID    meshAsset     = InvalidAssetID;
    MaterialID materialAsset = InvalidMaterialID;
    Mesh       mesh;
    Material   defaultMaterial;

    Mat4x4 localTransform = Mat4x4::Identity();

    uint32_t jointOffset   = 0;
    bool     isSkinned     = false;
    int32_t  nodeIndex     = -1;
    int32_t  skeletonIndex = -1;

    uint32_t morphOffset      = 0;
    uint32_t activeMorphCount = 0;
    // The morph weights travel ModelPart -> MorphTargetComponent -> DrawParams
    // -> InstanceData; one spelling for the four lanes end to end.
    ZHLN::FVec4 defaultMorphWeights = {0.0f, 0.0f, 0.0f, 0.0f};

    float        boundingRadius = 1.0f;
    ZHLN::FVec3 localMin       = {0.0f, 0.0f, 0.0f};
    ZHLN::FVec3 localMax       = {0.0f, 0.0f, 0.0f};

    std::optional<Physics::AnyShape> meshCollider;
    std::optional<Physics::AnyShape> boxCollider;

    // Background-layer parts (Source's 3D-skybox scenery) carry
    // DrawFlags::Background so their instances composite behind the main
    // world instead of racing it in the shared depth buffer.
    DrawFlags drawFlags = DrawFlags::None;

    std::vector<CSGModifier> csgModifiers;
};

struct ModelLight {
    String64     name;
    int32_t      nodeIndex = -1;
    LightType    type      = LightType::Point;
    ZHLN::FVec3 color {1.0f, 1.0f, 1.0f};
    float        intensity      = 1.0f;                     // Candela (lm/sr) for point/spot, lux (lm/m^2) for directional
    float        range          = 0.0f;                     // 0.0f = infinite / default inverse-square falloff
    float        innerConeAngle = 0.0f;                     // For spot lights (radians)
    float        outerConeAngle = std::numbers::pi * 0.25f; // For spot lights (radians)
};

struct ModelPrefab {
    String256 virtualPath;

    // Import-time emissive conversion, retained so a GPU rebuild uses the
    // same material units and opt-in virtual lights can recover authored
    // emission. Procedural prefabs keep the engine's presentation default.
    float emissiveFactorScale = kGLTFEmissiveDisplayScale;
    // Kept with the prefab so device-lost texture rebuilds preserve the
    // importer's resolution choice instead of silently reverting to 1024.
    uint32_t maxTextureDimension = kGLTFDefaultMaxTextureDimension;

    std::vector<ModelPart>     parts;
    std::vector<ModelNode>     nodes;
    std::vector<Skeleton>      skeletons;
    std::vector<AnimationClip> animations;
    std::vector<ModelLight>    lights;

    ModelPrefab()  = default;
    ~ModelPrefab() = default;

    ModelPrefab(const ModelPrefab&)            = delete;
    ModelPrefab& operator=(const ModelPrefab&) = delete;
    ModelPrefab(ModelPrefab&&)                 = default;
    ModelPrefab& operator=(ModelPrefab&&)      = default;
};

} // namespace ZHLN
