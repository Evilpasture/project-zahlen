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

#include <Zahlen/COM/COMTypes.hpp>
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
    [[nodiscard]] virtual Result<uint32_t> Instantiate(uint64_t hash, const ChainInNode* spawnChain, Entity* entities,
                                                       uint32_t capacity) noexcept = 0;

    // Spawns a scene-owned procedural primitive entity (mesh, material,
    // transforms, OwnedMeshComponent, and optional PhysicsComponent). Accepts
    // exactly one shape node (BoxMeshDesc, PlaneMeshDesc, SphereMeshDesc,
    // CylinderMeshDesc, or ConeMeshDesc) plus optional PrefabFactory::SpawnParams.
    [[nodiscard]] virtual Result<Entity> SpawnPrimitive(const ChainInNode* primitiveChain) noexcept = 0;

    [[nodiscard]] virtual Option<EnvironmentImageInfo> FindEnvironmentImage(StringView key) const noexcept = 0;

    // Borrowed linear RGBA floats owned by the cache: valid until the cache is
    // cleared or the engine is destroyed. Empty when the key is unknown.
    [[nodiscard]] virtual Span<const float> EnvironmentPixels(StringView key) const noexcept = 0;

    // --- Ergonomics: compiled in the consumer's TU, never in the library ---

    [[nodiscard]] auto FindPrefab(StringView virtualPath) const noexcept -> Result<uint64_t> {
        const uint64_t hash = PrefabHash(virtualPath);
        if (!HasPrefab(hash)) return ComError::AssetNotCached;
        return hash;
    }

    [[nodiscard]] auto PrefabVirtualPath(uint64_t hash) const -> std::string {
        const uint32_t length = PrefabVirtualPath(hash, nullptr, 0);
        if (length == 0) return {};

        std::string path(length, '\0');
        PrefabVirtualPath(hash, path.data(), length + 1);
        return path;
    }

    [[nodiscard]] auto Instantiate(uint64_t hash, const PrefabFactory::SpawnParams& params = {}) -> std::vector<Entity> {
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

    [[nodiscard]] auto SpawnBox(ZHLN::FVec3 halfExtents, const PrefabFactory::SpawnParams& params = {}) noexcept
        -> Result<Entity> {
        return SpawnPrimitive(InChain(BoxMeshDesc {.halfExtents = halfExtents, .color = params.color}, params));
    }

    [[nodiscard]] auto SpawnPlane(float extent = 10.0F, ZHLN::FVec4 color = {0.6F, 0.6F, 0.6F, 1.0F},
                                  const PrefabFactory::SpawnParams& params = {}) noexcept -> Result<Entity> {
        return SpawnPrimitive(InChain(PlaneMeshDesc {.extent = extent, .color = color}, params));
    }

    [[nodiscard]] auto SpawnSphere(float radius = 0.5F, const PrefabFactory::SpawnParams& params = {}) noexcept
        -> Result<Entity> {
        return SpawnPrimitive(InChain(SphereMeshDesc {.radius = radius, .color = params.color}, params));
    }

    [[nodiscard]] auto SpawnCylinder(float radius = 0.5F, float height = 1.0F,
                                     const PrefabFactory::SpawnParams& params = {}) noexcept -> Result<Entity> {
        return SpawnPrimitive(
            InChain(CylinderMeshDesc {.radius = radius, .height = height, .color = params.color}, params));
    }

    [[nodiscard]] auto SpawnCone(float radius = 0.5F, float height = 1.0F,
                                 const PrefabFactory::SpawnParams& params = {}) noexcept -> Result<Entity> {
        return SpawnPrimitive(
            InChain(ConeMeshDesc {.radius = radius, .height = height, .color = params.color}, params));
    }

protected:
    ~IAssetLibrary() = default;
};

} // namespace ZHLN::COM
