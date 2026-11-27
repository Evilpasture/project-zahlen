// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PrefabFactoryInternal.hpp"
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Math/Transform.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Render/SkeletalAnimation.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <algorithm>
#include <cstddef>
#include <ecs/ECS.hpp>
#include <engine/AssetManager.hpp>
#include <engine/Engine.hpp>
#include <engine/ModelPrefab.hpp>
#include <engine/PrefabFactory.hpp>
#include <engine/SceneResources.hpp>
#include <optional>
#include <physics/Physics.hpp>
#include <render/Render.hpp>
#include <span>
#include <utility>
#include <vector>

namespace ZHLN::PrefabFactory {

namespace {

[[nodiscard]] auto NativeModelMatrix(const Mat4x4& source) -> Mat4x4 {
    return source;
}

[[nodiscard]] auto SpawnLocalTransform(const SpawnParams& params) -> Components::TransformComponent {
    return {
        .position = {static_cast<float>(params.position.x), static_cast<float>(params.position.y), static_cast<float>(params.position.z)},
        .rotation = params.rotation,
        .scale    = params.scale
    };
}

[[nodiscard]] auto SpawnMatrix(const SpawnParams& params) -> Mat4x4 {
    const auto transform = SpawnLocalTransform(params);
    return ComposeTransform(transform.position, transform.rotation, transform.scale);
}

auto SpawnPrefabRoot(ECS::Registry& reg, std::string_view vPath, const SpawnParams& p) -> Entity {
    const auto root     = reg.Create();
    const auto localMat = SpawnMatrix(p);
    reg.Add(root, SpawnLocalTransform(p));
    reg.Add(root, Components::WorldTransformComponent {.world = localMat, .previous = localMat});
    reg.Add(root, Components::NameComponent {.name = String64("Root_" + std::string(vPath))});
    return root;
}

auto GetNodeLogicalTransform(const ModelPrefab& prefab, int32_t nodeIndex) -> Mat4x4 {
    // A standalone registered part need not have a logical node. -1 is the
    // descriptor's explicit no-node sentinel, not an index into an empty tree.
    if (nodeIndex < 0)
        return Mat4x4::Identity();
    auto    matrix    = NativeModelMatrix(prefab.nodes[nodeIndex].localTransform);
    int32_t parentIdx = prefab.nodes[nodeIndex].parentIndex;

    while (parentIdx >= 0) {
        matrix    = NativeModelMatrix(prefab.nodes[parentIdx].localTransform) * matrix;
        parentIdx = prefab.nodes[parentIdx].parentIndex;
    }
    return matrix;
}

struct PreparedPart {
    FVec3                    translation {};
    Quat                     rotation = Quat::Identity();
    FVec3                    scale {};
    float                    maxScale = 1.0f;
    const ModelCollider*    shape    = nullptr;
};

auto PreparePrefabPhysics(const ModelPrefab& prefab, const Mat4x4& baseTransform, bool createPhysics, bool useBoxColliders) -> std::vector<PreparedPart> {
    std::vector<PreparedPart> outPrepared(prefab.parts.size());

    TaskSystem::ParallelFor(prefab.parts.size(), 16, [&](uint32_t start, uint32_t end, uint32_t) -> void {
        for (uint32_t i = start; i < end; ++i) {
            const auto& part = prefab.parts[i];
            auto&       prep = outPrepared[i];

            const auto nodeWorld  = GetNodeLogicalTransform(prefab, part.nodeIndex);
            const auto finalLocal = baseTransform * nodeWorld * NativeModelMatrix(part.localTransform);
            const auto trs        = DecomposeTransform(finalLocal);

            prep.scale       = trs.scale;
            prep.rotation    = trs.rotation;
            prep.translation = trs.translation;
            prep.maxScale    = std::max({std::abs(trs.scale.x), std::abs(trs.scale.y), std::abs(trs.scale.z)});

            if (createPhysics) {
                const auto& shape = useBoxColliders ? part.boxCollider : part.meshCollider;
                if (shape) {
                    prep.shape = &*shape;
                }
            }
        }
    });
    return outPrepared;
}

auto InstantiateMeshPart(
    RenderContext&                         gfx,
    ECS::Registry&                         reg,
    PhysicsContext&                        pc,
    const ModelPrefab&                     prefab,
    const ModelPart&                       part,
    const PreparedPart&                    prep,
    const SpawnParams&                     params,
    Entity                                 rootEntity,
    std::unordered_map<int32_t, uint32_t>& allocatedSkeletons
) -> Entity {
    const auto baseTransform = SpawnMatrix(params);

    AssetID    meshAsset = part.meshAsset;
    MaterialID matAsset  = params.materialOverride.pipeline != PipelineID::Invalid ? static_cast<uint64_t>(params.materialOverride.pipeline) :
                                                                                     part.materialAsset;

    Material activeMat = params.materialOverride.pipeline != PipelineID::Invalid ? params.materialOverride : part.defaultMaterial;

    if (part.mesh.posBuffer != BufferID::Invalid) {
        gfx.RegisterGPUMesh(meshAsset, part.mesh);
    }
    if (activeMat.pipeline != PipelineID::Invalid) {
        gfx.RegisterGPUMaterial(matAsset, activeMat);
    }

    uint32_t assignedJointOffset = 0;
    if (part.isSkinned && params.isAnimated && part.skeletonIndex >= 0) {
        auto it = allocatedSkeletons.find(part.skeletonIndex);
        if (it != allocatedSkeletons.end()) {
            assignedJointOffset = it->second;
            // Every part sharing this skeleton holds one allocator ref, so
            // SceneResources releasing per-component cannot double-free.
            if (!gfx.RetainJoints(assignedJointOffset)) {
                return Entity::Null();
            }
        } else {
            assignedJointOffset = gfx.AllocateJoints(static_cast<uint32_t>(prefab.skeletons[part.skeletonIndex].joints.size()));
            if (assignedJointOffset == UINT32_MAX) {
                return Entity::Null();
            }
            allocatedSkeletons[part.skeletonIndex] = assignedJointOffset;
        }
    }

    Entity    e     = reg.Create();
    DrawFlags flags = DrawFlags::None;
    flags |= part.drawFlags;
    flags |= params.drawFlags;
    if (part.isSkinned && params.isAnimated) {
        flags |= DrawFlags::Skinned;
    }
    // Both alpha blending and optical transmission are composited in the
    // forward pass. A masked transmissive surface is still forward-only.
    if (activeMat.alphaMode == 2 || (!activeMat.unlit && activeMat.transmissionFactor > 0.0f)) {
        flags |= DrawFlags::ExcludeFromTLAS;
    }

    const auto worldMat = ComposeTransform(prep.translation, prep.rotation, prep.scale);

    auto body = Physics::BodyID::Null();
    if (params.createPhysics && prep.shape != nullptr) {
        // Collider -> runtime shape first, then the role decides whether the
        // shape family is allowed (a dynamic model with a mesh collider has
        // no legal role and spawns nothing, exactly as Jolt would have
        // rejected it before). compoundNodes holds the stamped children of an
        // OwnedCompound across the body-creation call below.
        std::vector<COM::ChainInNode> compoundNodes;
        auto colliderShape = std::visit(
            [&compoundNodes](const auto& collider) -> std::optional<Physics::ShapeVariant> {
                using C = std::remove_cvref_t<decltype(collider)>;
                if constexpr (std::is_same_v<C, std::monostate>) {
                    return std::nullopt;
                } else if constexpr (std::is_same_v<C, OwnedMesh>) {
                    if (!collider.geometry) return std::nullopt;
                    return Physics::ShapeVariant {
                        Physics::TriangleMesh {collider.geometry->vertices, collider.geometry->indices}};
                } else if constexpr (std::is_same_v<C, OwnedCompound>) {
                    return Physics::ShapeVariant {collider.View(compoundNodes)};
                } else {
                    return Physics::ShapeVariant {collider};
                }
            },
            *prep.shape
        );
        // Compound children carry a role policy (statics accept planes and
        // meshes; moving bodies convex + compound only). Reject a misfitting
        // child here, before any shape reaches Jolt.
        if (colliderShape.has_value() && std::holds_alternative<Physics::Compound>(*colliderShape)) {
            const Physics::Compound& compound = std::get<Physics::Compound>(*colliderShape);
            const bool               allowed  = (params.isStaticPhysics && !params.isKinematicPhysics)
                                    ? Physics::ValidateCompound<
                                          Physics::Box, Physics::Sphere, Physics::Capsule, Physics::Cylinder,
                                          Physics::Plane, Physics::TriangleMesh, Physics::MeshAsset, Physics::Compound>(
                                          compound)
                                    : Physics::ValidateCompound<Physics::Box, Physics::Sphere, Physics::Capsule,
                                                                Physics::Cylinder, Physics::Compound>(compound);
            if (!allowed) colliderShape.reset();
        }
        if (colliderShape.has_value()) {
            body = std::visit(
                [&]<typename S>(const S& shape) -> Physics::BodyID {
                    const Physics::CollisionFilter filter {.category = params.physicsCategory, .mask = params.physicsMask};
                    const Physics::StaticBody       staticRole {
                        .position = prep.translation.Cast<double>(), .rotation = prep.rotation, .scale = prep.scale};
                    const Physics::KinematicBody     kinematicRole {
                        .position = prep.translation.Cast<double>(), .rotation = prep.rotation, .scale = prep.scale};
                    const Physics::DynamicBody       dynamicRole {
                        .position   = prep.translation.Cast<double>(),
                        .rotation   = prep.rotation,
                        .scale      = prep.scale,
                        .startAsleep = params.startAsleep};
                    if constexpr (Physics::StaticShape<S>) {
                        if (params.isStaticPhysics && !params.isKinematicPhysics) {
                            return Physics::BodyID(pc.CreateBody(shape, staticRole, filter));
                        }
                    }
                    if constexpr (Physics::DynamicShape<S>) {
                        if (params.isStaticPhysics && params.isKinematicPhysics) {
                            return Physics::BodyID(pc.CreateBody(shape, kinematicRole, filter));
                        }
                        if (!params.isStaticPhysics) {
                            return Physics::BodyID(pc.CreateBody(shape, dynamicRole, filter));
                        }
                    }
                    return Physics::BodyID::Null();
                },
                *colliderShape
            );
        }
    }
    if (body != Physics::BodyID::Null()) {
        reg.Add(e, Components::TransformComponent {.position = prep.translation, .rotation = prep.rotation, .scale = prep.scale});
        reg.Add(e, Components::WorldTransformComponent {.world = worldMat, .previous = worldMat});
        reg.Add(e, Components::PhysicsComponent {.physicsID = body, .isStatic = params.isStaticPhysics});
    } else if (part.isSkinned && params.isAnimated) {
        reg.Add(e, Components::TransformComponent {});
        reg.Add(e, Components::WorldTransformComponent {.world = baseTransform, .previous = baseTransform});
        reg.Add(e, Components::HierarchyComponent {.parent = rootEntity});
    } else {
        const auto nodeLocal = GetNodeLogicalTransform(prefab, part.nodeIndex) * NativeModelMatrix(part.localTransform);
        const auto localTRS  = DecomposeTransform(nodeLocal);
        reg.Add(e, Components::TransformComponent {.position = localTRS.translation, .rotation = localTRS.rotation, .scale = localTRS.scale});
        reg.Add(e, Components::WorldTransformComponent {.world = worldMat, .previous = worldMat});
        reg.Add(e, Components::HierarchyComponent {.parent = rootEntity});
    }

    reg.Add(e, Components::NameComponent {.name = part.name});

    reg.Add(
        e, Components::MeshComponent {
               .meshAsset     = meshAsset,
               .materialAsset = matAsset,
               .cullRadius    = part.boundingRadius,
               .localCenter =
                   {(part.localMax[0] + part.localMin[0]) * 0.5f, (part.localMax[1] + part.localMin[1]) * 0.5f, (part.localMax[2] + part.localMin[2]) * 0.5f},
               .flags     = flags,
               .nodeIndex = part.nodeIndex
           }
    );

    if (part.isSkinned && params.isAnimated) {
        const auto jointCount = static_cast<uint32_t>(prefab.skeletons[part.skeletonIndex].joints.size());
        reg.Add(e, Components::SkeletalMeshComponent {
            .jointOffset = assignedJointOffset,
            .jointCount  = jointCount,
            .skeletonIndex = part.skeletonIndex
        });
    }

    if (part.activeMorphCount > 0) {
        const auto morphDeltaCount = part.activeMorphCount * part.mesh.vertexCount * 4u;
        reg.Add(
            e, Components::MorphTargetComponent {
                   .offset          = part.morphOffset,
                   .morphDeltaCount = morphDeltaCount,
                   .activeCount     = part.activeMorphCount,
                   .weights         = {part.defaultMorphWeights[0], part.defaultMorphWeights[1], part.defaultMorphWeights[2], part.defaultMorphWeights[3]}
               }
        );
    }

    return e;
}

// Conditional policy lives in the call site (whether to spawn at all); the
// optional is whether a light was created -- no Entity::Null() sentinel.
auto SpawnEmissiveVPL(ECS::Registry& reg, const ModelPart& part, Entity parentEntity, float scaleMult, float emissiveFactorScale) -> std::optional<Entity> {
    if (part.defaultMaterial.unlit)
        return std::nullopt; // KHR_materials_unlit ignores emissive factors.
    // Virtual lights approximate authored emission, not the presentation
    // boost chosen by the importer. Fidelity captures never opt into them.
    const float invImportScale = 1.0f / emissiveFactorScale;
    const auto& raw            = part.defaultMaterial.emissiveFactor;
    const float ef[3] {raw.x * invImportScale, raw.y * invImportScale, raw.z * invImportScale};

    float lum = ef[0] * 0.2126f + ef[1] * 0.7152f + ef[2] * 0.0722f;
    if (lum <= 0.01f) {
        return std::nullopt;
    }

    const auto localCenter = (part.localMax + part.localMin) * 0.5f;
    float      partExtent  = (part.localMax[0] - part.localMin[0]) + (part.localMax[1] - part.localMin[1]) + (part.localMax[2] - part.localMin[2]);

    Entity glowEnt = reg.Create();
    reg.Add(glowEnt, Components::TransformComponent {.position = localCenter});
    reg.Add(glowEnt, Components::HierarchyComponent {.parent = parentEntity});
    reg.Add(glowEnt, Components::NameComponent {.name = String64("Glow_" + std::string(part.name.c_str()))});

    reg.Add(
        glowEnt, Components::LightComponent {
                     .type      = LightType::Point,
                     .color     = {ef[0], ef[1], ef[2]},
                     .intensity = lum * 35.0f,
                     .radius    = std::max(partExtent * scaleMult * 0.15f, 0.05f),
                     .direction = {0.0f, -1.0f, 0.0f},
                     .range     = std::max(partExtent * scaleMult * 2.5f, 3.0f),
                     .points    = {},
                     .twoSided  = 0
                 }
    );
    return glowEnt;
}

} // namespace

auto CreateBoxInternal(
    RenderContext&                  gfx,
    ECS::Registry&                  reg,
    ZHLN::Optional<PhysicsContext&> pc,
    ZHLN::FVec3                     halfExtents,
    const SpawnParams&              params,
    bool                            buildBLAS
) -> std::expected<Entity, ErrorCode> {
    auto boxColor = params.materialOverride.baseColorFactor;
    if (!(boxColor.w >= 0.0f)) {
        boxColor = {0.8f, 0.4f, 0.2f, 1.0f};
    }

    auto built = BuildBoxMesh(gfx, halfExtents, boxColor, buildBLAS);
    if (!built) [[unlikely]] {
        return std::unexpected(built.error());
    }
    Mesh mesh = std::move(*built);

    Material mat;
    if (params.materialOverride.pipeline != PipelineID::Invalid) {
        mat = params.materialOverride;
    } else {
        auto mat_res        = gfx.CreateMaterial(MaterialDesc::Basic());
        mat                 = mat_res.value_or(Material {});
        mat.baseColorFactor = {boxColor.x, boxColor.y, boxColor.z, boxColor.w};
        mat.roughnessFactor = 0.3f;
        mat.metallicFactor  = 0.1f;
    }

    Entity     e         = reg.Create();
    AssetID    meshAsset = HashAssetID("prefab_box_mesh_" + std::to_string(e.index));
    MaterialID matAsset  = HashAssetID("prefab_box_mat_" + std::to_string(e.index));

    gfx.RegisterGPUMesh(meshAsset, mesh);
    gfx.RegisterGPUMaterial(matAsset, mat);

    const auto worldMat = SpawnMatrix(params);

    reg.Add(e, Components::NameComponent {.name = String64("Box_" + std::to_string(e.index))});
    reg.Add(e, SpawnLocalTransform(params));
    reg.Add(e, Components::WorldTransformComponent {.world = worldMat, .previous = worldMat});

    float maxExtent = std::max({halfExtents.x, halfExtents.y, halfExtents.z});
    reg.Add(e, Components::MeshComponent {.meshAsset = meshAsset, .materialAsset = matAsset, .cullRadius = maxExtent * 2.0f});
    reg.Add(
        e, Components::OwnedMeshComponent {
               .meshAsset  = meshAsset,
               .mesh       = mesh,
               .shape      = Components::OwnedMeshComponent::Shape::Box,
               .dimensions = halfExtents,
               .color      = boxColor,
               .buildBLAS  = buildBLAS
           }
    );
    reg.Add(e, Components::PBRComponent {.roughness = mat.roughnessFactor, .metallic = mat.metallicFactor});

    if (params.createPhysics && pc) {
        const Physics::Box          shape {FVec3 {halfExtents.x * params.scale.x, halfExtents.y * params.scale.y, halfExtents.z * params.scale.z}};
        const Physics::CollisionFilter filter {.category = params.physicsCategory, .mask = params.physicsMask};
        Physics::BodyID             body = Physics::BodyID::Null();
        if (params.isStaticPhysics && !params.isKinematicPhysics) {
            body = Physics::BodyID(pc->CreateBody(shape, Physics::StaticBody {
                                                              .position = params.position, .rotation = params.rotation},
                                                  filter));
        } else if (params.isStaticPhysics) {
            body = Physics::BodyID(pc->CreateBody(shape, Physics::KinematicBody {
                                                                .position = params.position, .rotation = params.rotation},
                                                  filter));
        } else {
            body = Physics::BodyID(pc->CreateBody(shape, Physics::DynamicBody {
                                                              .position    = params.position,
                                                              .rotation    = params.rotation,
                                                              .startAsleep = params.startAsleep},
                                                  filter));
        }
        reg.Add(e, Components::PhysicsComponent {.physicsID = body, .isStatic = params.isStaticPhysics});
    }

    return e;
}

namespace {

template <typename ShapeT>
auto SpawnPrimitive(
    RenderContext&                        gfx,
    ECS::Registry&                        reg,
    ZHLN::Optional<PhysicsContext&>       pc,
    std::string_view                      shapeName,
    Mesh                                  mesh,
    Components::OwnedMeshComponent::Shape shape,
    FVec3                                 dimensions,
    float                                 cullRadius,
    const ShapeT&                         physicsShape,
    const SpawnParams&                    params,
    bool                                  buildBLAS
) -> Entity {
    auto shapeColor = params.color;
    if (!(shapeColor.w >= 0.0f)) {
        shapeColor = {0.8f, 0.4f, 0.2f, 1.0f};
    }

    Material mat;
    if (params.materialOverride.pipeline != PipelineID::Invalid) {
        mat = params.materialOverride;
    } else {
        auto mat_res        = gfx.CreateMaterial(MaterialDesc::Basic());
        mat                 = mat_res.value_or(Material {});
        mat.baseColorFactor = {shapeColor.x, shapeColor.y, shapeColor.z, shapeColor.w};
        mat.roughnessFactor = params.roughness;
        mat.metallicFactor  = params.metallic;
    }

    Entity     e         = reg.Create();
    AssetID    meshAsset = HashAssetID("prefab_" + std::string(shapeName) + "_mesh_" + std::to_string(e.index));
    MaterialID matAsset  = HashAssetID("prefab_" + std::string(shapeName) + "_mat_" + std::to_string(e.index));

    gfx.RegisterGPUMesh(meshAsset, mesh);
    gfx.RegisterGPUMaterial(matAsset, mat);

    const auto worldMat = SpawnMatrix(params);

    reg.Add(e, Components::NameComponent {.name = String64(std::string(shapeName) + "_" + std::to_string(e.index))});
    reg.Add(e, SpawnLocalTransform(params));
    reg.Add(e, Components::WorldTransformComponent {.world = worldMat, .previous = worldMat});
    reg.Add(e, Components::MeshComponent {.meshAsset = meshAsset, .materialAsset = matAsset, .cullRadius = cullRadius});
    reg.Add(
        e, Components::OwnedMeshComponent {
               .meshAsset = meshAsset, .mesh = mesh, .shape = shape, .dimensions = dimensions, .color = shapeColor, .buildBLAS = buildBLAS
           }
    );
    reg.Add(e, Components::PBRComponent {.roughness = mat.roughnessFactor, .metallic = mat.metallicFactor});

    if (params.createPhysics && pc) {
        const Physics::CollisionFilter filter {.category = params.physicsCategory, .mask = params.physicsMask};
        Physics::BodyID               body = Physics::BodyID::Null();
        if (params.isStaticPhysics && !params.isKinematicPhysics) {
            body = Physics::BodyID(pc->CreateBody(physicsShape, Physics::StaticBody {
                                                                     .position = params.position, .rotation = params.rotation},
                                                  filter));
        } else if (params.isStaticPhysics) {
            body = Physics::BodyID(pc->CreateBody(physicsShape, Physics::KinematicBody {
                                                                       .position = params.position, .rotation = params.rotation},
                                                  filter));
        } else {
            body = Physics::BodyID(pc->CreateBody(physicsShape, Physics::DynamicBody {
                                                                     .position    = params.position,
                                                                     .rotation    = params.rotation,
                                                                     .startAsleep = params.startAsleep},
                                                  filter));
        }
        reg.Add(e, Components::PhysicsComponent {.physicsID = body, .isStatic = params.isStaticPhysics});
    }
    return e;
}

} // namespace

auto CreateSphereInternal(RenderContext& gfx, ECS::Registry& reg, ZHLN::Optional<PhysicsContext&> pc, float radius, const SpawnParams& params, bool buildBLAS)
    -> std::expected<Entity, ErrorCode> {
    SpawnParams resolved = params;
    if (resolved.color.w < 0.0f) {
        resolved.color = {0.8f, 0.4f, 0.2f, 1.0f};
    }
    const float maxScale = std::max({params.scale.x, params.scale.y, params.scale.z});
    auto built = BuildSphereMesh(gfx, radius, resolved.color, buildBLAS);
    if (!built) [[unlikely]] {
        return std::unexpected(built.error());
    }

    return SpawnPrimitive(
        gfx, reg, pc, "Sphere", std::move(*built), Components::OwnedMeshComponent::Shape::Sphere,
        FVec3 {radius, 0.0f, 0.0f}, radius * maxScale * 2.0f, Physics::Sphere {radius * maxScale}, resolved, buildBLAS
    );
}

auto CreateCylinderInternal(
    RenderContext&                  gfx,
    ECS::Registry&                  reg,
    ZHLN::Optional<PhysicsContext&> pc,
    float                           radius,
    float                           height,
    const SpawnParams&              params,
    bool                            buildBLAS
) -> std::expected<Entity, ErrorCode> {
    SpawnParams resolved = params;
    if (resolved.color.w < 0.0f) {
        resolved.color = {0.8f, 0.4f, 0.2f, 1.0f};
    }
    const float maxScale = std::max({params.scale.x, params.scale.y, params.scale.z});
    auto built = BuildCylinderMesh(gfx, radius, height, resolved.color, buildBLAS);
    if (!built) [[unlikely]] {
        return std::unexpected(built.error());
    }

    return SpawnPrimitive(
        gfx, reg, pc, "Cylinder", std::move(*built), Components::OwnedMeshComponent::Shape::Cylinder,
        FVec3 {radius, height, 0.0f}, std::max(radius, height * 0.5f) * maxScale * 2.0f,
        Physics::Cylinder {height * 0.5f * maxScale, radius * maxScale}, resolved, buildBLAS
    );
}

auto CreateConeInternal(
    RenderContext&                  gfx,
    ECS::Registry&                  reg,
    ZHLN::Optional<PhysicsContext&> pc,
    float                           radius,
    float                           height,
    const SpawnParams&              params,
    bool                            buildBLAS
) -> std::expected<Entity, ErrorCode> {
    SpawnParams resolved = params;
    if (resolved.color.w < 0.0f) {
        resolved.color = {0.8f, 0.4f, 0.2f, 1.0f};
    }
    const float maxScale = std::max({params.scale.x, params.scale.y, params.scale.z});
    auto built = BuildConeMesh(gfx, radius, height, resolved.color, buildBLAS);
    if (!built) [[unlikely]] {
        return std::unexpected(built.error());
    }

    return SpawnPrimitive(
        gfx, reg, pc, "Cone", std::move(*built), Components::OwnedMeshComponent::Shape::Cone,
        FVec3 {radius, height, 0.0f}, std::max(radius, height * 0.5f) * maxScale * 2.0f,
        Physics::Cylinder {height * 0.5f * maxScale, radius * 0.5f * maxScale}, resolved, buildBLAS
    );
}

auto CreatePlaneInternal(
    RenderContext&                  gfx,
    ECS::Registry&                  reg,
    ZHLN::Optional<PhysicsContext&> pc,
    float                           extent,
    ZHLN::FVec4                     color,
    const SpawnParams&              params,
    bool                            buildBLAS
) -> std::expected<Entity, ErrorCode> {
    auto built = BuildPlaneMesh(gfx, extent, color, buildBLAS);
    if (!built) [[unlikely]] {
        return std::unexpected(built.error());
    }
    Mesh mesh = std::move(*built);

    Material mat;
    if (params.materialOverride.pipeline != PipelineID::Invalid) {
        mat = params.materialOverride;
    } else {
        auto mat_res        = gfx.CreateMaterial(MaterialDesc::Basic());
        mat                 = mat_res.value_or(Material {});
        mat.baseColorFactor = color;
        mat.roughnessFactor = 0.35f;
        mat.metallicFactor  = 0.15f;
    }

    Entity     e         = reg.Create();
    AssetID    meshAsset = HashAssetID("prefab_plane_mesh_" + std::to_string(e.index));
    MaterialID matAsset  = HashAssetID("prefab_plane_mat_" + std::to_string(e.index));

    gfx.RegisterGPUMesh(meshAsset, mesh);
    gfx.RegisterGPUMaterial(matAsset, mat);

    const auto worldMat = SpawnMatrix(params);

    reg.Add(e, Components::NameComponent {.name = String64("Plane_" + std::to_string(e.index))});
    reg.Add(e, SpawnLocalTransform(params));
    reg.Add(e, Components::WorldTransformComponent {.world = worldMat, .previous = worldMat});

    reg.Add(e, Components::MeshComponent {.meshAsset = meshAsset, .materialAsset = matAsset, .cullRadius = extent * 2.0f});
    reg.Add(
        e, Components::OwnedMeshComponent {
               .meshAsset  = meshAsset,
               .mesh       = mesh,
               .shape      = Components::OwnedMeshComponent::Shape::Plane,
               .dimensions = {extent, 0.0f, 0.0f},
               .color      = color,
               .buildBLAS  = buildBLAS
           }
    );
    reg.Add(e, Components::PBRComponent {.roughness = mat.roughnessFactor, .metallic = mat.metallicFactor});

    if (params.createPhysics && pc) {
        const Physics::Plane            shape {};
        const Physics::CollisionFilter  filter {.category = params.physicsCategory, .mask = params.physicsMask};
        const Physics::BodyID body = Physics::BodyID(
            pc->CreateBody(shape, Physics::StaticBody {.position = params.position, .rotation = params.rotation}, filter)
        );
        reg.Add(e, Components::PhysicsComponent {.physicsID = body, .isStatic = true});
    }

    return e;
}

auto InstantiatePrefabInternal(
    RenderContext&       gfx,
    ECS::Registry&       reg,
    PhysicsContext&      pc,
    const ModelPrefab&   prefab,
    const SpawnParams&   params,
    Entity*              outBuffer,
    uint32_t             maxCount,
    void (*onRoot)(void*, Entity),
    void* rootUserdata
) -> uint32_t {
    uint32_t spawnedCount = 0;
    Entity   rootEntity   = Entity::Null();
    uint32_t startIndex   = 0;

    if (!params.createPhysics) {
        rootEntity = SpawnPrefabRoot(reg, prefab.virtualPath.c_str(), params);
        reg.Add(
            rootEntity, Components::PrefabInstanceComponent {
                            .prefabAsset    = HashAssetPath(std::string_view {prefab.virtualPath.c_str()}),
                            .animationCount = static_cast<uint32_t>(prefab.animations.size()),
                            .skeletonCount  = static_cast<uint32_t>(prefab.skeletons.size()),
                            .animated       = params.isAnimated,
                        }
        );

        if (onRoot)
            onRoot(rootUserdata, rootEntity);

        if (outBuffer != nullptr && maxCount > 0) {
            outBuffer[0] = rootEntity;
            startIndex   = 1;
            spawnedCount = 1;
        }
    }

    const auto baseTransform = SpawnMatrix(params);
    const auto preparedParts = PreparePrefabPhysics(prefab, baseTransform, params.createPhysics, params.useBoxColliders);

    float                                 scaleMult = std::max({params.scale.x, params.scale.y, params.scale.z});
    std::unordered_map<int32_t, uint32_t> allocatedSkeletons;

    std::unordered_map<std::string, Entity> instantiatedParts;

    for (size_t i = 0; i < prefab.parts.size(); ++i) {
        Entity meshEnt = InstantiateMeshPart(gfx, reg, pc, prefab, prefab.parts[i], preparedParts[i], params, rootEntity, allocatedSkeletons);

        if (meshEnt == Entity::Null())
            continue;
        instantiatedParts[prefab.parts[i].name.c_str()] = meshEnt;

        if (outBuffer != nullptr && spawnedCount < maxCount) {
            outBuffer[startIndex + (spawnedCount - startIndex)] = meshEnt;
        }
        spawnedCount++;

        const auto glowEnt = params.emissiveVirtualLights ? SpawnEmissiveVPL(reg, prefab.parts[i], meshEnt, scaleMult, prefab.emissiveFactorScale) :
                                                            std::optional<Entity> {};
        if (glowEnt) {
            if (outBuffer != nullptr && spawnedCount < maxCount) {
                outBuffer[spawnedCount] = *glowEnt;
            }
            spawnedCount++;
        }
    }

    for (const auto& part: prefab.parts) {
        if (!part.csgModifiers.empty()) {
            std::string partName = part.name.c_str();
            auto        it       = instantiatedParts.find(partName);
            if (it != instantiatedParts.end()) {
                Entity targetEntity = it->second;

                Components::CSGComponent csgComp;
                for (const auto& mod: part.csgModifiers) {
                    auto opIt = instantiatedParts.find(mod.operand_name);
                    if (opIt != instantiatedParts.end()) {
                        csgComp.modifiers.push_back({.operation = mod.operation, .operandEntity = opIt->second});

                        reg.Patch<Components::MeshComponent>(opIt->second, [&](auto& cutMesh) -> auto { cutMesh.flags |= DrawFlags::Hidden; });
                    }
                }

                if (!csgComp.modifiers.empty()) {
                    reg.Add(targetEntity, std::move(csgComp));
                }
            }
        }
    }

    // ------------------------------------------------------------------------
    // Instantiate KHR_lights_punctual (zero-allocation transform resolution)
    // ------------------------------------------------------------------------
    if (!prefab.lights.empty()) {
        for (const auto& light: prefab.lights) {
            // Walk up the parent hierarchy to compute the model-space transform
            auto    nodeModelMat = Mat4x4::Identity();
            int32_t curr         = light.nodeIndex;
            int     guard        = 0; // Prevent infinite loops on malformed DAGs

            while (curr >= 0 && curr < static_cast<int32_t>(prefab.nodes.size()) && guard++ < 256) {
                nodeModelMat = NativeModelMatrix(prefab.nodes[curr].localTransform) * nodeModelMat;
                curr         = prefab.nodes[curr].parentIndex;
            }

            const auto worldMat = baseTransform * nodeModelMat;
            const auto worldTRS = DecomposeTransform(worldMat);
            const auto localTRS = DecomposeTransform(nodeModelMat);

            // Per glTF spec, directional and spot lights point along local -Z.
            const auto rotation = ComposeTransform({}, worldTRS.rotation, {1.0f, 1.0f, 1.0f});
            const auto worldDir = TransformVector(rotation, {0.0f, 0.0f, -1.0f}).Normalized();

            Entity lightEnt = reg.Create();
            reg.Add(lightEnt, Components::NameComponent {.name = light.name});

            if (rootEntity != Entity::Null()) {
                reg.Add(lightEnt, Components::TransformComponent {.position = localTRS.translation, .rotation = localTRS.rotation, .scale = localTRS.scale});
                reg.Add(lightEnt, Components::HierarchyComponent {.parent = rootEntity});
            } else {
                reg.Add(lightEnt, Components::TransformComponent {.position = worldTRS.translation, .rotation = worldTRS.rotation, .scale = worldTRS.scale});
            }

            reg.Add(lightEnt, Components::WorldTransformComponent {.world = worldMat, .previous = worldMat});

            reg.Add(
                lightEnt, Components::LightComponent {
                              .type             = light.type,
                              .color            = light.color,
                              .intensity        = light.intensity,
                              .radius           = (light.type == LightType::Spot) ? light.outerConeAngle : 0.0f,
                              .direction        = worldDir,
                              .range            = (light.range > 0.0f) ? light.range * scaleMult : 1000.0f,
                              .innerConeRadians = light.innerConeAngle,
                              .outerConeRadians = light.outerConeAngle
                          }
            );

            reg.Add<Components::SceneLightTagComponent>(lightEnt);

            if (outBuffer != nullptr && spawnedCount < maxCount) {
                outBuffer[spawnedCount] = lightEnt;
            }
            spawnedCount++;
        }
    }

    return spawnedCount;
}

void RebuildVulkanResources(RenderContext& gfx, ECS::Registry& reg) {
    kLog.Warn("Device Lost: Rebuilding procedural resources; other GPU assets re-upload lazily.");

    gfx.ClearGPUCaches();

    // The scene owns its procedural mesh buffers. Its recipes survive device
    // loss, but its old IDs do not; rebuild these with the new context.
    // Terrain and other data-driven meshes are lazily rebuilt by their systems.
    using Shape         = Components::OwnedMeshComponent::Shape;
    const auto entities = reg.GetEntitiesWith<Components::OwnedMeshComponent>();
    auto       owners   = reg.GetRawArray<Components::OwnedMeshComponent>();
    for (size_t i = 0; i < entities.size(); ++i) {
        auto& owned = owners[i];
        switch (owned.shape) {
            case Shape::Box: {
                auto rebuilt = BuildBoxMesh(gfx, owned.dimensions, owned.color, owned.buildBLAS);
                if (!rebuilt) [[unlikely]] {
                    kLog.Error("Procedural mesh rebuild failed (shape=box, error={})", rebuilt.error());
                    // Whatever IDs it still holds died with the old context.
                    owned.mesh = {};
                    break;
                }
                owned.mesh = std::move(*rebuilt);
                break;
            }
            case Shape::Plane: {
                auto rebuilt = BuildPlaneMesh(gfx, owned.dimensions.x, owned.color, owned.buildBLAS);
                if (!rebuilt) [[unlikely]] {
                    kLog.Error("Procedural mesh rebuild failed (shape=plane, error={})", rebuilt.error());
                    // Whatever IDs it still holds died with the old context.
                    owned.mesh = {};
                    break;
                }
                owned.mesh = std::move(*rebuilt);
                break;
            }
            case Shape::Sphere: {
                auto rebuilt = BuildSphereMesh(gfx, owned.dimensions.x, owned.color, owned.buildBLAS);
                if (!rebuilt) [[unlikely]] {
                    kLog.Error("Procedural mesh rebuild failed (shape=sphere, error={})", rebuilt.error());
                    // Whatever IDs it still holds died with the old context.
                    owned.mesh = {};
                    break;
                }
                owned.mesh = std::move(*rebuilt);
                break;
            }
            case Shape::Cylinder: {
                auto rebuilt = BuildCylinderMesh(gfx, owned.dimensions.x, owned.dimensions.y, owned.color, owned.buildBLAS);
                if (!rebuilt) [[unlikely]] {
                    kLog.Error("Procedural mesh rebuild failed (shape=cylinder, error={})", rebuilt.error());
                    // Whatever IDs it still holds died with the old context.
                    owned.mesh = {};
                    break;
                }
                owned.mesh = std::move(*rebuilt);
                break;
            }
            case Shape::Cone: {
                auto rebuilt = BuildConeMesh(gfx, owned.dimensions.x, owned.dimensions.y, owned.color, owned.buildBLAS);
                if (!rebuilt) [[unlikely]] {
                    kLog.Error("Procedural mesh rebuild failed (shape=cone, error={})", rebuilt.error());
                    // Whatever IDs it still holds died with the old context.
                    owned.mesh = {};
                    break;
                }
                owned.mesh = std::move(*rebuilt);
                break;
            }
            case Shape::None:
                continue;
        }
        if (owned.meshAsset != InvalidAssetID && owned.mesh.posBuffer != BufferID::Invalid) {
            gfx.RegisterGPUMesh(owned.meshAsset, owned.mesh);
        }

        // Default procedural materials also lived in the lost renderer.
        // Recreate a drawable baseline from the scene's color/PBR settings;
        // data-driven materials are restored by their own asset systems.
        const auto meshComp = reg.Get<Components::MeshComponent>(entities[i]);
        if (meshComp && !gfx.GetGPUMaterial(meshComp->materialAsset).has_value()) {
            if (auto created = gfx.CreateMaterial(
                    owned.color.w < 1.0f ? MaterialDesc::Transparent({owned.color.x, owned.color.y, owned.color.z, owned.color.w}) :
                                           MaterialDesc::Basic({owned.color.x, owned.color.y, owned.color.z, owned.color.w})
                )) {
                Material mat        = *created;
                mat.baseColorFactor = {owned.color.x, owned.color.y, owned.color.z, owned.color.w};
                if (const auto pbr = reg.Get<Components::PBRComponent>(entities[i])) {
                    mat.roughnessFactor = pbr->roughness;
                    mat.metallicFactor  = pbr->metallic;
                }
                gfx.RegisterGPUMaterial(meshComp->materialAsset, mat);
            }
        }
    }
}

auto InstantiatePrefab(Engine& engine, std::string_view path, const SpawnParams& params, Entity* outBuffer, uint32_t maxCount) -> uint32_t {
    auto prefab = LoadModelPrefab(engine, path);
    if (!prefab) {
        return 0;
    }
    return InstantiatePrefab(engine, *prefab, params, outBuffer, maxCount);
}

} // namespace ZHLN::PrefabFactory
