// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/Components.hpp>
#include <Zahlen/ecs/SystemParameters.hpp>

namespace ZHLN {

class ZHLN_API TransformSystem {
  public:
    // A raw Registry& is explicit: a missing WorldTransformComponent must be
    // inserted, so the graph serialises this structural writer.
    static void Update(ECS::Query<const Components::HierarchyComponent, const Components::TransformComponent,
                                  Components::WorldTransformComponent&> query,
                       ECS::Registry& registry) noexcept;

    // The post-present history step lives in FrameScheduler, not SystemGraph.
    static void UpdateTransformHistory(ECS::Registry& registry) noexcept;
};

} // namespace ZHLN
