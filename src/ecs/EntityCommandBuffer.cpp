// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/ecs/EntityCommandBuffer.hpp>
#include <Zahlen/Core/HashMap.hpp>

namespace ZHLN::ECS {

void EntityCommandBuffer::Playback() {
    ZHLN::HashMap<uint32_t, Entity> tempToRealMap;

    for (auto& cmd: _commands) { // Use non-const reference so we can mutate cmd
        Entity target = cmd.entity;

        if (target.generation == 0xFFFFFFFF) {
            if (const auto* realEntity = tempToRealMap.Find(target.index)) {
                target = *realEntity;
            }
        }

        switch (cmd.type) {
            case CommandType::Create: {
                tempToRealMap.Insert(cmd.entity.index, _registry->Create());
                break;
            }
            case CommandType::Destroy: {
                _registry->Destroy(target);
                break;
            }
            case CommandType::AddComponent: {
                if (cmd.applyFn != nullptr && cmd.componentData != nullptr) {
                    cmd.applyFn(*_registry, target, cmd.componentData);
                }
                if (cmd.destructor != nullptr && cmd.componentData != nullptr) {
                    cmd.destructor(cmd.componentData);
                    cmd.componentData = nullptr; // Prevent double-free in Reset()
                }
                break;
            }
        }
    }
    Reset();
}

void EntityCommandBuffer::Reset() noexcept {
    for (auto& cmd: _commands) {
        if (cmd.type == CommandType::AddComponent && cmd.componentData != nullptr) {
            if (cmd.destructor != nullptr) {
                cmd.destructor(cmd.componentData);
                cmd.componentData = nullptr;
            }
        }
    }
    _commands.clear();
    _tempIndexCounter = 0xF0000000;
}

} // namespace ZHLN::ECS
