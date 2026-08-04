// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>

namespace ZHLN {

class Engine;

class ZHLN_API ParticleSystem {
  public:
    ParticleSystem()  = default;
    ~ParticleSystem() = default;

    ParticleSystem(const ParticleSystem&)            = delete;
    ParticleSystem& operator=(const ParticleSystem&) = delete;
    ParticleSystem(ParticleSystem&&)                 = default;
    ParticleSystem& operator=(ParticleSystem&&)      = default;

    /**
     * @brief Processes active particle emitters:
     *  - Lazily allocates GPU storage buffers for new emitters.
     *  - Resolves camera-relative attachment offsets.
     *  - Submits emitter render commands to the RenderContext for GPU compute update & rendering.
     */
    void Update(Engine& engine, float dt);
};

} // namespace ZHLN
