// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/COM/IAssetLibrary.hpp
//
// Assets and scene-owned primitives as the public API sees them. ModelPrefab
// holds std::vector, Jolt shape references and a deleted copy constructor, so
// the prefab itself never crosses: the caller names a prefab by the hash of its
// virtual path, asks the library what is in it, and asks the library to spawn
// it. Spawn options and procedural primitive descriptors cross via reflected
// ChainInNode lists.

#include <Zahlen/COM/Types.hpp>
#include <Zahlen/COM/AssetTypes.hpp>
#include <Zahlen/Render/EnvironmentImage.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/IObject.hpp>
#include <Zahlen/Engine/SpawnParams.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>

#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

namespace ZHLN::COM {

static_assert(std::is_standard_layout_v<PrefabFactory::SpawnParams> &&
                  std::is_trivially_copyable_v<PrefabFactory::SpawnParams>,
              "SpawnParams crosses via ChainInNode and must stay a fixed-layout value.");

// What a cached prefab contains, without handing over the prefab. Enough to
// size a scene, drive an asset browser or assert in a test.
struct PrefabInfo {
    uint64_t hash           = 0;
    uint32_t partCount      = 0;
    uint32_t nodeCount      = 0;
    uint32_t skeletonCount  = 0;
    uint32_t animationCount = 0;
    uint32_t lightCount     = 0;
    // Largest per-part bounding radius, in metres. Zero for an empty prefab.
    float boundingRadius = 0.0F;
    float emissiveFactorScale = 100.0f;
    uint32_t maxTextureDimension = 1024;
    // False after device invalidation until replacement succeeds.
    bool gpuResourcesReady = true;
    uint64_t gpuDeviceGeneration = 0;
    uint64_t gpuRevision = 0;
};

static_assert(std::is_standard_layout_v<PrefabInfo> && std::is_trivially_copyable_v<PrefabInfo>);

struct EnvironmentImageInfo {
    uint64_t contentHash = 0;
    uint32_t width       = 0;
    uint32_t height      = 0;
    bool     hasSun      = false;
    // True when a sunless panorama was prepared for lighting; the visible sky
    // is always the original.
    bool hasLightingPanorama = false;
};

static_assert(std::is_standard_layout_v<EnvironmentImageInfo> && std::is_trivially_copyable_v<EnvironmentImageInfo>);

struct IAssetLibrary : IObject {
    // Deterministic 64-bit asset/material key for a name or path.
    [[nodiscard]] virtual AssetID AssetHash(StringView name) const noexcept = 0;

    // The cache key for a virtual path. Pure function of the string, so a
    // caller can compute it once and keep it.
    [[nodiscard]] virtual uint64_t PrefabHash(StringView virtualPath) const noexcept = 0;

    [[nodiscard]] virtual bool HasPrefab(uint64_t hash) const noexcept = 0;

    [[nodiscard]] virtual Option<PrefabInfo> DescribePrefab(uint64_t hash) const noexcept = 0;

    virtual Result<void> GetPrefabBounds(uint64_t hash, ChainOutNode* bounds) const noexcept = 0;
    // Exports cached prefab nodes, part bounds, skeletons and animation clips
    // into PrefabRigCounts and/or PrefabRigData output chain nodes.
    virtual Result<void> ReadPrefabRig(uint64_t hash, ChainOutNode* out) const noexcept = 0;

    // Copies the prefab's virtual path into `buffer` and null-terminates it
    // when there is room. Returns the length the path actually needs, which
    // may exceed `capacity`; zero means nothing is cached under that hash.
    [[nodiscard]] virtual uint32_t PrefabVirtualPath(uint64_t hash, char* buffer, uint32_t capacity) const noexcept = 0;

    // Spawns the cached prefab and records the entities it created into
    // `entities`, up to `capacity`. `spawnChain` accepts nullptr (defaults) or
    // PrefabFactory::SpawnParams via COM::InChain.
    //
    // `entities` controls *recording*, not spawning: calling this with a null
    // buffer still instantiates the whole prefab and discards the ids. Size the
    // buffer first from PrefabInfo (the inline overload below does this) and
    // call once. Zero inside Result means nothing is cached under `hash`.
    virtual Result<uint32_t> Instantiate(uint64_t hash, const ChainInNode* spawnChain, Entity* entities,
                                                       uint32_t capacity) noexcept = 0;

    // Spawns a scene-owned procedural primitive entity (mesh, material,
    // transforms, OwnedMeshComponent, and optional PhysicsComponent). Accepts
    // exactly one shape node (BoxMeshDesc, PlaneMeshDesc, SphereMeshDesc,
    // CylinderMeshDesc, or ConeMeshDesc) plus optional PrefabFactory::SpawnParams.
    // Optional MeshBuildOptions explicitly selects BLAS construction (default
    // true here for native spawn compatibility); retained across device rebuilds.
    virtual Result<Entity> SpawnPrimitive(const ChainInNode* primitiveChain) noexcept = 0;

    [[nodiscard]] virtual Option<EnvironmentImageInfo> FindEnvironmentImage(StringView key) const noexcept = 0;

    // Borrowed linear RGBA floats owned by the cache: valid until the cache is
    // cleared or the engine is destroyed. Empty when the key is unknown.
    [[nodiscard]] virtual Span<const float> EnvironmentPixels(StringView key) const noexcept = 0;

