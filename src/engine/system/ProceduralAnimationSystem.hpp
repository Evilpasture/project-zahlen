// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>

namespace ZHLN {

class Engine;

/** Executes all procedural pose stages between physics and transform resolve. */
class ZHLN_API ProceduralAnimationSystem {
  public:
    ProceduralAnimationSystem()  = default;
    ~ProceduralAnimationSystem() = default;

    ProceduralAnimationSystem(const ProceduralAnimationSystem&)            = delete;
    ProceduralAnimationSystem& operator=(const ProceduralAnimationSystem&) = delete;

    void Update(Engine& engine, float dt) noexcept;
};

} // namespace ZHLN
