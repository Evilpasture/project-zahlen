// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace ZHLN {

class Engine;
namespace ECS {
class Registry;
}

struct SceneCleanupSystem {
    static void ProcessPending(Engine& engine, ECS::Registry& registry);
    static void ClearAll(Engine& engine, ECS::Registry& registry);
};

}