    virtual Result<void> MountDirectory(StringView path) noexcept = 0;
    virtual Result<void> UnmountDirectory(StringView path) noexcept = 0;
    virtual Result<void> MountPak(StringView path) noexcept = 0;
    [[nodiscard]] virtual bool VirtualFileExists(StringView path) const noexcept = 0;
    // Exact bytes; short buffers are untouched. A present empty file returns 0.
    virtual Result<uint64_t> ReadVirtualFile(StringView path, Span<uint8_t> bytes) const noexcept = 0;
    virtual void ClearCache() noexcept = 0;
    virtual Result<void> CacheCollisionMesh(const ChainInNode* chain) noexcept = 0;
    // Borrowed collision spans remain valid until cache replacement/clear or
    // destruction; never retain them across those operations.
    virtual Result<void> GetCollisionMesh(CollisionAssetID id, ChainOutNode* out) const noexcept = 0;
    virtual Result<void> CacheEnvironmentImage(const ChainInNode* chain) noexcept = 0;
    virtual Result<void> GetEnvironmentImage(StringView key, ChainOutNode* out) const noexcept = 0;
    // Copies flat data. Cached prefab GPU resources become cache-owned.
    // Do not destroy transferred mesh resources independently after success.
    virtual Result<uint64_t> RegisterPrefab(const ChainInNode* chain) noexcept = 0;

    // Validates every part before replacing aliases/retiring old buffers.
    // CPU nodes, colliders, animations and prefab address remain unchanged.
    // Live morph offsets/scratch buffers are rebound; animated weights and
    // joint palette offsets are retained. Returns the new monotonic revision.
    virtual Result<uint64_t> ReplacePrefabGPUResources(const ChainInNode* chain) noexcept = 0;

    // --- Ergonomics: compiled in the consumer's TU, never in the library ---

    auto FindPrefab(StringView virtualPath) const noexcept -> Result<uint64_t> {
        const uint64_t hash = PrefabHash(virtualPath);
        if (!HasPrefab(hash)) return Error::AssetNotCached;
        return hash;
    }

    [[nodiscard]] auto PrefabVirtualPath(uint64_t hash) const -> std::string {
        const uint32_t length = PrefabVirtualPath(hash, nullptr, 0);
        if (length == 0) return {};

        std::string path(length, '\0');
        PrefabVirtualPath(hash, path.data(), length + 1);
        return path;
    }

    auto Instantiate(uint64_t hash, const PrefabFactory::SpawnParams& params = {}) -> std::vector<Entity> {
        const Option<PrefabInfo> info = DescribePrefab(hash);
        if (!info.HasValue()) return {};

        const size_t perPart = params.emissiveVirtualLights ? 2U : 1U;
        const size_t bound   = 1U + (static_cast<size_t>(info.value.partCount) * perPart) + info.value.lightCount;

        std::vector<Entity>    entities(bound);
        const Result<uint32_t> outcome = Instantiate(hash, InChain(params), entities.data(), static_cast<uint32_t>(bound));
        if (!outcome.HasValue()) return {};
        const uint32_t recorded = outcome.value;
        entities.resize(recorded < bound ? recorded : bound);
        return entities;
    }

    auto SpawnBox(ZHLN::FVec3 halfExtents, const PrefabFactory::SpawnParams& params = {}) noexcept -> Entity {
        const Result<Entity> spawned =
            SpawnPrimitive(InChain(BoxMeshDesc {.halfExtents = halfExtents, .color = params.color}, params));
        return spawned.HasValue() ? spawned.value : Entity::Null();
    }

    auto SpawnPlane(float extent = 10.0F, ZHLN::FVec4 color = {0.6F, 0.6F, 0.6F, 1.0F},
                    const PrefabFactory::SpawnParams& params = {}) noexcept -> Entity {
        const Result<Entity> spawned =
            SpawnPrimitive(InChain(PlaneMeshDesc {.extent = extent, .color = color}, params));
        return spawned.HasValue() ? spawned.value : Entity::Null();
    }

    auto SpawnSphere(float radius = 0.5F, const PrefabFactory::SpawnParams& params = {}) noexcept -> Entity {
        const Result<Entity> spawned =
            SpawnPrimitive(InChain(SphereMeshDesc {.radius = radius, .color = params.color}, params));
        return spawned.HasValue() ? spawned.value : Entity::Null();
    }

    auto SpawnCylinder(float radius = 0.5F, float height = 1.0F,
                       const PrefabFactory::SpawnParams& params = {}) noexcept -> Entity {
        const Result<Entity> spawned =
            SpawnPrimitive(InChain(CylinderMeshDesc {.radius = radius, .height = height, .color = params.color}, params));
        return spawned.HasValue() ? spawned.value : Entity::Null();
    }

    auto SpawnCone(float radius = 0.5F, float height = 1.0F,
                   const PrefabFactory::SpawnParams& params = {}) noexcept -> Entity {
        const Result<Entity> spawned =
            SpawnPrimitive(InChain(ConeMeshDesc {.radius = radius, .height = height, .color = params.color}, params));
        return spawned.HasValue() ? spawned.value : Entity::Null();
    }

    auto CacheEnvironmentImage(StringView key, const EnvironmentImage& img) noexcept -> Result<void> {
        return CacheEnvironmentImage(InChain(EnvironmentImageCacheInfo {
            .key          = key,
            .extent       = {.width = img.width, .height = img.height},
            .rgba         = Span<const float>(img.rgba.data(), img.rgba.size()),
            .lightingRgba = Span<const float>(img.lightingRgba.data(), img.lightingRgba.size()),
            .sun          = img.sun ? Option<EnvironmentSun>(*img.sun) : Option<EnvironmentSun>{},
            .contentHash  = img.contentHash,
        }));
    }

protected:
    ~IAssetLibrary() = default;
};

} // namespace ZHLN::COM
