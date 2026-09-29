// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/MemoryPool.hpp>
#include <Zahlen/Log.hpp>
#include <array>
#include <cstdint>
#include <utility>

namespace ZHLN {


template <typename T, size_t MaxObjects, typename HandleType = uint64_t>
class GenerationalPool {
  public:
    GenerationalPool() {
        _freeIndices.reserve(MaxObjects);
        for (size_t i = 0; i < MaxObjects; ++i) {
            _freeIndices.push_back(MaxObjects - 1 - i);
        }
        _generations.fill(1);
    }

    ~GenerationalPool() {
        for (size_t i = 0; i < MaxObjects; ++i) {
            if (_pointers[i] != nullptr) {
                _pool.Destroy(_pointers[i]);
            }
        }
    }

    GenerationalPool(const GenerationalPool&)                    = delete;
    auto operator=(const GenerationalPool&) -> GenerationalPool& = delete;

    template <typename... Args>
    HandleType Create(Args&&... args) {
        if (_freeIndices.empty()) [[unlikely]] {
            ZHLN::Log(
                "ERROR: GenerationalPool has exceeded its maximum capacity of {}! Returning "
                "invalid handle.",
                MaxObjects
            );
            return static_cast<HandleType>(0);
        }
        uint32_t index = _freeIndices.back();
        _freeIndices.pop_back();

        uint32_t gen     = _generations[index];
        _pointers[index] = _pool.Create(std::forward<Args>(args)...);

        uint64_t packed = (static_cast<uint64_t>(gen) << 32) | index;
        return static_cast<HandleType>(packed);
    }

    template <typename Fn>
    void ForEachLive(Fn&& fn) {
        for (auto* pointer: _pointers) {
            if (pointer != nullptr) {
                fn(*pointer);
            }
        }
    }

    // Drop every live slot and invalidate its handle. Callers that own external
    // resources must retire those resources before clearing the pool.
    void Clear() noexcept {
        for (size_t index = 0; index < MaxObjects; ++index) {
            if (_pointers[index] != nullptr) {
                _pool.Destroy(_pointers[index]);
                _pointers[index] = nullptr;
                ++_generations[index];
                _freeIndices.push_back(static_cast<uint32_t>(index));
            }
        }
    }

    void Destroy(HandleType handle) {
        auto rawHandle = static_cast<uint64_t>(handle);
        auto index     = static_cast<uint32_t>(rawHandle & 0xFFFFFFFF);
        auto gen       = static_cast<uint32_t>(rawHandle >> 32);

        if (index >= MaxObjects || _generations[index] != gen || _pointers[index] == nullptr) {
            return;
        }

        _pool.Destroy(_pointers[index]);
        _pointers[index] = nullptr;
        _generations[index]++;
        _freeIndices.push_back(index);
    }

    // Null for every way a handle can fail to name a live object: zero,
    // out of range, stale generation, or a slot that was destroyed and has
    // not been refilled.
    //
    // This returned a std::expected<T*, Error> once, with four ways to fail:
    // InvalidHandle, StaleHandle, OutOfBoundsIndex, NullResource. Every call
    // site but one asked only "is there an object" -- twenty-three of them
    // wrote `.value_or(nullptr)` and then tested the pointer -- which a null
    // pointer answers without making them name a vocabulary they do not use.
    //
    // The exception is BuildMeshBLAS, whose failure MeshBuilder and the glTF
    // importer log as a warning. It gets a code of its own at the point of
    // use -- RenderFeatureError::UnresolvedMeshHandle -- so the log line
    // survives. Worth stating plainly: the four-way distinction is gone, so a
    // stale handle and an out-of-range one now read the same. If a bug ever
    // needs telling apart, that is the argument for bringing the error back,
    // and it is a better one than "somebody might want it".
    [[nodiscard]] auto Resolve(HandleType handle) const noexcept -> T* {
        const auto rawHandle = static_cast<uint64_t>(handle);
        if (rawHandle == 0) [[unlikely]] {
            return nullptr;
        }

        const auto index = static_cast<uint32_t>(rawHandle & 0xFFFFFFFF);
        const auto gen   = static_cast<uint32_t>(rawHandle >> 32);

        if (index >= MaxObjects) [[unlikely]] {
            return nullptr;
        }
        if (_generations[index] != gen) [[unlikely]] {
            return nullptr;
        }

        return _pointers[index];
    }

  private:
    ObjectPool<T, MaxObjects>        _pool;
    std::array<T*, MaxObjects>       _pointers {};
    std::array<uint32_t, MaxObjects> _generations {};
    ZHLN::Array<uint32_t>            _freeIndices;
};

}
