// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstddef>
#include <utility>

namespace ZHLN::Physics {

// Standard-layout fixed-capacity buffer of std::atomic<T> elements.
// Sized in Init() and released in Shutdown() without reallocating elements,
// keeping stable addresses for lock-free readers and preserving PhysicsWorld's
// standard-layout ABI.
template <typename T>
class AtomicBuffer {
  public:
    AtomicBuffer() noexcept = default;
    ~AtomicBuffer() noexcept {
        clear();
    }

    AtomicBuffer(const AtomicBuffer&)                    = delete;
    auto operator=(const AtomicBuffer&) -> AtomicBuffer& = delete;

    AtomicBuffer(AtomicBuffer&& other) noexcept: _data(std::exchange(other._data, nullptr)), _size(std::exchange(other._size, 0)) {}

    auto operator=(AtomicBuffer&& other) noexcept -> AtomicBuffer& {
        if (this != &other) {
            clear();
            _data = std::exchange(other._data, nullptr);
            _size = std::exchange(other._size, 0);
        }
        return *this;
    }

    void resize(size_t count) {
        clear();
        if (count > 0) {
            _data = new std::atomic<T>[count] {};
            _size = count;
        }
    }

    void clear() noexcept {
        delete[] _data;
        _data = nullptr;
        _size = 0;
    }

    [[nodiscard]] auto size() const noexcept -> size_t {
        return _size;
    }

    [[nodiscard]] auto empty() const noexcept -> bool {
        return _size == 0;
    }

    [[nodiscard]] auto data() noexcept -> std::atomic<T>* {
        return _data;
    }

    [[nodiscard]] auto data() const noexcept -> const std::atomic<T>* {
        return _data;
    }

    [[nodiscard]] auto operator[](size_t index) noexcept -> std::atomic<T>& {
        return _data[index];
    }

    [[nodiscard]] auto operator[](size_t index) const noexcept -> const std::atomic<T>& {
        return _data[index];
    }

  private:
    std::atomic<T>* _data = nullptr;
    size_t          _size = 0;
};

} // namespace ZHLN::Physics
