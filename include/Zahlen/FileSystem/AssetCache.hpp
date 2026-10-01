// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#include <Zahlen/Core/HashMap.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <cstdint>
#include <memory>
#include <vector>

namespace ZHLN::FS {

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

    [[nodiscard]] auto Find(uint64_t id) const noexcept -> T* {
        return Lock(_mutex, [&]() -> T* {
            const auto* entry = _map.Find(id);
            return entry != nullptr ? *entry : nullptr;
        });
    }

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

    void Insert(uint64_t id, T* asset) {
        Insert(id, std::unique_ptr<T>(asset));
    }

    void Clear() noexcept {
        Lock(_mutex, [&] {
            _map.Clear();
            _owned.clear();
        });
    }

    template <typename Fn>
    void ForEach(Fn&& fn) {
        Lock(_mutex, [&] {
            for (auto& asset: _owned) {
                fn(*asset);
            }
        });
    }

    [[nodiscard]] auto Count() const noexcept -> size_t {
        return Lock(_mutex, [&]() -> size_t { return _owned.size(); });
    }

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

}


