// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/PrefabFactory.cpp
//
// High-level PrefabFactory / EntitySpawner. Creates Jolt colliders, ECS entities,
// GPU buffers from cached prefabs. This is the high-level spawning layer that
// belongs to src/engine, not to filesystem/VFS.
//
// Renamed from PrefabFactory to PrefabFactory. Old namespace kept as alias.

#include <Zahlen/PrefabFactory.hpp>
#include <Zahlen/AssetManager.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/SkeletalAnimation.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/gui/GUI.hpp>
#include <Zahlen/gui/FontLoader.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <Zahlen/Components.hpp>
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <algorithm>
#include <cstddef>
#include <vector>
#include "ArticulationSystem.hpp"
#include "LightingSystem.hpp"
#include <stb_image.h>

namespace ZHLN::PrefabFactory {

namespace {

auto ResolveFontAsset(AssetManager* mgr, AssetID fontID, GUI::BakedFontAsset& owned) -> const GUI::BakedFontAsset* {
    if (mgr != nullptr && fontID != InvalidAssetID) {
        if (auto* cached = mgr->GetCachedFont(fontID); cached != nullptr) {
            return cached;
        }
        AssetLoadRequest req;
        req.assetID = fontID;
        if (mgr->LoadSync(req)) {
            const auto* bytes = static_cast<const std::byte*>(req.outData);
            auto decoded = GUI::DecodeCookedFont(std::span<const std::byte>(bytes, req.outSize));
            mgr->FreeMemory(req);
            if (decoded) {
                auto ownedPtr = std::make_unique<GUI::BakedFontAsset>(std::move(*decoded));
                auto* raw = ownedPtr.get();
                owned = *raw;
                mgr->CacheFont(fontID, std::move(ownedPtr));
                return raw;
            }
        }
    }
    if (GUI::LoadBakedFont(owned)) {
        return &owned;
    }
    return &GUI::GetDefaultBakedFont();
}

} // namespace

auto CreateFontAtlasTexture(RenderContext& ctx, ECS::Registry& registry) -> TextureHandle {
    return CreateFontAtlasTexture(ctx, registry, nullptr, GUI::kDefaultFontAssetID);
}

auto CreateFontAtlasTexture(RenderContext& ctx, ECS::Registry& registry, AssetManager& assetMgr, AssetID fontID) -> TextureHandle {
    return CreateFontAtlasTexture(ctx, registry, &assetMgr, fontID);
}

auto CreateFontAtlasTexture(RenderContext& ctx, ECS::Registry& registry, AssetManager* assetMgr, AssetID fontID) -> TextureHandle {
    auto* uiSettings = registry.GetSingleton<GUI::UISettingsComponent>();
    if (uiSettings == nullptr) {
        return TextureHandle::Invalid;
    }

    GUI::BakedFontAsset        ownedAsset;
    const GUI::BakedFontAsset* asset = ResolveFontAsset(assetMgr, fontID, ownedAsset);

    if (asset == &GUI::GetDefaultBakedFont() && assetMgr != nullptr && fontID == InvalidAssetID) {
        if (auto* def = assetMgr->GetCachedFont(GUI::kDefaultFontAssetID); def != nullptr) {
            asset = def;
        }
    }

    if ((asset->atlasWidth == 0) || (asset->atlasHeight == 0) || asset->coverage.empty()) {
        Log("WARNING: No baked font available; text cannot be drawn.");
        return TextureHandle::Invalid;
    }

    std::vector<uint32_t> rgbaPixels(asset->coverage.size());
    for (size_t i = 0; i < asset->coverage.size(); ++i) {
        rgbaPixels[i] = (static_cast<uint32_t>(asset->coverage[i]) << 24) | 0x00FFFFFF;
    }

    TextureHandle texHandle = ctx.CreateProceduralTexture("FontAtlas", asset->atlasWidth, asset->atlasHeight, false, rgbaPixels.data());

    FontAtlas& font      = uiSettings->fontAtlas;
    font                 = FontAtlas {};
    font.texture         = texHandle;
    font.atlasWidth      = static_cast<float>(asset->atlasWidth);
    font.atlasHeight     = static_cast<float>(asset->atlasHeight);
    font.fontSize        = asset->fontSize;
    font.baseline        = asset->baseline;
    font.lineHeight      = asset->lineHeight;
    font.isSDF           = asset->isSDF;
    font.firstCodepoint  = asset->firstCodepoint;
    font.glyphCount      = static_cast<uint32_t>(std::min<size_t>(asset->glyphs.size(), FontAtlas::kMaxGlyphs));
    for (uint32_t i = 0; i < font.glyphCount; ++i) {
        font.glyphs[i] = asset->glyphs[i];
    }

    uiSettings->defaultFontAtlas = texHandle;

    return texHandle;
}

auto CreateFontAtlasTexture(RenderContext& ctx, ECS::Registry& registry, AssetManager& assetMgr, std::string_view path) -> TextureHandle {
    const AssetID id = path.empty() ? GUI::kDefaultFontAssetID : HashAssetID(path);
    return CreateFontAtlasTexture(ctx, registry, &assetMgr, id);
}

auto PrimeDefaultBakedFont(AssetManager& assetMgr) -> bool {
    if (auto res = LoadFontAsset(assetMgr, GUI::kDefaultFontAssetPath); res.has_value()) {
        if (auto* cached = assetMgr.GetCachedFont(*res); cached != nullptr) {
            GUI::SetDefaultBakedFont(*cached);
        }
        return true;
    }
    return false;
}

auto LoadFontAsset(AssetManager& assetMgr, std::string_view path) -> std::expected<AssetID, ErrorCode> {
    const AssetID id = HashAssetPath(path);
    if (auto* cached = assetMgr.GetCachedFont(id); cached != nullptr) {
        return id;
    }

    AssetLoadRequest req;
    req.assetID = id;

    if (!assetMgr.LoadSync(req)) {
        return std::unexpected(GUI::FontAssetError::Truncated);
    }

    const auto* bytes = static_cast<const std::byte*>(req.outData);
    auto decoded = GUI::DecodeCookedFont(std::span<const std::byte>(bytes, req.outSize));
    assetMgr.FreeMemory(req);

    if (!decoded) {
        Log("WARNING: Cooked font at {} failed to decode; keeping the embedded default.", path);
        return std::unexpected(decoded.error());
    }

    auto heap = std::make_unique<GUI::BakedFontAsset>(std::move(*decoded));
    auto* raw = heap.get();
    assetMgr.CacheFont(id, std::move(heap));
    GUI::SetDefaultBakedFont(*raw);
    return id;
}

auto GetFontAsset(AssetManager& assetMgr, AssetID id) -> GUI::BakedFontAsset* {
    return assetMgr.GetCachedFont(id);
}

auto GetFontAsset(AssetManager& assetMgr, std::string_view path) -> GUI::BakedFontAsset* {
    return assetMgr.GetCachedFont(HashAssetPath(path));
}

auto LoadTexture(RenderContext& ctx, AssetManager& assetMgr, std::string_view path, bool isSRGB) -> uint32_t {
    uint64_t hash = HashAssetPath(path);

    AssetLoadRequest req;
    req.assetID = hash;

    if (!assetMgr.LoadSync(req)) {
        ZHLN::Log("WARNING: Failed to load texture asset from VFS: {}", path);
        return 1;
    }

    int            width    = 0;
    int            height   = 0;
    int            channels = 0;
    unsigned char* pixels   = stbi_load_from_memory(static_cast<const stbi_uc*>(req.outData), static_cast<int>(req.outSize), &width, &height, &channels, 4);

    assetMgr.FreeMemory(req);

    if (pixels == nullptr) {
        ZHLN::Log("ERROR: stbi_load_from_memory failed for texture: {}", path);
        return 1;
    }

    auto texRes = ctx.CreateTexture(pixels, static_cast<uint32_t>(width), static_cast<uint32_t>(height), isSRGB);
    stbi_image_free(pixels);

    return texRes ? *texRes : 1;
}

auto LoadModelPrefab(RenderContext& /*ctx*/, AssetManager& assetMgr, std::string_view path) -> ModelPrefab* {
    return assetMgr.GetCachedPrefab(HashAssetPath(path));
}

namespace {

auto SpawnPrefabRoot(ECS::Registry& reg, std::string_view vPath, const SpawnParams& p) -> Entity {
    Entity     root     = reg.Create();
    JPH::Mat44 localMat = Math::CreateTransform(JPH::Vec3(p.position), p.rotation, p.scale);
    reg.Add(root, Components::TransformComponent {.position = JPH::Vec3(p.position), .rotation = p.rotation, .scale = p.scale});
    reg.Add(root, Components::WorldTransformComponent {.world = localMat, .previous = localMat});
    reg.Add(root, Components::NameComponent {.name = String64("Root_" + std::string(vPath))});
    return root;
}

auto GetNodeLogicalTransform(const ModelPrefab& prefab, int32_t nodeIndex) -> JPH::Mat44 {
    JPH::Mat44 matrix    = prefab.nodes[nodeIndex].localTransform;
    int32_t    parentIdx = prefab.nodes[nodeIndex].parentIndex;

    while (parentIdx >= 0) {
        matrix    = prefab.nodes[parentIdx].localTransform * matrix;
        parentIdx = prefab.nodes[parentIdx].parentIndex;
    }
    return matrix;
}

struct PreparedPart {
    JPH::Vec3      translation {};
    JPH::Quat      rotation {};
    JPH::Vec3      scale {};
    float          maxScale = 1.0f;
    JPH::ShapeRefC shape    = nullptr;
};

void PreparePrefabPhysics(
    const ModelPrefab&         prefab,
    const JPH::Mat44&          baseTransform,
    bool                       createPhysics,
    bool                       useBoxColliders,
    std::vector<PreparedPart>& outPrepared
) {
    outPrepared.resize(prefab.parts.size());

    TaskSystem::ParallelFor(prefab.parts.size(), 16, [&](uint32_t start, uint32_t end, uint32_t) -> void {
        for (uint32_t i = start; i < end; ++i) {
            const auto& part = prefab.parts[i];
            auto&       prep = outPrepared[i];

            JPH::Mat44 nodeWorld  = GetNodeLogicalTransform(prefab, part.nodeIndex);
            JPH::Mat44 finalLocal = baseTransform * nodeWorld * part.localTransform;

            const Math::TransformTRS trs = Math::Decompose(finalLocal);

            prep.scale       = trs.scale;
            prep.rotation    = trs.rotation;
            prep.translation = trs.translation;
            prep.maxScale    = std::max({std::abs(trs.scale.GetX()), std::abs(trs.scale.GetY()), std::abs(trs.scale.GetZ())});

            if (createPhysics) {
                JPH::ShapeRefC rawShape = useBoxColliders ? part.boxCollider : part.meshCollider;
                if (rawShape != nullptr) {
                    prep.shape = !prep.scale.IsClose(JPH::Vec3::sReplicate(1.0f), 1e-5f) ? new JPH::ScaledShape(rawShape, prep.scale) : rawShape;
                }
            }
        }
    });
}

auto InstantiateMeshPart(
    RenderContext&                         ctx,
    ECS::Registry&                         reg,
    PhysicsContext&                        pc,
    ArticulationSystem&                    art,
    const ModelPrefab&                     prefab,
    const ModelPart&                       part,
    const PreparedPart&                    prep,
    const SpawnParams&                     params,
    Entity                                 rootEntity,
    std::unordered_map<int32_t, uint32_t>& allocatedSkeletons
) -> Entity {
    const JPH::Mat44 baseTransform = Math::CreateTransform(JPH::Vec3(params.position), params.rotation, params.scale);

    AssetID    meshAsset = part.meshAsset;
    MaterialID matAsset  = params.materialOverride.pipeline != PipelineHandle::Invalid ? static_cast<uint64_t>(params.materialOverride.pipeline) :
                                                                                         part.materialAsset;

    Material activeMat = params.materialOverride.pipeline != PipelineHandle::Invalid ? params.materialOverride : part.defaultMaterial;

    ctx.RegisterGPUMesh(meshAsset, part.mesh);
    ctx.RegisterGPUMaterial(matAsset, activeMat);

    uint32_t assignedJointOffset = 0;
    if (part.isSkinned && params.isAnimated && part.skeletonIndex >= 0) {
        auto it = allocatedSkeletons.find(part.skeletonIndex);
        if (it != allocatedSkeletons.end()) {
            assignedJointOffset = it->second;
        } else {
            assignedJointOffset                    = art.AllocateJoints(static_cast<uint32_t>(prefab.skeletons[part.skeletonIndex].joints.size()));
            allocatedSkeletons[part.skeletonIndex] = assignedJointOffset;
        }
    }

    Entity    e     = reg.Create();
    DrawFlags flags = DrawFlags::None;
    if (part.isSkinned && params.isAnimated) {
        flags |= DrawFlags::Skinned;
    }
    if (activeMat.alphaMode == 2) {
        flags |= DrawFlags::ExcludeFromTLAS;
    }

    JPH::Mat44 worldMat = Math::CreateTransform(prep.translation, prep.rotation, prep.scale);

    if (params.createPhysics && prep.shape != nullptr) {
        reg.Add(e, Components::TransformComponent {.position = prep.translation, .rotation = prep.rotation, .scale = prep.scale});
        reg.Add(e, Components::WorldTransformComponent {.world = worldMat, .previous = worldMat});

        reg.Add(
            e, Components::PhysicsComponent {
                   .physicsHandle = pc.CreateRigidBody(
                       prep.shape, JPH::RVec3(prep.translation), prep.rotation, params.isStaticPhysics ? JPH::EMotionType::Static : JPH::EMotionType::Dynamic,
                       params.isStaticPhysics ? Layers::ID::NON_MOVING : Layers::ID::MOVING, 0, params.physicsCategory, params.physicsMask, e
                   ),
                   .isStatic = params.isStaticPhysics
               }
        );
    } else if (part.isSkinned && params.isAnimated) {
        reg.Add(e, Components::TransformComponent {.position = JPH::Vec3::sZero(), .rotation = JPH::Quat::sIdentity(), .scale = JPH::Vec3::sReplicate(1.0f)});
        reg.Add(e, Components::WorldTransformComponent {.world = baseTransform, .previous = baseTransform});
        reg.Add(e, Components::HierarchyComponent {.parent = rootEntity});
    } else {
        const JPH::Mat44         nodeLocal  = GetNodeLogicalTransform(prefab, part.nodeIndex) * part.localTransform;
        const Math::TransformTRS localTRS   = Math::Decompose(nodeLocal);
        const JPH::Vec3&         localPos   = localTRS.translation;
        const JPH::Quat&         localRot   = localTRS.rotation;
        const JPH::Vec3&         localScale = localTRS.scale;

        reg.Add(e, Components::TransformComponent {.position = localPos, .rotation = localRot, .scale = localScale});
        reg.Add(e, Components::WorldTransformComponent {.world = worldMat, .previous = worldMat});
        reg.Add(e, Components::HierarchyComponent {.parent = rootEntity});
    }

    reg.Add(e, Components::NameComponent {.name = part.name});

    reg.Add(
        e, Components::MeshComponent {
               .meshAsset     = meshAsset,
               .materialAsset = matAsset,
               .cullRadius    = part.boundingRadius,
               .localCenter   = JPH::Vec3(
                   (part.localMax[0] + part.localMin[0]) * 0.5f, (part.localMax[1] + part.localMin[1]) * 0.5f, (part.localMax[2] + part.localMin[2]) * 0.5f
               ),
               .flags     = flags,
               .nodeIndex = part.nodeIndex
           }
    );

    if (part.isSkinned && params.isAnimated) {
        reg.Add(e, Components::SkeletalMeshComponent {.jointOffset = assignedJointOffset, .skeletonIndex = part.skeletonIndex});
    }

    if (part.activeMorphCount > 0) {
        reg.Add(
            e, Components::MorphTargetComponent {
                   .offset      = part.morphOffset,
                   .activeCount = part.activeMorphCount,
                   .weights     = {part.defaultMorphWeights[0], part.defaultMorphWeights[1], part.defaultMorphWeights[2], part.defaultMorphWeights[3]}
               }
        );
    }

    return e;
}

auto TrySpawnEmissiveVPL(ECS::Registry& reg, const ModelPart& part, Entity parentEntity, float scaleMult) -> Entity {
    static constexpr float kInvDisplayScale = 1.0f / kGLTFEmissiveDisplayScale;

    const float* raw = part.defaultMaterial.emissiveFactor;
    const float  ef[3] {raw[0] * kInvDisplayScale, raw[1] * kInvDisplayScale, raw[2] * kInvDisplayScale};

    float lum = ef[0] * 0.2126f + ef[1] * 0.7152f + ef[2] * 0.0722f;
    if (lum <= 0.01f) {
        return Entity::Null();
    }

    JPH::Vec3 localCenter(
        (part.localMax[0] + part.localMin[0]) * 0.5f, (part.localMax[1] + part.localMin[1]) * 0.5f, (part.localMax[2] + part.localMin[2]) * 0.5f
    );
    float partExtent = (part.localMax[0] - part.localMin[0]) + (part.localMax[1] - part.localMin[1]) + (part.localMax[2] - part.localMin[2]);

    Entity glowEnt = reg.Create();
    reg.Add(glowEnt, Components::TransformComponent {.position = localCenter, .rotation = JPH::Quat::sIdentity(), .scale = JPH::Vec3::sReplicate(1.0f)});
    reg.Add(glowEnt, Components::HierarchyComponent {.parent = parentEntity});
    reg.Add(glowEnt, Components::NameComponent {.name = String64("Glow_" + std::string(part.name.c_str()))});

    reg.Add(
        glowEnt, Components::LightComponent {
                     .type        = LightType::Point,
                     .color       = JPH::Vec3(ef[0], ef[1], ef[2]),
                     .intensity   = lum * 35.0f,
                     .radius      = std::max(partExtent * scaleMult * 0.15f, 0.05f),
                     .direction   = JPH::Vec3(0, -1, 0),
                     .range       = std::max(partExtent * scaleMult * 2.5f, 3.0f),
                     .points      = {},
                     .twoSided    = 0,
                     .shadowLayer = -1
                 }
    );
    return glowEnt;
}

} // namespace

auto CreateBox(RenderContext& ctx, ECS::Registry& reg, PhysicsContext* pc, JPH::Vec3Arg halfExtents, const SpawnParams& params) -> Entity {
    JPH::Vec4 boxColor = (params.materialOverride.baseColorFactor[3] >= 0.0f) ?
                             JPH::Vec4(
                                 params.materialOverride.baseColorFactor[0], params.materialOverride.baseColorFactor[1],
                                 params.materialOverride.baseColorFactor[2], params.materialOverride.baseColorFactor[3]
                             ) :
                             JPH::Vec4(0.8f, 0.4f, 0.2f, 1.0f);

    Mesh mesh = CreateBoxMesh(ctx, halfExtents, boxColor);

    Material mat;
    if (params.materialOverride.pipeline != PipelineHandle::Invalid) {
        mat = params.materialOverride;
    } else {
        auto mat_res           = ctx.CreateBasicMaterial(false, false, false);
        mat                    = mat_res.value_or(Material {});
        mat.baseColorFactor[0] = boxColor.GetX();
        mat.baseColorFactor[1] = boxColor.GetY();
        mat.baseColorFactor[2] = boxColor.GetZ();
        mat.baseColorFactor[3] = boxColor.GetW();
        mat.roughnessFactor    = 0.3f;
        mat.metallicFactor     = 0.1f;
    }

    Entity     e         = reg.Create();
    AssetID    meshAsset = HashAssetID("prefab_box_mesh_" + std::to_string(e.index));
    MaterialID matAsset  = HashAssetID("prefab_box_mat_" + std::to_string(e.index));

    ctx.RegisterGPUMesh(meshAsset, mesh);
    ctx.RegisterGPUMaterial(matAsset, mat);

    JPH::Mat44 worldMat = Math::CreateTransform(JPH::Vec3(params.position), params.rotation, params.scale);

    reg.Add(e, Components::NameComponent {.name = String64("Box_" + std::to_string(e.index))});
    reg.Add(e, Components::TransformComponent {.position = JPH::Vec3(params.position), .rotation = params.rotation, .scale = params.scale});
    reg.Add(e, Components::WorldTransformComponent {.world = worldMat, .previous = worldMat});

    float maxExtent = std::max({halfExtents.GetX(), halfExtents.GetY(), halfExtents.GetZ()});
    reg.Add(e, Components::MeshComponent {.meshAsset = meshAsset, .materialAsset = matAsset, .cullRadius = maxExtent * 2.0f});
    reg.Add(e, Components::PBRComponent {.roughness = mat.roughnessFactor, .metallic = mat.metallicFactor});

    if (params.createPhysics && pc != nullptr) {
        auto shape = pc->GetOrCreateShape(
            Physics::ShapeType::Box, halfExtents.GetX() * params.scale.GetX(), halfExtents.GetY() * params.scale.GetY(),
            halfExtents.GetZ() * params.scale.GetZ()
        );
        auto body = pc->CreateRigidBody(
            shape, params.position, params.rotation, params.isStaticPhysics ? JPH::EMotionType::Static : JPH::EMotionType::Dynamic,
            params.isStaticPhysics ? Layers::ID::NON_MOVING : Layers::ID::MOVING, 0, params.physicsCategory, params.physicsMask, e
        );
        reg.Add(e, Components::PhysicsComponent {.physicsHandle = body, .isStatic = params.isStaticPhysics});
    }

    return e;
}

auto CreateBox(Engine& engine, JPH::Vec3Arg halfExtents, const SpawnParams& params) -> Entity {
    return CreateBox(engine.GetRenderContext(), engine.GetRegistry(), &engine.GetPhysicsContext(), halfExtents, params);
}

namespace {

auto SpawnPrimitive(
    RenderContext&  ctx,
    ECS::Registry&  reg,
    PhysicsContext* pc,
    std::string_view shapeName,
    Mesh             mesh,
    float            cullRadius,
    Physics::ShapeType physicsShape,
    float            physP1,
    float            physP2,
    const SpawnParams& params
) -> Entity {
    const JPH::Vec4 shapeColor = (params.color.GetW() >= 0.0f) ? params.color : JPH::Vec4(0.8f, 0.4f, 0.2f, 1.0f);

    Material mat;
    if (params.materialOverride.pipeline != PipelineHandle::Invalid) {
        mat = params.materialOverride;
    } else {
        auto mat_res           = ctx.CreateBasicMaterial(false, false, false);
        mat                    = mat_res.value_or(Material {});
        mat.baseColorFactor[0] = shapeColor.GetX();
        mat.baseColorFactor[1] = shapeColor.GetY();
        mat.baseColorFactor[2] = shapeColor.GetZ();
        mat.baseColorFactor[3] = shapeColor.GetW();
        mat.roughnessFactor    = params.roughness;
        mat.metallicFactor     = params.metallic;
    }

    Entity     e         = reg.Create();
    AssetID    meshAsset = HashAssetID("prefab_" + std::string(shapeName) + "_mesh_" + std::to_string(e.index));
    MaterialID matAsset  = HashAssetID("prefab_" + std::string(shapeName) + "_mat_" + std::to_string(e.index));

    ctx.RegisterGPUMesh(meshAsset, mesh);
    ctx.RegisterGPUMaterial(matAsset, mat);

    JPH::Mat44 worldMat = Math::CreateTransform(JPH::Vec3(params.position), params.rotation, params.scale);

    reg.Add(e, Components::NameComponent {.name = String64(std::string(shapeName) + "_" + std::to_string(e.index))});
    reg.Add(e, Components::TransformComponent {.position = JPH::Vec3(params.position), .rotation = params.rotation, .scale = params.scale});
    reg.Add(e, Components::WorldTransformComponent {.world = worldMat, .previous = worldMat});
    reg.Add(e, Components::MeshComponent {.meshAsset = meshAsset, .materialAsset = matAsset, .cullRadius = cullRadius});
    reg.Add(e, Components::PBRComponent {.roughness = mat.roughnessFactor, .metallic = mat.metallicFactor});

    if (params.createPhysics && pc != nullptr) {
        auto shape = pc->GetOrCreateShape(physicsShape, physP1, physP2);
        auto body  = pc->CreateRigidBody(
            shape, params.position, params.rotation, params.isStaticPhysics ? JPH::EMotionType::Static : JPH::EMotionType::Dynamic,
            params.isStaticPhysics ? Layers::ID::NON_MOVING : Layers::ID::MOVING, 0, params.physicsCategory, params.physicsMask, e
        );
        reg.Add(e, Components::PhysicsComponent {.physicsHandle = body, .isStatic = params.isStaticPhysics});
    }
    return e;
}

} // namespace

auto CreateSphere(RenderContext& ctx, ECS::Registry& reg, PhysicsContext* pc, float radius, const SpawnParams& params) -> Entity {
    SpawnParams resolved = params;
    if (resolved.color.GetW() < 0.0f) {
        resolved.color = JPH::Vec4(0.8f, 0.4f, 0.2f, 1.0f);
    }
    const float maxScale = std::max({params.scale.GetX(), params.scale.GetY(), params.scale.GetZ()});
    return SpawnPrimitive(
        ctx, reg, pc, "Sphere", CreateSphereMesh(ctx, radius, resolved.color), radius * maxScale * 2.0f, Physics::ShapeType::Sphere,
        radius * maxScale, 0.0f, resolved
    );
}

auto CreateSphere(Engine& engine, float radius, const SpawnParams& params) -> Entity {
    return CreateSphere(engine.GetRenderContext(), engine.GetRegistry(), &engine.GetPhysicsContext(), radius, params);
}

auto CreateCylinder(RenderContext& ctx, ECS::Registry& reg, PhysicsContext* pc, float radius, float height, const SpawnParams& params) -> Entity {
    SpawnParams resolved = params;
    if (resolved.color.GetW() < 0.0f) {
        resolved.color = JPH::Vec4(0.8f, 0.4f, 0.2f, 1.0f);
    }
    const float maxScale = std::max({params.scale.GetX(), params.scale.GetY(), params.scale.GetZ()});
    return SpawnPrimitive(
        ctx, reg, pc, "Cylinder", CreateCylinderMesh(ctx, radius, height, resolved.color), std::max(radius, height * 0.5f) * maxScale * 2.0f,
        Physics::ShapeType::Cylinder, radius * maxScale, height * 0.5f * maxScale, resolved
    );
}

auto CreateCylinder(Engine& engine, float radius, float height, const SpawnParams& params) -> Entity {
    return CreateCylinder(engine.GetRenderContext(), engine.GetRegistry(), &engine.GetPhysicsContext(), radius, height, params);
}

auto CreateCone(RenderContext& ctx, ECS::Registry& reg, PhysicsContext* pc, float radius, float height, const SpawnParams& params) -> Entity {
    SpawnParams resolved = params;
    if (resolved.color.GetW() < 0.0f) {
        resolved.color = JPH::Vec4(0.8f, 0.4f, 0.2f, 1.0f);
    }
    const float maxScale = std::max({params.scale.GetX(), params.scale.GetY(), params.scale.GetZ()});
    return SpawnPrimitive(
        ctx, reg, pc, "Cone", CreateConeMesh(ctx, radius, height, resolved.color), std::max(radius, height * 0.5f) * maxScale * 2.0f,
        Physics::ShapeType::Cylinder, radius * 0.5f * maxScale, height * 0.5f * maxScale, resolved
    );
}

auto CreateCone(Engine& engine, float radius, float height, const SpawnParams& params) -> Entity {
    return CreateCone(engine.GetRenderContext(), engine.GetRegistry(), &engine.GetPhysicsContext(), radius, height, params);
}

auto CreatePlane(RenderContext& ctx, ECS::Registry& reg, PhysicsContext* pc, float extent, const JPH::Vec4& color, const SpawnParams& params) -> Entity {
    Mesh mesh = CreatePlaneMesh(ctx, extent, color);

    Material mat;
    if (params.materialOverride.pipeline != PipelineHandle::Invalid) {
        mat = params.materialOverride;
    } else {
        auto mat_res           = ctx.CreateBasicMaterial(false, false, false);
        mat                    = mat_res.value_or(Material {});
        mat.baseColorFactor[0] = color.GetX();
        mat.baseColorFactor[1] = color.GetY();
        mat.baseColorFactor[2] = color.GetZ();
        mat.baseColorFactor[3] = color.GetW();
        mat.roughnessFactor    = 0.35f;
        mat.metallicFactor     = 0.15f;
    }

    Entity     e         = reg.Create();
    AssetID    meshAsset = HashAssetID("prefab_plane_mesh_" + std::to_string(e.index));
    MaterialID matAsset  = HashAssetID("prefab_plane_mat_" + std::to_string(e.index));

    ctx.RegisterGPUMesh(meshAsset, mesh);
    ctx.RegisterGPUMaterial(matAsset, mat);

    JPH::Mat44 worldMat = Math::CreateTransform(JPH::Vec3(params.position), params.rotation, params.scale);

    reg.Add(e, Components::NameComponent {.name = String64("Plane_" + std::to_string(e.index))});
    reg.Add(e, Components::TransformComponent {.position = JPH::Vec3(params.position), .rotation = params.rotation, .scale = params.scale});
    reg.Add(e, Components::WorldTransformComponent {.world = worldMat, .previous = worldMat});

    reg.Add(e, Components::MeshComponent {.meshAsset = meshAsset, .materialAsset = matAsset, .cullRadius = extent * 2.0f});
    reg.Add(e, Components::PBRComponent {.roughness = mat.roughnessFactor, .metallic = mat.metallicFactor});

    if (params.createPhysics && pc != nullptr) {
        auto shape = pc->GetOrCreateShape(Physics::ShapeType::Plane, 0.0f, 1.0f, 0.0f, 0.0f);
        auto body  = pc->CreateRigidBody(shape, params.position, params.rotation, JPH::EMotionType::Static, Layers::ID::NON_MOVING, 0, params.physicsCategory, params.physicsMask, e);
        reg.Add(e, Components::PhysicsComponent {.physicsHandle = body, .isStatic = true});
    }

    return e;
}

auto CreatePlane(Engine& engine, float extent, const JPH::Vec4& color, const SpawnParams& params) -> Entity {
    return CreatePlane(engine.GetRenderContext(), engine.GetRegistry(), &engine.GetPhysicsContext(), extent, color, params);
}

auto InstantiatePrefab(
    RenderContext&      ctx,
    ECS::Registry&      reg,
    PhysicsContext&     pc,
    ArticulationSystem& art,
    const ModelPrefab&  prefab,
    const SpawnParams&  params,
    Entity*             outBuffer,
    uint32_t            maxCount
) -> uint32_t {
    uint32_t spawnedCount = 0;
    Entity   rootEntity   = Entity::Null();
    uint32_t startIndex   = 0;

    if (!params.createPhysics) {
        rootEntity = SpawnPrefabRoot(reg, prefab.virtualPath.c_str(), params);

        if (params.isAnimated && (!prefab.animations.empty() || !prefab.skeletons.empty())) {
            reg.Add(
                rootEntity, Components::AnimatorComponent {
                                .currentTrackIdx  = prefab.animations.empty() ? -1 : 0,
                                .currentTrackTime = 0.0f,
                                .currentLoop      = true,
                                .prefab           = &prefab,
                            }
            );
        }

        if (outBuffer != nullptr && maxCount > 0) {
            outBuffer[0] = rootEntity;
            startIndex   = 1;
            spawnedCount = 1;
        }
    }

    JPH::Mat44                baseTransform = Math::CreateTransform(JPH::Vec3(params.position), params.rotation, params.scale);
    std::vector<PreparedPart> preparedParts;
    PreparePrefabPhysics(prefab, baseTransform, params.createPhysics, params.useBoxColliders, preparedParts);

    float                                 scaleMult = std::max({params.scale.GetX(), params.scale.GetY(), params.scale.GetZ()});
    std::unordered_map<int32_t, uint32_t> allocatedSkeletons;

    std::unordered_map<std::string, Entity> instantiatedParts;

    for (size_t i = 0; i < prefab.parts.size(); ++i) {
        Entity meshEnt = InstantiateMeshPart(ctx, reg, pc, art, prefab, prefab.parts[i], preparedParts[i], params, rootEntity, allocatedSkeletons);

        instantiatedParts[prefab.parts[i].name.c_str()] = meshEnt;

        if (outBuffer != nullptr && spawnedCount < maxCount) {
            outBuffer[startIndex + (spawnedCount - startIndex)] = meshEnt;
        }
        spawnedCount++;

        Entity glowEnt = params.emissiveVirtualLights ? TrySpawnEmissiveVPL(reg, prefab.parts[i], meshEnt, scaleMult) : Entity::Null();
        if (glowEnt != Entity::Null()) {
            if (outBuffer != nullptr && spawnedCount < maxCount) {
                outBuffer[spawnedCount] = glowEnt;
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

    return spawnedCount;
}

void RebuildVulkanResources(RenderContext& ctx, ECS::Registry& reg) {
    ZHLN::Log("[Engine] Device Lost: Clearing GPU asset cache. Next frame will re-upload assets lazily.");

    ctx.ClearGPUCaches();
    CreateFontAtlasTexture(ctx, reg);
}

auto LoadModelPrefab(Engine& engine, std::string_view path) -> ModelPrefab* {
    return LoadModelPrefab(engine.GetRenderContext(), engine.GetAssetManager(), path);
}

auto InstantiatePrefab(Engine& engine, const ModelPrefab& prefab, const SpawnParams& params, Entity* outBuffer, uint32_t maxCount) -> uint32_t {
    return InstantiatePrefab(engine.GetRenderContext(), engine.GetRegistry(), engine.GetPhysicsContext(), engine.GetArticulationSystem(), prefab, params, outBuffer, maxCount);
}

auto InstantiatePrefab(Engine& engine, std::string_view path, const SpawnParams& params, Entity* outBuffer, uint32_t maxCount) -> uint32_t {
    ModelPrefab* prefab = LoadModelPrefab(engine, path);
    if (prefab == nullptr) {
        return 0;
    }
    return InstantiatePrefab(engine, *prefab, params, outBuffer, maxCount);
}

} // namespace ZHLN::PrefabFactory
