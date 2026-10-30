// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <Zahlen/Common.h>
#include <Zahlen/Components.hpp>
#include <Zahlen/ECS/SystemParameters.hpp>

namespace ZHLN {
class Engine;
class ZHLN_API LODSystem {
  public:
    using LODQuery = ECS::Query<
        Components::LODComponent&, Components::MeshComponent&, const Components::CameraComponent,
        const Components::WorldTransformComponent, const Components::TransformComponent>;

    static void GraphUpdate(LODQuery query);
    static void Update(Engine& engine);
};
}
