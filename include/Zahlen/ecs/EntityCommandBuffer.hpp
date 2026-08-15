// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Zahlen/Entity.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <new>
#include <utility>
#include <vector>

namespace ZHLN::ECS {

class EntityCommandBuffer {
  public:
    explicit EntityCommandBuffer(Registry& reg): _registry(&reg) {
    }
    ~EntityCommandBuffer() {
        Reset();
    }

    EntityCommandBuffer(const EntityCommandBuffer&)            = delete;
    EntityCommandBuffer& operator=(const EntityCommandBuffer&) = delete;
    EntityCommandBuffer(EntityCommandBuffer&&)                 = delete;
    EntityCommandBuffer& operator=(EntityCommandBuffer&&)      = delete;

    // --- 1. Create Entity (Empty) ---
    [[nodiscard]] Entity CreateEntity() {
        Entity e = {.index = _tempIndexCounter++, .generation = 0xFFFFFFFF};
        _commands.push_back({CommandType::Create, e, 0, nullptr, nullptr, nullptr});
        return e;
    }

    // --- 2. Create Entity with Component Instances (Fold Expression) ---
    template <typename C1, typename... Cs>
    Entity CreateEntity(C1&& c1, Cs&&... cs) {
        Entity e = CreateEntity();
        AddComponent(e, std::forward<C1>(c1));
        (AddComponent(e, std::forward<Cs>(cs)), ...);
        return e;
    }

    // --- 3. Create Entity with Default-Constructed Components by Type (Fold Expression) ---
    template <typename T1, typename... Ts>
        requires(std::is_default_constructible_v<T1> && (std::is_default_constructible_v<Ts> && ...))
    Entity CreateEntity() {
        Entity e = CreateEntity();
        AddComponent<T1>(e);
        (AddComponent<Ts>(e), ...);
        return e;
    }

    void DestroyEntity(Entity e) {
        _commands.push_back({CommandType::Destroy, e, 0, nullptr, nullptr, nullptr});
    }

    // --- 4. Single Component Instance ---
    template <typename T>
    void AddComponent(Entity e, T&& component) {
        using ComponentType = std::decay_t<T>;
        uint32_t familyId   = ComponentFamily::GetTypeID<ComponentType>();

        void* storage = ::operator new(sizeof(ComponentType), std::align_val_t {alignof(ComponentType)});
        ::new (storage) ComponentType(std::forward<T>(component));

        auto destructor = [](void* ptr) {
            static_cast<ComponentType*>(ptr)->~ComponentType();
            ::operator delete(ptr, std::align_val_t {alignof(ComponentType)});
        };

        auto applyFn = [](Registry& reg, Entity target, void* ptr) { reg.Add<ComponentType>(target, std::move(*static_cast<ComponentType*>(ptr))); };

        _commands.push_back({CommandType::AddComponent, e, familyId, storage, destructor, applyFn});
    }

    // --- 5. Multiple Component Instances (Fold Expression) ---
    template <typename C1, typename... Cs>
    void AddComponent(Entity e, C1&& c1, Cs&&... cs) {
        AddComponent(e, std::forward<C1>(c1));
        (AddComponent(e, std::forward<Cs>(cs)), ...);
    }

    // --- 6. Single Default-Constructed Component by Type ---
    template <typename T>
        requires std::is_default_constructible_v<T>
    void AddComponent(Entity e) {
        AddComponent(e, T {});
    }

    // --- 7. Multiple Default-Constructed Components by Type (Fold Expression) ---
    template <typename T1, typename T2, typename... Ts>
        requires(std::is_default_constructible_v<T1> && std::is_default_constructible_v<T2> && (std::is_default_constructible_v<Ts> && ...))
    void AddComponent(Entity e) {
        AddComponent<T1>(e);
        AddComponent<T2>(e);
        (AddComponent<Ts>(e), ...);
    }

    void Playback();
    void Reset() noexcept;

  private:
    enum class CommandType : uint8_t { Create, Destroy, AddComponent };

    struct Command {
        CommandType type;
        Entity      entity;
        uint32_t    familyId;
        void*       componentData;
        void (*destructor)(void*);
        void (*applyFn)(Registry&, Entity, void*);
    };

    Registry*            _registry = nullptr;
    std::vector<Command> _commands;
    uint32_t             _tempIndexCounter = 0xF0000000;
};

} // namespace ZHLN::ECS
