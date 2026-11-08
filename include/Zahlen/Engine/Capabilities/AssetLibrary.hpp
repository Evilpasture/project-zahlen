// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Asset caching and lookup. Everything here is a value or an operation; the
// manager itself stays inside Engine.

#include <Zahlen/Common.h>
#include <Zahlen/Render/EnvironmentImage.hpp>
#include <Zahlen/Core/Optional.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace ZHLN {

class AssetManager;
struct ModelPrefab;

namespace Remote {
class AsyncAssetFetcher;
}

class ZHLN_API AssetLibrary {
  public:
    [[nodiscard]] auto CachedPrefab(uint64_t hash) noexcept -> ZHLN::Optional<ModelPrefab&>;
    [[nodiscard]] auto CachedPrefab(uint64_t hash) const noexcept -> ZHLN::Optional<const ModelPrefab&>;
    void               CachePrefab(uint64_t hash, ModelPrefab* prefab);
    void               CachePrefab(uint64_t hash, std::unique_ptr<ModelPrefab> prefab);
    [[nodiscard]] auto CacheEnvironmentImage(std::string_view key, EnvironmentImage image) -> bool;
    [[nodiscard]] auto FindEnvironmentImage(std::string_view key) const noexcept -> std::optional<EnvironmentImageView>;
    void               SetRemoteFetcher(Remote::AsyncAssetFetcher* fetcher) noexcept;


  private:
    friend class Engine;
    explicit AssetLibrary(AssetManager& assets) noexcept: _assets(&assets) {
    }
    AssetManager* _assets;
};

} // namespace ZHLN
