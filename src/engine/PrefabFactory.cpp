// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/Engine/AssetManager.hpp>
#include <Zahlen/Engine/Capabilities/AssetLibrary.hpp>
#include <Zahlen/Engine/Capabilities/GraphicsDevice.hpp>
#include <Zahlen/Engine/Capabilities/Simulation.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include "EngineAccess.hpp"
#include <Zahlen/Engine/ModelPrefab.hpp>
#include <Zahlen/Engine/PrefabFactory.hpp>
#include <Zahlen/Engine/SceneResources.hpp>
#include <Zahlen/GUI/FontLoader.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math/Transform.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Physics/Physics.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Render/SkeletalAnimation.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <optional>
// TODO(Evilpasture): REMOVE SYSTEM LEAK
#include "ArticulationSystem.hpp"
#include <algorithm>
#include <cstddef>
#include <span>
#include <stb_image.h>
#include <utility>
#include <vector>

namespace ZHLN::PrefabFactory {

namespace {

auto ResolveFontAsset(ZHLN::Optional<AssetManager&> mgr, AssetID fontID, GUI::BakedFontAsset& owned) -> const GUI::BakedFontAsset& {
    if (mgr && fontID != InvalidAssetID) {
        if (auto cached = mgr->GetCachedFont(fontID)) {
            return *cached;
        }
        AssetLoadRequest req;
        req.assetID = fontID;
        if (mgr->LoadSync(req)) {
            const auto* bytes   = static_cast<const std::byte*>(req.outData);
            auto        decoded = GUI::DecodeCookedFont(std::span<const std::byte>(bytes, req.outSize));
            mgr->FreeMemory(req);
            if (decoded) {
                auto  ownedPtr = std::make_unique<GUI::BakedFontAsset>(std::move(*decoded));
                auto* raw      = ownedPtr.get();
                owned          = *raw;
                mgr->CacheFont(fontID, std::move(ownedPtr));
                return *raw;
            }
        }
    }
    if (GUI::LoadBakedFont(owned)) {
        return owned;
    }
    return GUI::GetDefaultBakedFont();
}

} // namespace

auto CreateFontAtlas(RenderContext& render) -> FontAtlas {
    return CreateFontAtlas(render, std::nullopt, GUI::kDefaultFontAssetID);
}

auto CreateFontAtlas(RenderContext& render, ZHLN::Optional<AssetManager&> assetMgr, AssetID fontID) -> FontAtlas {
    GUI::BakedFontAsset        ownedAsset;
    const GUI::BakedFontAsset* asset = &ResolveFontAsset(assetMgr, fontID, ownedAsset);

    if (asset == &GUI::GetDefaultBakedFont() && assetMgr && fontID == InvalidAssetID) {
        if (auto def = assetMgr->GetCachedFont(GUI::kDefaultFontAssetID)) {
            asset = &*def;
        }
    }

    if ((asset->atlasWidth == 0) || (asset->atlasHeight == 0) || asset->coverage.empty()) {
        LogWarning("No baked font available; text cannot be drawn.");
        return GUI::BuildFontAtlas(*asset, TextureID::Invalid);
    }

    std::vector<uint32_t> rgbaPixels(asset->coverage.size());
    for (size_t i = 0; i < asset->coverage.size(); ++i) {
        rgbaPixels[i] = (static_cast<uint32_t>(asset->coverage[i]) << 24) | 0x00FFFFFF;
    }

    // No mip chain: the atlas packs glyphs side by side, so a generated mip
    // averages neighbours into each other and any sampled LOD above zero
    // smears text together. Single-level also pins the sampler at mip 0 no
    // matter the LOD range the scene sampler uses.
    const TextureID texID = render.CreateProceduralTexture("FontAtlas", {asset->atlasWidth, asset->atlasHeight}, rgbaPixels, false, false);
    return GUI::BuildFontAtlas(*asset, texID);
}

auto CreateFontAtlas(RenderContext& render, AssetManager& assetMgr, std::string_view path) -> FontAtlas {
    const AssetID id = path.empty() ? GUI::kDefaultFontAssetID : HashAssetID(path);
    return CreateFontAtlas(render, assetMgr, id);
}

auto PrimeDefaultBakedFont(AssetManager& assetMgr) -> bool {
    if (auto res = LoadFontAsset(assetMgr, GUI::kDefaultFontAssetPath); res.has_value()) {
        if (auto cached = assetMgr.GetCachedFont(*res)) {
            GUI::SetDefaultBakedFont(*cached);
        }
        return true;
    }
    return false;
}

auto LoadFontAsset(AssetManager& assetMgr, std::string_view path) -> std::expected<AssetID, ErrorCode> {
    const AssetID id = HashAssetPath(path);
    if (auto cached = assetMgr.GetCachedFont(id)) {
        return id;
    }

    AssetLoadRequest req;
    req.assetID = id;

    if (!assetMgr.LoadSync(req)) {
        return std::unexpected(GUI::FontAssetError::Truncated);
    }

    const auto* bytes   = static_cast<const std::byte*>(req.outData);
    auto        decoded = GUI::DecodeCookedFont(std::span<const std::byte>(bytes, req.outSize));
    assetMgr.FreeMemory(req);

    if (!decoded) {
        LogWarning("Cooked font at {} failed to decode ({}); keeping the embedded default.", path, decoded.error());
        return std::unexpected(decoded.error());
    }

    auto  heap = std::make_unique<GUI::BakedFontAsset>(std::move(*decoded));
    auto* raw  = heap.get();
    assetMgr.CacheFont(id, std::move(heap));
    GUI::SetDefaultBakedFont(*raw);
    return id;
}

auto GetFontAsset(AssetManager& assetMgr, AssetID id) -> ZHLN::Optional<GUI::BakedFontAsset&> {
    return assetMgr.GetCachedFont(id);
}

auto GetFontAsset(AssetManager& assetMgr, std::string_view path) -> ZHLN::Optional<GUI::BakedFontAsset&> {
    return assetMgr.GetCachedFont(HashAssetPath(path));
}

auto LoadTexture(RenderContext& render, AssetManager& assetMgr, std::string_view path, bool isSRGB) -> TextureID {
    uint64_t hash = HashAssetPath(path);

    AssetLoadRequest req;
    req.assetID = hash;

    if (!assetMgr.LoadSync(req)) {
        ZHLN::LogWarning("Failed to load texture asset from VFS: {}", path);
        return TextureID::Invalid;
    }

    int            width    = 0;
    int            height   = 0;
    int            channels = 0;
    unsigned char* pixels   = stbi_load_from_memory(static_cast<const stbi_uc*>(req.outData), static_cast<int>(req.outSize), &width, &height, &channels, 4);

    assetMgr.FreeMemory(req);

    if (pixels == nullptr) {
        ZHLN::LogError("stbi_load_from_memory failed for texture: {}", path);
        return TextureID::Invalid;
    }

    auto texRes = render.CreateTexture(
        path, std::span {pixels, static_cast<size_t>(width) * height * 4}, {static_cast<uint32_t>(width), static_cast<uint32_t>(height)}, isSRGB
    );
    stbi_image_free(pixels);

    if (!texRes) {
        ZHLN::LogError("Failed to upload texture asset {}: {}", path, texRes.error());
    }
    return texRes.value_or(TextureID::Invalid);
}

auto LoadModelPrefab(GraphicsDevice, AssetManager& assetMgr, std::string_view path) -> ZHLN::Optional<ModelPrefab&> {
    return assetMgr.GetCachedPrefab(HashAssetPath(path));
}

namespace {

[[nodiscard]] auto NativeModelMatrix(const Mat4x4& source) -> Mat4x4 { return source; }

[[nodiscard]] auto SpawnLocalTransform(const SpawnParams& params) -> Components::TransformComponent {
    return {
        .position = {static_cast<float>(params.position.x), static_cast<float>(params.position.y), static_cast<float>(params.position.z)},
        .rotation = params.rotation,
        .scale = params.scale
    };
}

[[nodiscard]] auto SpawnMatrix(const SpawnParams& params) -> Mat4x4 {
    const auto transform = SpawnLocalTransform(params);
    return ComposeTransform(transform.position, transform.rotation, transform.scale);
}

auto SpawnPrefabRoot(ECS::Registry& reg, std::string_view vPath, const SpawnParams& p) -> Entity {
    const auto root = reg.Create();
    const auto localMat = SpawnMatrix(p);
    reg.Add(root, SpawnLocalTransform(p));
    reg.Add(root, Components::WorldTransformComponent {.world = localMat, .previous = localMat});
    reg.Add(root, Components::NameComponent {.name = String64("Root_" + std::string(vPath))});
    return root;
}

auto GetNodeLogicalTransform(const ModelPrefab& prefab, int32_t nodeIndex) -> Mat4x4 {
    auto matrix = NativeModelMatrix(prefab.nodes[nodeIndex].localTransform);
    int32_t    parentIdx = prefab.nodes[nodeIndex].parentIndex;

    while (parentIdx >= 0) {
        matrix    = NativeModelMatrix(prefab.nodes[parentIdx].localTransform) * matrix;
        parentIdx = prefab.nodes[parentIdx].parentIndex;
    }
    return matrix;
}

struct PreparedPart {
    FVec3      translation {};
    Quat           rotation = Quat::Identity();
    FVec3      scale {};
    float          maxScale = 1.0f;
    const Physics::AnyShape* shape = nullptr;
};

auto PreparePrefabPhysics(const ModelPrefab& prefab, const Mat4x4& baseTransform, bool createPhysics, bool useBoxColliders) -> std::vector<PreparedPart> {
    std::vector<PreparedPart> outPrepared(prefab.parts.size());

    TaskSystem::ParallelFor(prefab.parts.size(), 16, [&](uint32_t start, uint32_t end, uint32_t) -> void {
        for (uint32_t i = start; i < end; ++i) {
            const auto& part = prefab.parts[i];
            auto&       prep = outPrepared[i];

            const auto nodeWorld = GetNodeLogicalTransform(prefab, part.nodeIndex);
            const auto finalLocal = baseTransform * nodeWorld * NativeModelMatrix(part.localTransform);
            const auto trs = DecomposeTransform(finalLocal);

            prep.scale       = trs.scale;
            prep.rotation    = trs.rotation;
            prep.translation = trs.translation;
            prep.maxScale    = std::max({std::abs(trs.scale.x), std::abs(trs.scale.y), std::abs(trs.scale.z)});

            if (createPhysics) {
                const auto& shape = useBoxColliders ? part.boxCollider : part.meshCollider;
                if (shape) prep.shape = &*shape;
            }
        }
    });
    return outPrepared;
}

auto InstantiateMeshPart(
    GraphicsDevice                         gfx,
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
    const auto baseTransform = SpawnMatrix(params);

    AssetID    meshAsset = part.meshAsset;
    MaterialID matAsset  = params.materialOverride.pipeline != PipelineID::Invalid ? static_cast<uint64_t>(params.materialOverride.pipeline) :
                                                                                     part.materialAsset;

    Material activeMat = params.materialOverride.pipeline != PipelineID::Invalid ? params.materialOverride : part.defaultMaterial;

    if (part.mesh.posBuffer != BufferID::Invalid) {
        gfx.RegisterMesh(meshAsset, part.mesh);
    }
    if (activeMat.pipeline != PipelineID::Invalid) {
        gfx.RegisterMaterial(matAsset, activeMat);
    }

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
        body = pc.CreateRigidBody(
            *prep.shape, prep.translation.Cast<double>(), prep.rotation.AsVec4(),
            params.isStaticPhysics ? (params.isKinematicPhysics ? Physics::MotionType::Kinematic : Physics::MotionType::Static) : Physics::MotionType::Dynamic,
            (params.isStaticPhysics && !params.isKinematicPhysics) ? Physics::ObjectLayer::NonMoving : Physics::ObjectLayer::Moving, 0, params.physicsCategory,
            params.physicsMask, (params.isStaticPhysics || params.startAsleep) ? Physics::Activation::DontActivate : Physics::Activation::Activate,
            prep.scale
        );
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
        const auto localTRS = DecomposeTransform(nodeLocal);
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
               .localCenter   = {
                   (part.localMax[0] + part.localMin[0]) * 0.5f, (part.localMax[1] + part.localMin[1]) * 0.5f, (part.localMax[2] + part.localMin[2]) * 0.5f
               },
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

// Conditional policy lives in the call site (whether to spawn at all); the
// optional is whether a light was created -- no Entity::Null() sentinel.
auto SpawnEmissiveVPL(ECS::Registry& reg, const ModelPart& part, Entity parentEntity, float scaleMult, float emissiveFactorScale) -> std::optional<Entity> {
    if (part.defaultMaterial.unlit)
        return std::nullopt; // KHR_materials_unlit ignores emissive factors.
    // Virtual lights approximate authored emission, not the presentation
    // boost chosen by the importer. Fidelity captures never opt into them.
    const float         invImportScale = 1.0f / emissiveFactorScale;
    const auto& raw = part.defaultMaterial.emissiveFactor;
    const float         ef[3] {raw.x * invImportScale, raw.y * invImportScale, raw.z * invImportScale};

    float lum = ef[0] * 0.2126f + ef[1] * 0.7152f + ef[2] * 0.0722f;
    if (lum <= 0.01f) {
        return std::nullopt;
    }

    const auto localCenter = (part.localMax + part.localMin) * 0.5f;
    float partExtent = (part.localMax[0] - part.localMin[0]) + (part.localMax[1] - part.localMin[1]) + (part.localMax[2] - part.localMin[2]);

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

static auto CreateBox(GraphicsDevice gfx, ECS::Registry& reg, ZHLN::Optional<PhysicsContext&> pc, ZHLN::FVec3 halfExtents, const SpawnParams& params) -> Entity {
    auto boxColor = params.materialOverride.baseColorFactor;
    if (!(boxColor.w >= 0.0f)) {
        boxColor = {0.8f, 0.4f, 0.2f, 1.0f};
    }

    Mesh mesh = CreateBoxMesh(gfx, halfExtents, boxColor);

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

    gfx.RegisterMesh(meshAsset, mesh);
    gfx.RegisterMaterial(matAsset, mat);

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
               .color      = boxColor
           }
    );
    reg.Add(e, Components::PBRComponent {.roughness = mat.roughnessFactor, .metallic = mat.metallicFactor});

    if (params.createPhysics && pc) {
        Physics::AnyShape shape = Physics::Shape<Physics::Box> {
            halfExtents.x * params.scale.x, halfExtents.y * params.scale.y, halfExtents.z * params.scale.z
        };
        auto body = pc->CreateRigidBody(
            shape, params.position, params.rotation.AsVec4(),
            params.isStaticPhysics ? (params.isKinematicPhysics ? Physics::MotionType::Kinematic : Physics::MotionType::Static) : Physics::MotionType::Dynamic,
            (params.isStaticPhysics && !params.isKinematicPhysics) ? Physics::ObjectLayer::NonMoving : Physics::ObjectLayer::Moving, 0, params.physicsCategory, params.physicsMask,
            (params.isStaticPhysics || params.startAsleep) ? Physics::Activation::DontActivate : Physics::Activation::Activate
        );
        reg.Add(e, Components::PhysicsComponent {.physicsID = body, .isStatic = params.isStaticPhysics});
    }

    return e;
}

auto CreateBox(Engine& engine, ZHLN::FVec3 halfExtents, const SpawnParams& params) -> Entity {
    return CreateBox(engine.Graphics(), ZHLN::EngineAccess::RegistryOf(engine), ZHLN::EngineAccess::PhysicsOf(engine), halfExtents, params);
}

namespace {

auto SpawnPrimitive(
    GraphicsDevice                        gfx,
    ECS::Registry&                        reg,
    ZHLN::Optional<PhysicsContext&>       pc,
    std::string_view                      shapeName,
    Mesh                                  mesh,
    Components::OwnedMeshComponent::Shape shape,
    FVec3                             dimensions,
    float                                 cullRadius,
    Physics::ShapeType                    physicsShape,
    float                                 physP1,
    float                                 physP2,
    const SpawnParams&                    params
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

    gfx.RegisterMesh(meshAsset, mesh);
    gfx.RegisterMaterial(matAsset, mat);

    const auto worldMat = SpawnMatrix(params);

    reg.Add(e, Components::NameComponent {.name = String64(std::string(shapeName) + "_" + std::to_string(e.index))});
    reg.Add(e, SpawnLocalTransform(params));
    reg.Add(e, Components::WorldTransformComponent {.world = worldMat, .previous = worldMat});
    reg.Add(e, Components::MeshComponent {.meshAsset = meshAsset, .materialAsset = matAsset, .cullRadius = cullRadius});
    reg.Add(e, Components::OwnedMeshComponent {.meshAsset = meshAsset, .mesh = mesh, .shape = shape, .dimensions = dimensions, .color = shapeColor});
    reg.Add(e, Components::PBRComponent {.roughness = mat.roughnessFactor, .metallic = mat.metallicFactor});

    if (params.createPhysics && pc) {
        Physics::AnyShape bodyShape = physicsShape == Physics::ShapeType::Sphere ?
            Physics::AnyShape(Physics::Shape<Physics::Sphere> {physP1}) :
            Physics::AnyShape(Physics::Shape<Physics::Cylinder> {physP1, physP2});
        auto body      = pc->CreateRigidBody(
            bodyShape, params.position, params.rotation.AsVec4(),
            params.isStaticPhysics ? (params.isKinematicPhysics ? Physics::MotionType::Kinematic : Physics::MotionType::Static) : Physics::MotionType::Dynamic,
            (params.isStaticPhysics && !params.isKinematicPhysics) ? Physics::ObjectLayer::NonMoving : Physics::ObjectLayer::Moving, 0, params.physicsCategory, params.physicsMask,
            (params.isStaticPhysics || params.startAsleep) ? Physics::Activation::DontActivate : Physics::Activation::Activate
        );
        reg.Add(e, Components::PhysicsComponent {.physicsID = body, .isStatic = params.isStaticPhysics});
    }
    return e;
}

} // namespace

static auto CreateSphere(GraphicsDevice gfx, ECS::Registry& reg, ZHLN::Optional<PhysicsContext&> pc, float radius, const SpawnParams& params) -> Entity {
    SpawnParams resolved = params;
    if (resolved.color.w < 0.0f) {
        resolved.color = {0.8f, 0.4f, 0.2f, 1.0f};
    }
    const float maxScale = std::max({params.scale.x, params.scale.y, params.scale.z});
    return SpawnPrimitive(
        gfx, reg, pc, "Sphere", CreateSphereMesh(gfx, radius, resolved.color), Components::OwnedMeshComponent::Shape::Sphere, FVec3{radius, 0.0f, 0.0f},
        radius * maxScale * 2.0f, Physics::ShapeType::Sphere, radius * maxScale, 0.0f, resolved
    );
}

auto CreateSphere(Engine& engine, float radius, const SpawnParams& params) -> Entity {
    return CreateSphere(engine.Graphics(), ZHLN::EngineAccess::RegistryOf(engine), ZHLN::EngineAccess::PhysicsOf(engine), radius, params);
}

static auto CreateCylinder(GraphicsDevice gfx, ECS::Registry& reg, ZHLN::Optional<PhysicsContext&> pc, float radius, float height, const SpawnParams& params)
    -> Entity {
    SpawnParams resolved = params;
    if (resolved.color.w < 0.0f) {
        resolved.color = {0.8f, 0.4f, 0.2f, 1.0f};
    }
    const float maxScale = std::max({params.scale.x, params.scale.y, params.scale.z});
    return SpawnPrimitive(
        gfx, reg, pc, "Cylinder", CreateCylinderMesh(gfx, radius, height, resolved.color), Components::OwnedMeshComponent::Shape::Cylinder,
        FVec3{radius, height, 0.0f}, std::max(radius, height * 0.5f) * maxScale * 2.0f, Physics::ShapeType::Cylinder, radius * maxScale,
        height * 0.5f * maxScale, resolved
    );
}

auto CreateCylinder(Engine& engine, float radius, float height, const SpawnParams& params) -> Entity {
    return CreateCylinder(engine.Graphics(), ZHLN::EngineAccess::RegistryOf(engine), ZHLN::EngineAccess::PhysicsOf(engine), radius, height, params);
}

static auto CreateCone(GraphicsDevice gfx, ECS::Registry& reg, ZHLN::Optional<PhysicsContext&> pc, float radius, float height, const SpawnParams& params) -> Entity {
    SpawnParams resolved = params;
    if (resolved.color.w < 0.0f) {
        resolved.color = {0.8f, 0.4f, 0.2f, 1.0f};
    }
    const float maxScale = std::max({params.scale.x, params.scale.y, params.scale.z});
    return SpawnPrimitive(
        gfx, reg, pc, "Cone", CreateConeMesh(gfx, radius, height, resolved.color), Components::OwnedMeshComponent::Shape::Cone, FVec3{radius, height, 0.0f},
        std::max(radius, height * 0.5f) * maxScale * 2.0f, Physics::ShapeType::Cylinder, radius * 0.5f * maxScale, height * 0.5f * maxScale, resolved
    );
}

auto CreateCone(Engine& engine, float radius, float height, const SpawnParams& params) -> Entity {
    return CreateCone(engine.Graphics(), ZHLN::EngineAccess::RegistryOf(engine), ZHLN::EngineAccess::PhysicsOf(engine), radius, height, params);
}

static auto CreatePlane(GraphicsDevice gfx, ECS::Registry& reg, ZHLN::Optional<PhysicsContext&> pc, float extent, ZHLN::FVec4 color, const SpawnParams& params)
    -> Entity {
    Mesh mesh = CreatePlaneMesh(gfx, extent, color);

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

    gfx.RegisterMesh(meshAsset, mesh);
    gfx.RegisterMaterial(matAsset, mat);

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
               .color      = color
           }
    );
    reg.Add(e, Components::PBRComponent {.roughness = mat.roughnessFactor, .metallic = mat.metallicFactor});

