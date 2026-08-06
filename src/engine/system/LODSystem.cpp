// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "LODSystem.hpp"
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/ecs/ECS.hpp>

namespace ZHLN {

void LODSystem::Update(Engine& engine) {
    auto& reg = engine.GetRegistry();
    auto& cam = engine.GetCamera();

    auto entities = reg.GetEntitiesWith<Components::LODComponent>();
    if (entities.empty()) {
        return;
    }

    auto lods = reg.GetRawArray<Components::LODComponent>();

    TaskSystem::ParallelFor(entities.size(), 256, [&](uint32_t start, uint32_t end, uint32_t) {
        for (uint32_t i = start; i < end; ++i) {
            Entity e        = entities[i];
            auto&  lodGroup = lods[i];
            if (lodGroup.count == 0) {
                continue;
            }

            auto* meshComp = reg.Get<Components::MeshComponent>(e);
            if (!meshComp) {
                continue;
            }

            float dist = (meshComp->worldTransform.GetTranslation() - cam.position).Length();

            // Default to the lowest detail mesh available
            uint8_t selectedLOD = lodGroup.count - 1;

            for (uint8_t l = 0; l < lodGroup.count; ++l) {
                if (dist <= lodGroup.levels[l].distance) {
                    selectedLOD = l;
                    break;
                }
            }

            if (selectedLOD != lodGroup.currentLOD) {
                lodGroup.currentLOD = selectedLOD;
                meshComp->meshAsset = lodGroup.levels[selectedLOD].meshAsset;
            }
        }
    });
}

} // namespace ZHLN
