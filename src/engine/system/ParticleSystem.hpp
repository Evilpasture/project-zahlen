// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>

namespace ZHLN {

class Engine;
struct SystemContext;

class ZHLN_API ParticleSystem {
  public:
    ParticleSystem()  = default;
    ~ParticleSystem() = default;

    ParticleSystem(const ParticleSystem&)            = delete;
    ParticleSystem& operator=(const ParticleSystem&) = delete;
    ParticleSystem(ParticleSystem&&)                 = default;
    ParticleSystem& operator=(ParticleSystem&&)      = default;

    void Update(SystemContext& ctx, float dt);
};

}
