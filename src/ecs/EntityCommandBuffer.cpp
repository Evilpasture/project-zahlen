// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include <ecs/EntityCommandBuffer.hpp>
#include <algorithm>
#include <array>
#include <vector>

namespace ZHLN::ECS {

void EntityCommandBuffer::Playback() {
    constexpr size_t                    kStackTempLimit = 64;
    std::array<Entity, kStackTempLimit> stackTemps {};
    std::vector<Entity>                 heapTemps;
    Entity*                             tempToReal = nullptr;

    const size_t tempCount = _tempIndexCounter;
    if (tempCount <= kStackTempLimit) {
        std::fill_n(stackTemps.begin(), tempCount, Entity::Null());
        tempToReal = stackTemps.data();
    } else {
        heapTemps.resize(tempCount, Entity::Null());
        tempToReal = heapTemps.data();
    }

    for (auto& cmd: _commands) {
        Entity target = cmd.entity;

        if (target.generation == kTempGeneration && target.index < tempCount) {
            target = tempToReal[target.index];
        }

        switch (cmd.type) {
            case CommandType::Create: {
                if (cmd.entity.index < tempCount) {
                    tempToReal[cmd.entity.index] = _registry->Create();
                }
                break;
            }
            case CommandType::Destroy: {
                if (_destroy != nullptr) {
                    _destroy(*_registry, target);
                } else {
                    _registry->Destroy(target);
                }
                break;
            }
            case CommandType::AddComponent: {
                if (cmd.applyFn != nullptr && cmd.componentData != nullptr) {
                    cmd.applyFn(*_registry, target, cmd.componentData);
                }
                if (cmd.destructor != nullptr && cmd.componentData != nullptr) {
                    cmd.destructor(cmd.componentData);
                }
                cmd.componentData = nullptr;
                break;
            }
            case CommandType::RemoveComponent: {
                if (cmd.applyFn != nullptr) {
                    cmd.applyFn(*_registry, target, nullptr);
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
            }
            cmd.componentData = nullptr;
        }
    }
    _commands.clear();
    // Every payload lived in the arena, so the storage the destructors just ran
    // against is reclaimed in one store. Playback() nulls each payload it
    // consumed, which is why a Reset after one cannot destroy anything twice.
    _arena.Reset();
    _tempIndexCounter = 0;
}

} // namespace ZHLN::ECS
