// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Asset caching and lookup. Everything here is a value or an operation; the
// manager itself stays inside Engine.

#include <Zahlen/Common.h>
#include <Zahlen/Core/Optional.hpp>
#include <Zahlen/Render/EnvironmentImage.hpp>
#include <concepts>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ZHLN {

class AssetManager;
struct ModelPrefab;

namespace Remote {
class AsyncAssetFetcher;
}

class ZHLN_API AssetLibrary {
  public:
    // Non-owning borrow of a cached prefab. Invalidated by ClearCache() or
    // device-loss recovery; prefer WithCachedPrefab when the whole read must
    // be atomic with respect to the cache lock.
    [[nodiscard]] auto CachedPrefab(uint64_t hash) noexcept -> ZHLN::Optional<ModelPrefab&>;
    [[nodiscard]] auto CachedPrefab(uint64_t hash) const noexcept -> ZHLN::Optional<const ModelPrefab&>;

    // Visits the cached prefab inside the cache's lock. Returns false when the
    // hash is not cached. The callable is type-erased through a trampoline so
    // this facade keeps a light header (no AssetManager/Vulkan includes).
    // The facade's visit is bool-valued: its trampoline cannot carry a value
    // back out, so only void visitors are accepted here. AssetManager offers
    // the value-returning form for internal callers.
    template <typename Fn>
        requires std::invocable<Fn, const ModelPrefab&> &&
                 std::is_void_v<std::invoke_result_t<Fn, const ModelPrefab&>>
    [[nodiscard]] auto WithCachedPrefab(uint64_t hash, Fn&& fn) const -> bool {
        return WithCachedPrefabImpl(
            hash,
            +[](const void* ctx, const ModelPrefab& prefab) {
                (*static_cast<const std::remove_reference_t<Fn>*>(ctx))(prefab);
            },
            std::addressof(fn)
        );
    }

    // The one approved escape hatch: an explicitly borrowed, non-owning
    // pointer for importers and test harnesses that must hand a prefab to a
    // caller. Invalidated by ClearCache() / device-loss recovery. Do not use
    // WithCachedPrefab as a pointer-extractor; use this instead.
    [[nodiscard]] auto BorrowCachedPrefab(uint64_t hash) const noexcept -> const ModelPrefab*;

    void               CachePrefab(uint64_t hash, ModelPrefab* prefab);
    void               CachePrefab(uint64_t hash, std::unique_ptr<ModelPrefab> prefab);
    [[nodiscard]] auto CacheEnvironmentImage(std::string_view key, EnvironmentImage image) -> bool;
    [[nodiscard]] auto FindEnvironmentImage(std::string_view key) const noexcept -> std::optional<EnvironmentImageView>;
    void               SetRemoteFetcher(Remote::AsyncAssetFetcher* fetcher) noexcept;


  private:
    // Type-erased trampoline so this facade keeps a light header; the callable
    // stays on the caller's stack for the duration of the synchronous visit.
    [[nodiscard]] auto WithCachedPrefabImpl(
        uint64_t hash, void (*visit)(const void*, const ModelPrefab&), const void* ctx
    ) const -> bool;

    friend class Engine;
    explicit AssetLibrary(AssetManager& assets) noexcept: _assets(&assets) {
    }
    AssetManager* _assets;
};

} // namespace ZHLN
