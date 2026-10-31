// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Math/Float3.h>
#include <Jolt/Math/Float4.h>
// clang-format on
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Core/Optional.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/Engine/ModelPrefab.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/GUI/FontLoader.hpp>
#include <Zahlen/Math/ValueTypes.hpp>
#include <Zahlen/Physics/Physics.hpp>
#include <Zahlen/Render/Types.hpp>
#include <span>
#include <string_view>

namespace ZHLN {
class Engine;
class RenderContext;
class AssetManager;
class ArticulationSystem;
namespace ECS {
class Registry;
}
}

namespace ZHLN::PrefabFactory {

// Standalone Registry+RenderContext scenes must release generated meshes
// before removing components or clearing the registry. Engine scenes use
// DespawnEntity, Engine::ClearScene and SceneResources::Attach/Detach instead.
void AttachOwnedMesh(RenderContext& ctx, ECS::Registry& reg, Entity entity, Components::OwnedMeshComponent component);
void DetachOwnedMesh(RenderContext& ctx, ECS::Registry& reg, Entity entity);
void ReleaseOwnedMeshes(RenderContext& ctx, ECS::Registry& reg);

// Standalone builders return caller-owned buffers: use DestroyMesh after their
// last draw. Entity spawners below attach scene-owned cleanup instead.
auto CreateTetrahedronMesh(RenderContext& ctx) -> Mesh;
auto CreatePlaneMesh(RenderContext& ctx, float extent = 10.0f, ZHLN::Float4 color = {0.6f, 0.6f, 0.6f, 1.0f}) -> Mesh;
auto CreateBoxMesh(RenderContext& ctx, ZHLN::Float3 halfExtents, ZHLN::Float4 color = {0.8f, 0.4f, 0.2f, 1.0f}) -> Mesh;
auto CreateSphereMesh(RenderContext& ctx, float radius, ZHLN::Float4 color = {0.8f, 0.4f, 0.2f, 1.0f}) -> Mesh;
auto CreateCylinderMesh(RenderContext& ctx, float radius, float height, ZHLN::Float4 color = {0.8f, 0.4f, 0.2f, 1.0f}) -> Mesh;
auto CreateConeMesh(RenderContext& ctx, float radius, float height, ZHLN::Float4 color = {0.8f, 0.4f, 0.2f, 1.0f}) -> Mesh;

auto CreateFontAtlas(RenderContext& ctx) -> FontAtlas;
auto CreateFontAtlas(RenderContext& ctx, ZHLN::Optional<AssetManager&> assetMgr, AssetID fontID) -> FontAtlas;
auto CreateFontAtlas(RenderContext& ctx, AssetManager& assetMgr, std::string_view path) -> FontAtlas;

auto PrimeDefaultBakedFont(AssetManager& assetMgr) -> bool;

auto LoadFontAsset(AssetManager& assetMgr, std::string_view path) -> std::expected<AssetID, ErrorCode>;

auto GetFontAsset(AssetManager& assetMgr, AssetID id) -> ZHLN::Optional<GUI::BakedFontAsset&>;
auto GetFontAsset(AssetManager& assetMgr, std::string_view path) -> ZHLN::Optional<GUI::BakedFontAsset&>;

auto LoadTexture(RenderContext& ctx, AssetManager& assetMgr, std::string_view path, bool isSRGB = true) -> TextureHandle;

struct SpawnParams {
    ZHLN::Double3 position {};
    JPH::Quat     rotation = JPH::Quat::sIdentity();
    ZHLN::Float3  scale {1.0f, 1.0f, 1.0f};

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
    ZHLN::Float4 color {0.8f, 0.4f, 0.2f, -1.0f};

    Material materialOverride = {.pipeline = PipelineHandle::Invalid};
};

auto CreateBox(RenderContext& ctx, ECS::Registry& reg, ZHLN::Optional<PhysicsContext&> pc, ZHLN::Float3 halfExtents, const SpawnParams& params = {}) -> Entity;
auto CreateBox(Engine& engine, ZHLN::Float3 halfExtents, const SpawnParams& params = {}) -> Entity;

auto CreatePlane(
    RenderContext&                  ctx,
    ECS::Registry&                  reg,
    ZHLN::Optional<PhysicsContext&> pc,
    float                           extent = 10.0f,
    ZHLN::Float4                    color  = {0.6f, 0.6f, 0.6f, 1.0f},
    const SpawnParams&              params = {}
) -> Entity;
auto CreatePlane(Engine& engine, float extent = 10.0f, ZHLN::Float4 color = {0.6f, 0.6f, 0.6f, 1.0f}, const SpawnParams& params = {}) -> Entity;

auto CreateSphere(RenderContext& ctx, ECS::Registry& reg, ZHLN::Optional<PhysicsContext&> pc, float radius, const SpawnParams& params = {}) -> Entity;
auto CreateSphere(Engine& engine, float radius, const SpawnParams& params = {}) -> Entity;
auto CreateCylinder(RenderContext& ctx, ECS::Registry& reg, ZHLN::Optional<PhysicsContext&> pc, float radius, float height, const SpawnParams& params = {}) -> Entity;
auto CreateCylinder(Engine& engine, float radius, float height, const SpawnParams& params = {}) -> Entity;
auto CreateCone(RenderContext& ctx, ECS::Registry& reg, ZHLN::Optional<PhysicsContext&> pc, float radius, float height, const SpawnParams& params = {}) -> Entity;
auto CreateCone(Engine& engine, float radius, float height, const SpawnParams& params = {}) -> Entity;

auto LoadModelPrefab(RenderContext& ctx, AssetManager& assetMgr, std::string_view path) -> ZHLN::Optional<ModelPrefab&>;
auto LoadModelPrefab(Engine& engine, std::string_view path) -> ZHLN::Optional<ModelPrefab&>;

auto InstantiatePrefab(
    RenderContext&      ctx,
    ECS::Registry&      reg,
    PhysicsContext&     pc,
    ArticulationSystem& art,
    const ModelPrefab&  prefab,
    const SpawnParams&  params,
    Entity*             outBuffer = nullptr,
    uint32_t            maxCount  = 0
) -> uint32_t;

auto InstantiatePrefab(Engine& engine, const ModelPrefab& prefab, const SpawnParams& params, Entity* outBuffer = nullptr, uint32_t maxCount = 0) -> uint32_t;
auto InstantiatePrefab(Engine& engine, std::string_view path, const SpawnParams& params, Entity* outBuffer = nullptr, uint32_t maxCount = 0) -> uint32_t;

void RebuildVulkanResources(RenderContext& ctx, ECS::Registry& reg);

}
