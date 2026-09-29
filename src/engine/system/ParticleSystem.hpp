// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/Components.hpp>
#include <Zahlen/ecs/SystemParameters.hpp>

namespace ZHLN {

class RenderContext;
struct Camera;

class ZHLN_API ParticleSystem {
  public:
    ParticleSystem()  = default;
    ~ParticleSystem() = default;

    ParticleSystem(const ParticleSystem&)            = delete;
    ParticleSystem& operator=(const ParticleSystem&) = delete;
    ParticleSystem(ParticleSystem&&)                 = default;
    ParticleSystem& operator=(ParticleSystem&&)      = default;

    static void Update(ECS::Query<Components::ParticleEmitterComponent, Components::MeshParticleEmitterComponent> emitters,
                       ECS::ResMut<RenderContext> render, ECS::Res<Camera> camera);
};

}
