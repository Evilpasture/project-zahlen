// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/ECS/Components.hpp>
#include <ecs/SystemParameters.hpp>

namespace ZHLN {
class RenderContext;

class DecalSystem {
  public:
    static void Update(ECS::Query<const Components::DecalComponent, const Components::WorldTransformComponent> decals,
                       ECS::ResMut<RenderContext> render);
};

}
