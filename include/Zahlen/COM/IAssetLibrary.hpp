// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/COM/IAssetLibrary.hpp
//
// Assets as the public API sees it. ModelPrefab holds std::vector, Jolt shape
// references and a deleted copy constructor, so the prefab itself never
// crosses: the caller names a prefab by the hash of its virtual path, asks the
// library what is in it, and asks the library to spawn it. The bytes stay on
// the side that allocated them.
//
// That is also why the cache-filling half of the AssetLibrary facade is absent.
// CachePrefab takes a std::unique_ptr and CacheEnvironmentImage takes a struct
// of std::vectors; handing either across would mean one CRT allocating what
// another frees.
//
// Nor is there an import call. The engine's own LoadModelPrefab is a cache
// lookup -- every importer (glTF, BSP, StudioModel) lives in plugins/, a layer
// above libzahlen_engine.so, so the library has nothing to call and this
// boundary cannot invent one without inverting that dependency. What is here
// is honest about it: resolve a path to a cache key, read what is cached, and
// spawn it. Filling the cache stays with whoever linked the importer.

#include <Zahlen/COM/COMTypes.hpp>
#include <Zahlen/Core/COM/IObject.hpp>
#include <Zahlen/Engine/SpawnParams.hpp>

#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

namespace ZHLN::COM {

static_assert(std::is_standard_layout_v<PrefabFactory::SpawnParams> &&
                  std::is_trivially_copyable_v<PrefabFactory::SpawnParams>,
              "SpawnParams crosses by const reference and must stay a fixed-layout value.");

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
    // `entities`, up to `capacity`. Returns how many were recorded.
    //
    // Read that twice before reusing the count-then-fill shape from the query
    // methods: `entities` controls *recording*, not spawning. Calling this
    // with a null buffer still instantiates the whole prefab -- it just throws
    // away the ids. Size the buffer first (PrefabInfo gives the bound, and the
    // inline overload below does it for you) and call this exactly once.
    // Zero means nothing is cached under that hash.
    [[nodiscard]] virtual uint32_t Instantiate(uint64_t hash, const PrefabFactory::SpawnParams& params, Entity* entities,
                                               uint32_t capacity) noexcept = 0;

    [[nodiscard]] virtual Option<EnvironmentImageInfo> FindEnvironmentImage(StringView key) const noexcept = 0;

    // Borrowed linear RGBA floats owned by the cache: valid until the cache is
    // cleared or the engine is destroyed. Empty when the key is unknown.
    [[nodiscard]] virtual Span<const float> EnvironmentPixels(StringView key) const noexcept = 0;

    // --- Ergonomics: compiled in the consumer's TU, never in the library ---

    // Path to usable key in one step: the hash if something is cached under
    // it, ComError::AssetNotCached if not. Two virtual calls, no vtable slot.
    [[nodiscard]] auto FindPrefab(StringView virtualPath) const noexcept -> Result<uint64_t> {
        const uint64_t hash = PrefabHash(virtualPath);
        if (!HasPrefab(hash)) return ComError::AssetNotCached;
        return hash;
    }

    // The library writes through std::string's own buffer, including the
    // terminator slot at data()[size()], which is the one past-the-end byte
    // the standard lets a caller write as long as it stays a null.
    [[nodiscard]] auto PrefabVirtualPath(uint64_t hash) const -> std::string {
        const uint32_t length = PrefabVirtualPath(hash, nullptr, 0);
        if (length == 0) return {};

        std::string path(length, '\0');
        PrefabVirtualPath(hash, path.data(), length + 1);
        return path;
    }

    // One spawn, not two. The vector is sized from the prefab's own contents
    // -- root, one entity per part, one more per part when the spawn asks for
    // emissive virtual lights, and one per punctual light -- so the single
    // call to the wire method has room to record everything it creates, and
    // the vector shrinks to what came back.
    [[nodiscard]] auto Instantiate(uint64_t hash, const PrefabFactory::SpawnParams& params = {}) -> std::vector<Entity> {
        const Option<PrefabInfo> info = DescribePrefab(hash);
        if (!info.HasValue()) return {};

        const size_t perPart = params.emissiveVirtualLights ? 2U : 1U;
        const size_t bound   = 1U + (static_cast<size_t>(info.value.partCount) * perPart) + info.value.lightCount;

        std::vector<Entity> entities(bound);
        const uint32_t      recorded = Instantiate(hash, params, entities.data(), static_cast<uint32_t>(bound));
        entities.resize(recorded < bound ? recorded : bound);
        return entities;
    }

protected:
    ~IAssetLibrary() = default;
};

} // namespace ZHLN::COM