    if (params.createPhysics && pc) {
        Physics::AnyShape shape = Physics::Shape<Physics::Plane> {};
        auto body  = pc->CreateRigidBody(
            shape, params.position, params.rotation.AsVec4(), Physics::MotionType::Static, Physics::ObjectLayer::NonMoving, 0, params.physicsCategory, params.physicsMask
        );
        reg.Add(e, Components::PhysicsComponent {.physicsID = body, .isStatic = true});
    }

    return e;
}

auto CreatePlane(Engine& engine, float extent, ZHLN::FVec4 color, const SpawnParams& params) -> Entity {
    return CreatePlane(engine.Graphics(), ZHLN::EngineAccess::RegistryOf(engine), ZHLN::EngineAccess::PhysicsOf(engine), extent, color, params);
}

static auto InstantiatePrefab(
    GraphicsDevice      gfx,
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
                                .prefabAsset      = HashAssetPath(std::string_view(prefab.virtualPath)),
                                .currentTrackIdx  = prefab.animations.empty() ? -1 : 0,
                                .currentTrackTime = 0.0f,
                                .currentLoop      = true,
                            }
            );
        }

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
        Entity meshEnt = InstantiateMeshPart(gfx, reg, pc, art, prefab, prefab.parts[i], preparedParts[i], params, rootEntity, allocatedSkeletons);

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
            auto nodeModelMat = Mat4x4::Identity();
            int32_t    curr         = light.nodeIndex;
            int        guard        = 0; // Prevent infinite loops on malformed DAGs

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

