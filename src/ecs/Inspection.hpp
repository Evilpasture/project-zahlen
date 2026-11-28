// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <ecs/ECS.hpp>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace ZHLN::ECS {

// Unscheduled, synchronous dynamic inspection for tools and scripting. The
// caller must exclude graph execution and structural changes while using a
// returned pointer or span. Pointers are invalidated by structural changes;
// boxed script values must resolve their entity and component again on use.
// This is not a graph parameter and grants no entity/component lifecycle API.
class Inspection {
  public:
    explicit Inspection(Registry& registry) noexcept: _registry(&registry) {}

    [[nodiscard]] auto IsAlive(Entity entity) const noexcept -> bool {
        return _registry->IsAlive(entity);
    }
    [[nodiscard]] auto IsRegistered(std::string_view name) const noexcept -> bool {
        return _registry->HasFamily(name);
    }
    [[nodiscard]] auto Component(Entity entity, std::string_view name) const noexcept -> void* {
        return _registry->GetRawByName(entity, name);
    }
    void Dump(Entity entity, std::string& out) const {
        _registry->DebugDumpEntity(entity, out);
    }
    template <typename T>
    void Register(std::string_view name) const {
        _registry->RegisterComponent<T>(name);
    }
    template <typename T>
    [[nodiscard]] auto EntitiesWith() const noexcept -> std::span<const Entity> {
        return _registry->GetEntitiesWith<T>();
    }
    template <typename T>
    [[nodiscard]] auto Read(Entity entity) const noexcept -> ZHLN::Optional<const T&> {
        return std::as_const(*_registry).Get<T>(entity);
    }

  private:
    Registry* _registry;
};

} // namespace ZHLN::ECS
