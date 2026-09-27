// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/MemoryPool.hpp>
#include <Zahlen/Log.hpp>
#include <array>
#include <cstdint>
#include <expected>
#include <utility>

namespace ZHLN {


template <typename T, size_t MaxObjects, typename HandleType = uint64_t>
class GenerationalPool {
  public:
    enum class Error : uint8_t {
        InvalidHandle = 1,
        StaleHandle,
        OutOfBoundsIndex,
        NullResource
    };

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

}
