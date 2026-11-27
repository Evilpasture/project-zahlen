// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Render/AssetDefaults.hpp>
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
#include <variant>
#include <utility>
#include <vector>

namespace ZHLN {

// Retained as a source-compatible helper now that prefab matrices are native.
[[nodiscard]] inline auto PrefabMatrixToNative(const Mat4x4& source) noexcept -> Mat4x4 { return source; }

struct ModelNode {
    String64   name;
    int32_t    parentIndex    = -1;
    Mat4x4 localTransform = Mat4x4::Identity();
    bool       hasMesh        = false;
};

// Prefab-owned collider storage. Geometry and children live with the prefab;
// body creation borrows a wire view (TriangleMesh / Compound spans) into them,
// so a spawn never copies the collision data.
struct OwnedMesh {
    std::shared_ptr<const Physics::MeshGeometry> geometry;
};

struct OwnedCompound {
    std::vector<Physics::Placed<Physics::Box>>          boxes;
    std::vector<Physics::Placed<Physics::Sphere>>       spheres;
    std::vector<Physics::Placed<Physics::Capsule>>      capsules;
    std::vector<Physics::Placed<Physics::Cylinder>>     cylinders;
    std::vector<Physics::Placed<Physics::Plane>>        planes;
    std::vector<Physics::Placed<Physics::TriangleMesh>> meshes;
    std::vector<Physics::Placed<Physics::MeshAsset>>    meshAssets;
    // Mesh children only borrow spans into their source geometry; the owners
    // keep those snapshots alive for as long as the compound does.
    std::vector<std::shared_ptr<const Physics::MeshGeometry>> meshOwners;

    [[nodiscard]] auto View() const noexcept -> Physics::Compound {
        return Physics::Compound {
            .boxes      = boxes,
            .spheres    = spheres,
            .capsules   = capsules,
            .cylinders  = cylinders,
            .planes     = planes,
            .meshes     = meshes,
            .meshAssets = meshAssets,
        };
    }
};

// What a prefab part can hold as a collider: one wire primitive, owned mesh
// geometry, or an owned compound. std::monostate means "no collider".
using ModelCollider = std::variant<
    std::monostate, Physics::Box, Physics::Sphere, Physics::Capsule, Physics::Cylinder, Physics::Plane,
    Physics::HeightField, OwnedMesh, OwnedCompound>;

// Importer-owned collision data survives the temporary import buffers and is
// shared when model parts share a source primitive.
[[nodiscard]] inline auto MakePrefabMeshCollider(std::span<const VertexPosition> vertices, std::span<const uint32_t> indices)
    -> std::optional<ModelCollider> {
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
    return OwnedMesh {std::move(data)};
}

[[nodiscard]] inline auto MakePrefabBoxCollider(FVec3 halfExtents, FVec3 center = {}) -> ModelCollider {
    if (center == FVec3 {}) return Physics::Box {halfExtents};
    OwnedCompound compound;
    compound.boxes.push_back({Physics::Box {halfExtents}, center});
    return compound;
}

inline void AppendPrefabCollider(OwnedCompound& compound, const ModelCollider& source) {
    std::visit(
        [&](const auto& shape) {
            using S = std::remove_cvref_t<decltype(shape)>;
            if constexpr (std::is_same_v<S, OwnedCompound>) {
                compound.boxes.insert(compound.boxes.end(), shape.boxes.begin(), shape.boxes.end());
                compound.spheres.insert(compound.spheres.end(), shape.spheres.begin(), shape.spheres.end());
                compound.capsules.insert(compound.capsules.end(), shape.capsules.begin(), shape.capsules.end());
                compound.cylinders.insert(compound.cylinders.end(), shape.cylinders.begin(), shape.cylinders.end());
                compound.planes.insert(compound.planes.end(), shape.planes.begin(), shape.planes.end());
                compound.meshes.insert(compound.meshes.end(), shape.meshes.begin(), shape.meshes.end());
                compound.meshAssets.insert(compound.meshAssets.end(), shape.meshAssets.begin(), shape.meshAssets.end());
                compound.meshOwners.insert(compound.meshOwners.end(), shape.meshOwners.begin(), shape.meshOwners.end());
            } else if constexpr (std::is_same_v<S, OwnedMesh>) {
                if (shape.geometry) {
                    compound.meshOwners.push_back(shape.geometry);
                    compound.meshes.push_back(
                        {Physics::TriangleMesh {shape.geometry->vertices, shape.geometry->indices}, {}}
                    );
                } else {
                    // An unresolvable snapshot must keep failing the whole
                    // compound, exactly like the old null-mesh child.
                    compound.meshes.push_back({Physics::TriangleMesh {}, {}});
                }
            } else if constexpr (std::is_same_v<S, Physics::Box>) {
                compound.boxes.push_back({shape});
            } else if constexpr (std::is_same_v<S, Physics::Sphere>) {
                compound.spheres.push_back({shape});
            } else if constexpr (std::is_same_v<S, Physics::Capsule>) {
                compound.capsules.push_back({shape});
            } else if constexpr (std::is_same_v<S, Physics::Cylinder>) {
                compound.cylinders.push_back({shape});
            } else if constexpr (std::is_same_v<S, Physics::Plane>) {
                compound.planes.push_back({shape});
            } else if constexpr (std::is_same_v<S, Physics::HeightField>) {
                // Height fields cannot nest inside a compound; treat the
                // same as "nothing to append" (the old variant had no
                // height-field alternative at all).
            }
        },
        source
    );
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

    std::optional<ModelCollider> meshCollider;
    std::optional<ModelCollider> boxCollider;

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
    float        outerConeAngle = std::numbers::pi_v<float> * 0.25f; // For spot lights (radians)
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

    bool gpuResourcesReady = true;
    uint64_t gpuDeviceGeneration = 0;
    uint64_t gpuRevision = 0;

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
