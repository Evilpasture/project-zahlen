// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Core/AssetCache.hpp
//
// A generic, type-erased-free cache that only knows identity, lifetime and
// caching. It does NOT know how to load, parse or instantiate T. That is the
// job of the factory that produces T and then hands ownership to the cache.
//
// Thread-safe for the engine's use: every public operation locks internally.
// Ownership is via std::unique_ptr<T> (R.11/R.20), no explicit new/delete in
// cache management. The HashMap holds non-owning raw pointers for O(1) lookup;
// the vector of unique_ptr owns.

#pragma once

#include <Zahlen/Core/HashMap.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <cstdint>
#include <memory>
#include <vector>

namespace ZHLN {

template <typename T>
class AssetCache {
  public:
    AssetCache() = default;
    ~AssetCache() {
        Clear();
    }

    AssetCache(const AssetCache&) = delete;
    AssetCache& operator=(const AssetCache&) = delete;

    AssetCache(AssetCache&& other) noexcept {
        Lock(_mutex, [&] {
            Lock(other._mutex, [&] {
                _map = std::move(other._map);
                _owned = std::move(other._owned);
            });
        });
    }

    AssetCache& operator=(AssetCache&& other) noexcept {
        if (this != &other) {
            Clear();
            Lock(_mutex, [&] {
                Lock(other._mutex, [&] {
                    _map = std::move(other._map);
                    _owned = std::move(other._owned);
                });
            });
        }
        return *this;
    }

    // Identity: O(1) lookup by AssetID, non-owning raw pointer.
    [[nodiscard]] auto Find(uint64_t id) const noexcept -> T* {
        return Lock(_mutex, [&]() -> T* {
            const auto* entry = _map.Find(id);
            return entry != nullptr ? *entry : nullptr;
        });
    }

    // Lifetime + caching: takes ownership via unique_ptr. Does NOT load/parse.
    void Insert(uint64_t id, std::unique_ptr<T> asset) {
        if (asset == nullptr) {
            return;
        }
        Lock(_mutex, [&] {
            T* raw = asset.get();
            _map.Insert(id, raw);
            _owned.emplace_back(std::move(asset));
        });
    }

    // Compatibility overload: takes raw pointer and adopts ownership.
    // Prefer the unique_ptr overload; this exists for call sites that still
    // produce T* via `new T`.
    void Insert(uint64_t id, T* asset) {
        Insert(id, std::unique_ptr<T>(asset));
    }

    void Clear() noexcept {
        Lock(_mutex, [&] {
            _map.Clear();
            _owned.clear();
        });
    }

    [[nodiscard]] auto Count() const noexcept -> size_t {
        return Lock(_mutex, [&]() -> size_t { return _owned.size(); });
    }

    // Enumerate cached assets. Pass nullptr/0 to query count.
    auto GetAll(T** out, uint32_t maxCount) const noexcept -> uint32_t {
        return Lock(_mutex, [&]() -> uint32_t {
            if (out == nullptr || maxCount == 0) {
                return static_cast<uint32_t>(_owned.size());
            }
            uint32_t toCopy = std::min(static_cast<uint32_t>(_owned.size()), maxCount);
            for (uint32_t i = 0; i < toCopy; ++i) {
                out[i] = _owned[i].get();
            }
            return toCopy;
        });
    }

  private:
    mutable HashMap<uint64_t, T*> _map;
    mutable std::vector<std::unique_ptr<T>> _owned;
    mutable Mutex _mutex {};
};

} // namespace ZHLN