void RebuildVulkanResources(GraphicsDevice gfx, ECS::Registry& reg) {
    ZHLN::Log("[Engine] Device Lost: Rebuilding procedural resources; other GPU assets re-upload lazily.");

    gfx.ClearCaches();

    // The scene owns its procedural mesh buffers. Its recipes survive device
    // loss, but its old IDs do not; rebuild these with the new context.
    // Terrain and other data-driven meshes are lazily rebuilt by their systems.
    using Shape         = Components::OwnedMeshComponent::Shape;
    const auto entities = reg.GetEntitiesWith<Components::OwnedMeshComponent>();
    auto       owners   = reg.GetRawArray<Components::OwnedMeshComponent>();
    for (size_t i = 0; i < entities.size(); ++i) {
        auto& owned = owners[i];
        switch (owned.shape) {
            case Shape::Box:
                owned.mesh = CreateBoxMesh(gfx, owned.dimensions, owned.color);
                break;
            case Shape::Plane:
                owned.mesh = CreatePlaneMesh(gfx, owned.dimensions.x, owned.color);
                break;
            case Shape::Sphere:
                owned.mesh = CreateSphereMesh(gfx, owned.dimensions.x, owned.color);
                break;
            case Shape::Cylinder:
                owned.mesh = CreateCylinderMesh(gfx, owned.dimensions.x, owned.dimensions.y, owned.color);
                break;
            case Shape::Cone:
                owned.mesh = CreateConeMesh(gfx, owned.dimensions.x, owned.dimensions.y, owned.color);
                break;
            case Shape::None:
                continue;
        }
        if (owned.meshAsset != InvalidAssetID && owned.mesh.posBuffer != BufferID::Invalid) {
            gfx.RegisterMesh(owned.meshAsset, owned.mesh);
        }

        // Default procedural materials also lived in the lost renderer.
        // Recreate a drawable baseline from the scene's color/PBR settings;
        // data-driven materials are restored by their own asset systems.
        const auto meshComp = reg.Get<Components::MeshComponent>(entities[i]);
        if (meshComp && !gfx.Material(meshComp->materialAsset).has_value()) {
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
                gfx.RegisterMaterial(meshComp->materialAsset, mat);
            }
        }
    }
}

auto LoadModelPrefab(Engine& engine, std::string_view path) -> ZHLN::Optional<ModelPrefab&> {
    return LoadModelPrefab(engine.Graphics(), ZHLN::EngineAccess::AssetsOf(engine), path);
}

auto InstantiatePrefab(Engine& engine, const ModelPrefab& prefab, const SpawnParams& params, Entity* outBuffer, uint32_t maxCount) -> uint32_t {
    return InstantiatePrefab(
        engine.Graphics(), ZHLN::EngineAccess::RegistryOf(engine), ZHLN::EngineAccess::PhysicsOf(engine), ZHLN::EngineAccess::ArticulationOf(engine), prefab,
        params, outBuffer, maxCount
    );
}

auto InstantiatePrefab(Engine& engine, std::string_view path, const SpawnParams& params, Entity* outBuffer, uint32_t maxCount) -> uint32_t {
    auto prefab = LoadModelPrefab(engine, path);
    if (!prefab) {
        return 0;
    }
    return InstantiatePrefab(engine, *prefab, params, outBuffer, maxCount);
}

} // namespace ZHLN::PrefabFactory
