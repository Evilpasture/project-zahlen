// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
namespace ZHLN {
class GraphicsDevice;
namespace ECS { class Registry; }
namespace PrefabFactory {
// Engine's device-recovery barrier, not a public spawner.
void RebuildVulkanResources(GraphicsDevice gfx, ECS::Registry& registry);
}
}
