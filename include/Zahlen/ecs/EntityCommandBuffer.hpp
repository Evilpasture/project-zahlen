// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Zahlen/Core/Arena.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <cstddef>
#include <new>
#include <utility>
#include <vector>

namespace ZHLN::ECS {

class EntityCommandBuffer {
  public:
    // The optional destroy operation is chosen once by the owning scene.
    // Plain ECS buffers destroy immediately; Engine buffers mark for cleanup.
    using DestroyFn = void (*)(Registry&, Entity);

    // Component payloads are copied into `arenaCapacity` bytes of the buffer's
    // own arena rather than one heap allocation per command: a system that
    // spawns a thousand entities records a thousand components without a
    // thousand allocator calls, and Reset() hands the whole block back at once.
    // The capacity is a hard limit -- an overrun panics in LinearArena rather
    // than falling back to the heap, so a scene that outgrows its buffer says so
    // instead of quietly paying for it every frame.
    static constexpr size_t DefaultArenaCapacity = 256 * 1024;

    explicit EntityCommandBuffer(Registry& reg, DestroyFn destroy = nullptr, size_t arenaCapacity = DefaultArenaCapacity):
        _registry(&reg), _destroy(destroy), _arena(arenaCapacity) {
    }
    ~EntityCommandBuffer() {
        Reset();
    }

    EntityCommandBuffer(const EntityCommandBuffer&)                    = delete;
    auto operator=(const EntityCommandBuffer&) -> EntityCommandBuffer& = delete;
    EntityCommandBuffer(EntityCommandBuffer&&)                         = delete;
    auto operator=(EntityCommandBuffer&&) -> EntityCommandBuffer&      = delete;

    [[nodiscard]] auto CreateEntity() -> Entity {
        Entity e = {.index = _tempIndexCounter++, .generation = 0xFFFFFFFF};
        _commands.push_back({CommandType::Create, e, 0, nullptr, nullptr, nullptr});
        return e;
    }

    template <typename C1, typename... Cs>
    auto CreateEntity(C1&& c1, Cs&&... cs) -> Entity {
        Entity e = CreateEntity();
        AddComponent(e, std::forward<C1>(c1));
        (AddComponent(e, std::forward<Cs>(cs)), ...);
        return e;
    }

    template <typename T1, typename... Ts>
        requires(std::is_default_constructible_v<T1> && (std::is_default_constructible_v<Ts> && ...))
    auto CreateEntity() -> Entity {
        Entity e = CreateEntity();
        AddComponent<T1>(e);
        (AddComponent<Ts>(e), ...);
        return e;
    }

    void DestroyEntity(Entity e) {
        _commands.push_back({CommandType::Destroy, e, 0, nullptr, nullptr, nullptr});
    }

    template <typename T>
    void AddComponent(Entity e, T&& component) {
        using ComponentType = std::decay_t<T>;
        uint32_t familyId   = ComponentFamily::GetTypeID<ComponentType>();

        void* storage = _arena.Allocate(sizeof(ComponentType), alignof(ComponentType));
        ::new (storage) ComponentType(std::forward<T>(component));

        // The destructor runs the component's destructor and nothing else: the
        // storage is the arena's, and one Reset() reclaims every payload at
        // once. A type with a destructor and no way to free it is exactly what
        // the ECB wants -- the lifetime it manages is the buffer's, not each
        // component's.
        auto destructor = [](void* ptr) -> auto {
            static_cast<ComponentType*>(ptr)->~ComponentType();
        };

        auto applyFn = [](Registry& reg, Entity target, void* ptr) -> auto { reg.Add<ComponentType>(target, std::move(*static_cast<ComponentType*>(ptr))); };

        _commands.push_back({CommandType::AddComponent, e, familyId, storage, destructor, applyFn});
    }

    template <typename C1, typename... Cs>
    void AddComponent(Entity e, C1&& c1, Cs&&... cs) {
        AddComponent(e, std::forward<C1>(c1));
        (AddComponent(e, std::forward<Cs>(cs)), ...);
    }

    template <typename T>
        requires std::is_default_constructible_v<T>
    void AddComponent(Entity e) {
        AddComponent(e, T {});
    }

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
    DestroyFn            _destroy  = nullptr;
    std::vector<Command> _commands;
    LinearArena          _arena;
    uint32_t             _tempIndexCounter = 0xF0000000;
};

}
