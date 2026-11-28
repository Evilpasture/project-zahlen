// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/COM/IEntities.hpp
//
// The ECS seam. A virtual function cannot be a template, so the vtable carries
// one unified set of type-erased operations keyed on a component's canonical
// qualified name (whose 64-bit FNV-1a hash matches Reflect::TypeKey<T>()), and
// the typed API is rebuilt from inline templates that compile in the consumer's
// own translation unit. Nothing about a consumer-defined component type needs
// to be known inside the library.
//
// Published engine components route to the engine's own typed SparseSets after
// verifying size, alignment, and non-zero recursive leaf LayoutHash, and run
// SceneResources retirement hooks on replacement and detach. Unpublished native
// engine components and short-name aliases are refused. Consumer-defined types
// auto-register dynamic raw byte storage on first AttachRaw.

#include <Zahlen/COM/Types.hpp>
#include <Zahlen/Core/COM/Collect.hpp>
#include <Zahlen/Core/COM/IObject.hpp>
#include <Zahlen/Core/Reflection/CanonicalName.hpp>
#include <Zahlen/Core/Reflection/LayoutHash.hpp>

#include <cstdint>
#include <new>
#include <type_traits>
#include <vector>

namespace ZHLN::COM {

template <typename T>
concept Component = std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T>;

struct IEntities : IObject {
    [[nodiscard]] virtual Entity CreateEntity() noexcept = 0;
    virtual void                 Despawn(Entity entity) noexcept = 0;
    [[nodiscard]] virtual bool   IsAlive(Entity entity) const noexcept = 0;

    // Reports whether `typeName` names a published engine component in typed
    // storage (false for unpublished native engine types and consumer dynamic
    // components).
    [[nodiscard]] virtual bool IsEngineComponent(StringView typeName) const noexcept = 0;

    // Unified raw component seam keyed on canonical `typeName` (hashed with
    // Hash64 to match Reflect::TypeKey<T>()).
    // - Published engine components require exact size, alignment, and non-zero
    //   layoutHash, route to typed SparseSets, and run retirement hooks before
    //   overwrite or detach.
    // - Unpublished native engine components and short-name aliases are refused.
    // - Consumer-defined components auto-register dynamic byte storage on first
    //   attach and validate size, alignment, and layoutHash on later attaches.
    [[nodiscard]] virtual void* AttachRaw(Entity entity, StringView typeName, uint32_t size, uint32_t alignment,
                                          uint64_t layoutHash) noexcept = 0;
    virtual bool                DetachRaw(Entity entity, StringView typeName) noexcept = 0;
    [[nodiscard]] virtual void* GetRaw(Entity entity, StringView typeName) const noexcept = 0;

    // Writes up to `capacity` entities owning `typeName` (published engine
    // component or dynamic consumer component) and returns the total count in
    // storage. Pass nullptr / 0 to query the count first.
    [[nodiscard]] virtual uint32_t EntitiesWithRaw(StringView typeName, Entity* outEntities,
                                                   uint32_t capacity) const noexcept = 0;

    // Compiled in the consumer's TU; not part of the vtable. The audit ignores
    // templates, and these never cross the boundary as symbols.
    template <Component T>
    auto Attach(Entity entity, const T& component) noexcept -> T* {
        void* storage = AttachRaw(entity, ComponentName<T>(), sizeof(T), alignof(T), Reflect::LayoutHash<T>());
        if (storage == nullptr) return nullptr;
        return new (storage) T(component);
    }

    template <Component T>
    [[nodiscard]] auto Get(Entity entity) const noexcept -> T* {
        return static_cast<T*>(GetRaw(entity, ComponentName<T>()));
    }

    template <Component T>
    auto Detach(Entity entity) noexcept -> bool {
        return DetachRaw(entity, ComponentName<T>());
    }

    [[nodiscard]] auto EntitiesWith(StringView typeName) const -> std::vector<Entity> {
        return Collect<&IEntities::EntitiesWithRaw>(*this, typeName);
    }

    template <Component T>
    [[nodiscard]] auto EntitiesWith() const -> std::vector<Entity> {
        return EntitiesWith(ComponentName<T>());
    }

    [[nodiscard]] auto SingletonEntity(StringView typeName) const noexcept -> Option<Entity> {
        Entity first {};
        if (EntitiesWithRaw(typeName, &first, 1) == 0) return std::nullopt;
        return first;
    }

    template <Component T>
    [[nodiscard]] auto SingletonEntity() const noexcept -> Option<Entity> {
        return SingletonEntity(ComponentName<T>());
    }

    // The qualified name, so two components called Transform in different
    // namespaces are different families.
    template <typename T>
    [[nodiscard]] static consteval auto ComponentName() noexcept -> StringView {
        return Reflect::CanonicalTypeName<T>();
    }

    template <Component T>
    [[nodiscard]] auto GetSingleton() const noexcept -> T* {
        const auto entity = SingletonEntity<T>();
        return entity.HasValue() ? Get<T>(entity.value) : nullptr;
    }
    template <Component T, typename Fn>
    auto Patch(Entity entity, Fn&& patch) noexcept -> bool {
        if (auto* value = Get<T>(entity)) { patch(*value); return true; }
        return false;
    }
    template <Component T, typename Fn>
    auto PatchSingleton(Fn&& patch) noexcept -> bool {
        if (auto* value = GetSingleton<T>()) { patch(*value); return true; }
        return false;
    }
    template <Component... T>
    auto AttachMany(Entity entity, const T&... components) noexcept -> bool {
        return ((Attach<T>(entity, components) != nullptr) && ...);
    }
    template <Component... T>
    auto Spawn(const T&... components) noexcept -> Entity {
        const auto entity = CreateEntity();
        if (!((Attach<T>(entity, components) != nullptr) && ...)) { Despawn(entity); return Entity::Null(); }
        return entity;
    }

    template <Component T>
    [[nodiscard]] auto Has(Entity entity) const noexcept -> bool {
        return Get<T>(entity) != nullptr;
    }

protected:
    ~IEntities() = default;
};

} // namespace ZHLN::COM
