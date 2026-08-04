// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Zahlen/Engine.hpp"

namespace ZHLN {

class DecalSystem {
  public:
    /**
     * @brief Iterates all entities with DecalComponent + TransformComponent
     *        and submits them to the renderer via Renderer::DrawDecal.
     */
    static void Update(Engine& engine);
};

} // namespace ZHLN
