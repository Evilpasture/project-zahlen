// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#include <Zahlen/Core/HashMap.hpp>
#include <Zahlen/Core/Optional.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <concepts>
#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
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

    // The lock stays held for the whole visit. A lookup that returned a
    // reference (or a pointer) would drop the mutex before the caller reads
    // it: Clear() or a move could then retire the asset mid-read.
    // A void visitor gets bool (was the id found?). A visitor that returns a
    // value gets std::optional<Ret>, so a value snapshot can leave the lock
    // without an out-parameter. References are decayed: no borrowed reference
    // can be carried out of the lock through the return value either.
    template <typename Fn>
        requires std::invocable<Fn, T&>
    [[nodiscard]] auto With(uint64_t id, Fn&& fn) const {
        using Result = std::invoke_result_t<Fn, T&>;
        if constexpr (std::is_void_v<Result>) {
            return Lock(_mutex, [&]() -> bool {
                const auto entry = _map.Find(id);
                if (!entry || *entry == nullptr) {
                    return false;
                }
                std::forward<Fn>(fn)(**entry);
                return true;
            });
        } else {
            using Ret = std::remove_cvref_t<Result>;
            return Lock(_mutex, [&]() -> std::optional<Ret> {
                const auto entry = _map.Find(id);
                if (!entry || *entry == nullptr) {
                    return std::nullopt;
                }
                return std::forward<Fn>(fn)(**entry);
            });
        }
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


