// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Components.hpp>
#include <Zahlen/ecs/SystemParameters.hpp>

namespace ZHLN {
class RenderContext;

class DecalSystem {
  public:
    static void Update(ECS::Query<const Components::DecalComponent, const Components::WorldTransformComponent> decals,
                       ECS::ResMut<RenderContext> render);
};

}
