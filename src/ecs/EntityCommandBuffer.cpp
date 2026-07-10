// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "EntityCommandBuffer.hpp"
#include <unordered_map>

namespace ZHLN::ECS {

void EntityCommandBuffer::Playback() {
    std::unordered_map<uint32_t, Entity> tempToRealMap;

    for (const auto& cmd: _commands) {
        Entity target = cmd.entity;

        if (target.generation == 0xFFFFFFFF) {
            auto it = tempToRealMap.find(target.index);
            if (it != tempToRealMap.end()) {
                target = it->second;
            }
        }

        switch (cmd.type) {
            case CommandType::Create: {
                tempToRealMap[cmd.entity.index] = _registry->Create();
                break;
            }
            case CommandType::Destroy: {
                _registry->Destroy(target);
                break;
            }
            case CommandType::AddComponent: {
                _registry->EnsureComponentCapacity(cmd.familyId);
                SparseSet* set = (_registry->GetRawByFamily(target, cmd.familyId) != nullptr) ? nullptr : _registry->_components[cmd.familyId];
                if (set != nullptr) {
                    set->Insert(target, cmd.componentData);
                }
                if (cmd.destructor != nullptr && cmd.componentData != nullptr) {
                    cmd.destructor(cmd.componentData);
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
        }
    }
    _commands.clear();
    _tempIndexCounter = 0xF0000000;
}

} // namespace ZHLN::ECS
