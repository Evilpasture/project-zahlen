// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <chrono>

namespace ZHLN {

class Clock {
  public:
    Clock(): _start(std::chrono::high_resolution_clock::now()), _last(_start) {
    }

    float GetDeltaTime() {
        auto  now = std::chrono::high_resolution_clock::now();
        float dt  = std::chrono::duration<float>(now - _last).count();
        _last     = now;
        return dt;
    }

    [[nodiscard]] double GetTotalTime() const {
        return std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - _start).count();
    }

  private:
    std::chrono::high_resolution_clock::time_point _start;
    std::chrono::high_resolution_clock::time_point _last;
};

} // namespace ZHLN
