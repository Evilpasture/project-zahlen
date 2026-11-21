// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Zahlen/Core/Reflection/CanonicalName.hpp>
#include <Zahlen/Core/Reflection/LayoutHash.hpp>
#include <ecs/ECS.hpp>
#include <engine/Engine.hpp>
#include <vector>
namespace ZHLN {
// Native optional-module publication into this engine's generic COM ECS bridge.
// Descriptors and their functions must outlive the engine (static libraries do).
struct PublishedComponent {
    std::string_view name;
    uint32_t         size, alignment;
    uint64_t         layoutHash;
    uint32_t (*familyID)() noexcept;
    void* (*ensure)(Engine&, ECS::Registry&, Entity) noexcept;
    bool (*remove)(Engine&, ECS::Registry&, Entity) noexcept;
    void* (*fetch)(const ECS::Registry&, Entity) noexcept;
};
struct PublishedComponents {
    std::vector<PublishedComponent> entries;
};
template <class T>
consteval PublishedComponent PublishModuleComponent() {
    static_assert(std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>);
    return {
        Reflect::CanonicalTypeName<T>(),
        sizeof(T),
        alignof(T),
        Reflect::LayoutHash<T>(),
        +[]() noexcept { return ECS::ComponentFamily::GetTypeID<T>(); },
        +[](Engine&, ECS::Registry& registry, Entity entity) noexcept -> void* {
            if (!registry.IsAlive(entity))
                return nullptr;
            if (auto existing = registry.Get<T>(entity))
                return &*existing;
            return &registry.Add<T>(entity);
        },
        +[](Engine&, ECS::Registry& registry, Entity entity) noexcept {
            if (!registry.Get<T>(entity))
                return false;
            registry.Remove<T>(entity);
            return true;
        },
        +[](const ECS::Registry& registry, Entity entity) noexcept -> void* { return registry.GetRawByFamily(entity, ECS::ComponentFamily::GetTypeID<T>()); }
    };
}
} // namespace ZHLN
