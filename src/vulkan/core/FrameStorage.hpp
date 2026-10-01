// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "FrameConfig.hpp"
#include <array>
#include <cstdint>

namespace ZHLN {

// Physical resources owned by an in-flight GPU frame. Selection is explicit;
// there is no temporal "previous" state or implicit frame counter here.
template <typename T, uint32_t N = Vk::kFramesInFlight>
class PerFrame {
  public:
    static_assert(N > 0);
    static constexpr uint32_t Capacity = N;

    [[nodiscard]] constexpr auto operator[](uint32_t frameIndex) noexcept -> T& {
        return _data[frameIndex % N];
    }
    [[nodiscard]] constexpr auto operator[](uint32_t frameIndex) const noexcept -> const T& {
        return _data[frameIndex % N];
    }

    constexpr auto begin() noexcept {
        return _data.begin();
    }
    constexpr auto begin() const noexcept {
        return _data.begin();
    }
    constexpr auto end() noexcept {
        return _data.end();
    }
    constexpr auto end() const noexcept {
        return _data.end();
    }

  private:
    std::array<T, N> _data {};
};

// Two temporal states irrespective of the number of frames in flight: Current
// is written this frame, Previous is the result of the last frame. Logical
// roles do not imply physical safety: the owner must synchronize GPU readers
// before reusing either state.
template <typename T>
class PingPong {
  public:
    [[nodiscard]] constexpr auto Current() noexcept -> T& {
        return _data[_current];
    }
    [[nodiscard]] constexpr auto Current() const noexcept -> const T& {
        return _data[_current];
    }
    [[nodiscard]] constexpr auto Previous() noexcept -> T& {
        return _data[1u - _current];
    }
    [[nodiscard]] constexpr auto Previous() const noexcept -> const T& {
        return _data[1u - _current];
    }

    constexpr void Swap() noexcept {
        _current = 1u - _current;
    }

    constexpr auto begin() noexcept {
        return _data.begin();
    }
    constexpr auto begin() const noexcept {
        return _data.begin();
    }
    constexpr auto end() noexcept {
        return _data.end();
    }
    constexpr auto end() const noexcept {
        return _data.end();
    }

  private:
    std::array<T, 2> _data {};
    uint32_t         _current = 0;
};

}
