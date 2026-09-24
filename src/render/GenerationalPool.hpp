// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/GenerationalPool.hpp
//
// A fixed-capacity pool of heap objects addressed by a packed
// (generation, index) handle. Destroying an entry bumps its generation, so a
// handle held past the object's lifetime fails the generation check and
// resolves to nothing instead of onto the slot's new occupant -- the same
// stale-handle discipline DestinationRegistry enforces for windows.
//
// Lifted out of RenderInternal.hpp because GeometryManager owns the buffer
// handle table and cannot include the renderer's private header: DI means the
// manager does not know the context exists, and that only works if the
// containers it is built from are reachable on their own.

#pragma once
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/MemoryPool.hpp>
#include <Zahlen/Log.hpp>
#include <array>
#include <cstdint>
#include <expected>
#include <utility>

namespace ZHLN {

// GenerationalPool Template

template <typename T, size_t MaxObjects, typename HandleType = uint64_t>
class GenerationalPool {
  public:
    enum class Error : uint8_t {
        InvalidHandle = 1, // The handle was 0/Null
        StaleHandle,       // Generational mismatch (the resource was already destroyed)
        OutOfBoundsIndex,  // Index exceeds pool capacity
        NullResource       // Internal error: slot points to null pointer
    };

    GenerationalPool() {
        _freeIndices.reserve(MaxObjects);
        for (size_t i = 0; i < MaxObjects; ++i) {
            _freeIndices.push_back(MaxObjects - 1 - i);
        }
        _generations.fill(1); // Generations start at 1
    }

    ~GenerationalPool() {
        // Automatically sweeps and safely destroys all remaining active allocations on shutdown
        for (size_t i = 0; i < MaxObjects; ++i) {
            if (_pointers[i] != nullptr) {
                _pool.Destroy(_pointers[i]);
            }
        }
    }

    // Non-copyable, non-movable matching engine context lifetime
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

    void Destroy(HandleType handle) {
        auto rawHandle = static_cast<uint64_t>(handle);
        auto index     = static_cast<uint32_t>(rawHandle & 0xFFFFFFFF);
        auto gen       = static_cast<uint32_t>(rawHandle >> 32);

        if (index >= MaxObjects || _generations[index] != gen || _pointers[index] == nullptr) {
            return; // Safely ignore stale or invalid handles
        }

        _pool.Destroy(_pointers[index]);
        _pointers[index] = nullptr;
        _generations[index]++; // Increment generation to invalidate stale handles
        _freeIndices.push_back(index);
    }

    [[nodiscard]] auto Resolve(HandleType handle) const noexcept -> std::expected<T*, Error> {
        auto rawHandle = static_cast<uint64_t>(handle);
        if (rawHandle == 0) [[unlikely]] {
            return std::unexpected(Error::InvalidHandle);
        }

        auto index = static_cast<uint32_t>(rawHandle & 0xFFFFFFFF);
        auto gen   = static_cast<uint32_t>(rawHandle >> 32);

        if (index >= MaxObjects) [[unlikely]] {
            return std::unexpected(Error::OutOfBoundsIndex);
        }
        if (_generations[index] != gen) [[unlikely]] {
            return std::unexpected(Error::StaleHandle);
        }
        if (_pointers[index] == nullptr) [[unlikely]] {
            return std::unexpected(Error::NullResource);
        }

        return _pointers[index];
    }

  private:
    ObjectPool<T, MaxObjects>        _pool;
    std::array<T*, MaxObjects>       _pointers {};
    std::array<uint32_t, MaxObjects> _generations {};
    ZHLN::Array<uint32_t>            _freeIndices;
};

} // namespace ZHLN
