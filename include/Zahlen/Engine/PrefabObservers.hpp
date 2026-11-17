// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Zahlen/ECS/Entity.hpp>
#include <vector>
namespace ZHLN {
class Engine;
struct ModelPrefab;
namespace PrefabFactory {
struct SpawnParams;
}
// Optional-module notifications, invoked synchronously as a prefab root is
// created. No domain-specific component or subsystem is named by the factory.
struct PrefabObservers {
    using Callback = void (*)(Engine&, const ModelPrefab&, const PrefabFactory::SpawnParams&, Entity);
    std::vector<Callback> onRoot;
};
} // namespace ZHLN
