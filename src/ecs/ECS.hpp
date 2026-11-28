// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Buffer.h>
#include <Zahlen/Common.h>
#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/Core/Optional.hpp>
#include <Zahlen/Core/Reflection/Class.hpp>
#include <Zahlen/Core/Reflection/LayoutHash.hpp>
#include <Zahlen/Core/Reflection/TypeKey.hpp>
#include <Zahlen/Core/Reflection/Utilities.hpp>
#include <Zahlen/Core/Span.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <Zahlen/Threading/Sync.hpp>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory_resource>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace ZHLN::ECS {

template <typename T>
concept CompleteType = requires { sizeof(T); };

template <typename T>
constexpr auto BoxedName() -> std::string_view {
    if constexpr (requires { ZHLN::Reflect::GetSchemaNameOf(static_cast<T*>(nullptr)); }) {
        return ZHLN::Reflect::GetSchemaNameOf(static_cast<T*>(nullptr));
    } else if constexpr (requires { ZHLN::Reflect::TypeName<T>(); }) {
        return ZHLN::Reflect::TypeName<T>();
    } else {
        static_assert(false, "Unknown type");
    }
}

class SparseSet {
  public:
    using DestructorFn = void (*)(void*) noexcept;
    using RelocateFn   = void (*)(void* dst, void* src) noexcept;
    using RetireFn     = void (*)(void* ctx, Entity entity, void* component) noexcept;

    SparseSet(size_t elementSize, size_t alignment, ZHLN::BufferSync* syncPtr, DestructorFn destructor = nullptr, RelocateFn relocate = nullptr);
    SparseSet(size_t elementSize, size_t alignment, ZHLN::BufferSync* syncPtr, DestructorFn destructor, RelocateFn relocate, std::pmr::memory_resource* memory);
    ~SparseSet();

    SparseSet(const SparseSet&)                    = delete;
    auto operator=(const SparseSet&) -> SparseSet& = delete;

    struct ExtractedBatch {
        Entity*    entities      = nullptr;
        std::byte* data          = nullptr;
        size_t     count         = 0;
        size_t     denseCapacity = 0;
    };

    void               Insert(Entity entity, const void* data);
    // Allocates (or reuses, running destructor on overwrite) an uninitialized
    // dense slot for placement construction by the caller.
    [[nodiscard]] auto ReserveSlot(Entity entity) -> void*;
    // Reserves a slot and zero-initializes its bytes (for dynamic raw storage).
    [[nodiscard]] auto InsertEmpty(Entity entity) -> void*;
    void               Remove(Entity entity);
    // Moves/copies the component out into caller-provided uninitialized storage
    // (size/alignment of this set) and removes it from the set without running
    // the moved-out element's destructor (caller invokes onRetire + destructor).
    [[nodiscard]] auto ExtractAndRemove(Entity entity, void* outBuffer) noexcept -> bool;
    // Transfers ownership of dense entity and component buffers out of the set
    // (clearing the set in place) so the caller can run onRetire + destructors
    // outside shadowLock and deallocate the buffers back to the same resource.
    [[nodiscard]] auto ExtractAll() noexcept -> ExtractedBatch;
    [[nodiscard]] auto Contains(Entity entity) const noexcept -> bool;
    [[nodiscard]] auto Get(Entity entity) const noexcept -> void*;
    void               Clear() noexcept;

    auto GetBufferView(const void* owner, const char* format) const noexcept -> BufferView;
    auto GetEntityView(const void* owner) const noexcept -> BufferView;

    [[nodiscard]] auto Count() const noexcept -> size_t {
        return _count;
    }
    [[nodiscard]] auto GetDenseArray() const noexcept -> Entity* {
        return _dense;
    }
    [[nodiscard]] auto GetDataArray() const noexcept -> void* {
        return _data;
    }

    void SetRetireHook(RetireFn fn, void* ctx) noexcept {
        _onRetire  = fn;
        _retireCtx = ctx;
    }
    [[nodiscard]] auto HasRetireHook() const noexcept -> bool {
        return _onRetire != nullptr;
    }
    void InvokeRetire(Entity entity, void* element) const noexcept {
        if (_onRetire != nullptr) {
            _onRetire(_retireCtx, entity, element);
        }
    }

  private:
    static constexpr uint32_t INVALID_DENSE = 0xFFFFFFFFu;

    uint32_t*  _sparse = nullptr;
    Entity*    _dense  = nullptr;
    std::byte* _data   = nullptr;

    std::pmr::memory_resource* _memory;
    size_t                     _elementSize;
    size_t                     _alignment;
    size_t                     _count          = 0;
    size_t                     _denseCapacity  = 0;
    size_t                     _sparseCapacity = 0;

    ZHLN::BufferSync* _sync;
    DestructorFn      _destructor = nullptr;
    RelocateFn        _relocate   = nullptr;
    RetireFn          _onRetire   = nullptr;
    void*             _retireCtx  = nullptr;

    void ResizeSparse(uint32_t required);
    void ResizeDense();
};

// Optional boundary/reflection policy attached to a typed ComponentSlot via
// Registry::SetPolicy<T>. Keeps Registry itself free of engine resource types.
struct ComponentPolicy {
    bool published = false;
    bool readOnly  = false;
};

struct ComponentOps {
    size_t   size       = 0;
    size_t   alignment  = 1;
    uint64_t layoutHash = 0; // Reflect::LayoutHash<T>() for POD typed; caller-supplied (or 0) for dynamic
    void (*constructDefault)(void*)                                   = nullptr;
    void (*relocate)(void* dst, void* src) noexcept                   = nullptr; // move-construct + destroy src (nullptr = memcpy)
    void (*destroy)(void*) noexcept                                   = nullptr;
    void (*debugDump)(const void*, std::string&)                      = nullptr;
    void (*onRetire)(void* hookCtx, Entity, void* component) noexcept = nullptr;
    void* hookCtx   = nullptr;
    bool  isDynamic = false;
    bool  published = false;
    bool  readOnly  = false;
};

struct ComponentSlot {
    uint64_t         typeKey = 0;
    std::pmr::string name; // owned by Registry's _memory
    SparseSet*       set = nullptr; // pointer-stable across _slots growth
    ComponentOps     ops {};

    explicit ComponentSlot(std::pmr::memory_resource* memory) noexcept: name(memory) {
    }
};

class Registry {
    friend class EntityCommandBuffer;

  public:
    static constexpr uint32_t InvalidSlot = 0xFFFFFFFFu;

    mutable ZHLN::BufferSync sync {};

    // All registry-owned storage (entity tables, component sets, schema slots,
    // lookup tables, and interned names) uses this caller-owned resource. Null
    // selects the standard resource; the resource must outlive the registry.
    Registry();
    explicit Registry(std::pmr::memory_resource* memory);
    ~Registry();

    Registry(const Registry&)                    = delete;
    auto operator=(const Registry&) -> Registry& = delete;
    Registry(Registry&&)                         = delete;
    auto operator=(Registry&&) -> Registry&      = delete;

    auto Create() -> Entity;

    template <typename C1, typename... Cs>
    auto Create(C1&& c1, Cs&&... cs) -> Entity {
        const Entity entity = Create();
        Add(entity, std::forward<C1>(c1));
        (Add(entity, std::forward<Cs>(cs)), ...);
        return entity;
    }

    template <typename T1, typename... Ts>
        requires(std::is_default_constructible_v<T1> && (std::is_default_constructible_v<Ts> && ...))
    auto Create() -> Entity {
        const Entity entity = Create();
        Add(entity, T1 {});
        (Add(entity, Ts {}), ...);
        return entity;
    }

    // Destroys an entity and removes all of its components. Any registered
    // onRetire hooks run after sync.shadowLock is released.
    void               Destroy(Entity entity);
    [[nodiscard]] auto IsAlive(Entity entity) const noexcept -> bool;

    [[nodiscard]] auto AliveQuery() const noexcept -> EntityAliveQuery {
        return {
            .userdata = this,
            .isAlive  = [](const void* userdata, Entity entity) noexcept -> bool {
                return static_cast<const Registry*>(userdata)->IsAlive(entity);
            },
        };
    }

    // Clears all entities and component storage. Any registered onRetire hooks
    // run after sync.shadowLock is released.
    void Clear();

    // Monotonically incremented whenever a live SparseSet is materialized.
    [[nodiscard]] auto SchemaEpoch() const noexcept -> uint64_t {
        return _schemaEpoch.load(std::memory_order::acquire);
    }

    // Lock-free hot lookup of a component's pointer-stable SparseSet.
    // Returns nullptr if no component of this type has ever been attached yet.
    [[nodiscard]] auto FindSparseSet(uint64_t typeKey) const noexcept -> SparseSet*;

    template <typename T>
        requires CompleteType<T>
    [[nodiscard]] auto FindSparseSet() const noexcept -> SparseSet* {
        return FindSparseSet(Reflect::TypeKey<std::remove_cvref_t<T>>());
    }

    auto RegisterComponentDynamic(std::string_view name, size_t size, size_t alignment, uint64_t layoutHash = 0) -> uint32_t;

    template <typename T>
        requires CompleteType<T>
    auto RegisterComponent() -> bool {
        return RegisterComponent<T>(BoxedName<T>());
    }

    template <typename... Components>
    void RegisterComponents() {
        (RegisterComponent<Components>(BoxedName<Components>()), ...);
    }

    template <typename Container>
    void RegisterAllComponentsIn() {
        ZHLN::Reflect::ForEachNestedType<Container>([this]<typename Comp>() -> void { this->RegisterComponent<Comp>(BoxedName<Comp>()); });
    }

    // Registers typed schema metadata and display name lazily (without
    // allocating a SparseSet). Returns false without overwriting if another
    // distinct slot already owns `name`.
    template <typename T>
        requires CompleteType<T>
    auto RegisterComponent(std::string_view name) -> bool {
        using Bare             = std::remove_cvref_t<T>;
        constexpr auto key     = Reflect::TypeKey<Bare>();
        auto debugDumpFn       = +[](const void* data, std::string& out) { out += ZHLN::Reflect::ToDebugString(*static_cast<const Bare*>(data)); };
        const ComponentOps ops = MakeTypedComponentOps<Bare>(debugDumpFn);
        return ZHLN::Lock(sync.shadowLock, [&] -> bool {
            const uint32_t slotIdx = EnsureTypedSlotLocked(key, ops);
            ComponentSlot& slot    = _slots[slotIdx];
            slot.ops.debugDump     = debugDumpFn;
            if (!name.empty()) {
                if (const uint32_t existing = FindSlotIndexByNameLocked(name); existing != InvalidSlot && existing != slotIdx) {
                    return false;
                }
                slot.name.assign(name.data(), name.size());
                return InsertNameTableLocked(slot.name, slotIdx);
            }
            return true;
        });
    }

    // Reserves a typed schema slot lazily (set remains nullptr, published stays
    // false) so external raw operations recognize the native type key and refuse
    // treating it as an auto-registered dynamic blob.
    template <typename T>
        requires CompleteType<T>
    void ReserveTypedSlot() {
        using Bare             = std::remove_cvref_t<T>;
        constexpr auto key     = Reflect::TypeKey<Bare>();
        const ComponentOps ops = MakeTypedComponentOps<Bare>();
        ZHLN::Lock(sync.shadowLock, [&] { (void)EnsureTypedSlotLocked(key, ops); });
    }

    template <typename T>
        requires CompleteType<T>
    void SetPolicy(ComponentPolicy policy) {
        using Bare             = std::remove_cvref_t<T>;
        constexpr auto key     = Reflect::TypeKey<Bare>();
        const ComponentOps ops = MakeTypedComponentOps<Bare>();
        ZHLN::Lock(sync.shadowLock, [&] {
            const uint32_t slotIdx = EnsureTypedSlotLocked(key, ops);
            ComponentSlot& slot    = _slots[slotIdx];
            slot.ops.published     = policy.published;
            slot.ops.readOnly      = policy.readOnly;
        });
    }

    template <typename T>
        requires CompleteType<T>
    void SetRetireHook(SparseSet::RetireFn fn, void* ctx) {
        using Bare             = std::remove_cvref_t<T>;
        constexpr auto key     = Reflect::TypeKey<Bare>();
        const ComponentOps ops = MakeTypedComponentOps<Bare>();
        ZHLN::Lock(sync.shadowLock, [&] {
            const uint32_t slotIdx = EnsureTypedSlotLocked(key, ops);
            ComponentSlot& slot    = _slots[slotIdx];
            slot.ops.onRetire      = fn;
            slot.ops.hookCtx       = ctx;
            if (slot.set != nullptr) {
                slot.set->SetRetireHook(fn, ctx);
            }
        });
    }

    template <typename T>
        requires std::is_default_constructible_v<T>
    auto Add(Entity entity) -> T& {
        return Add(entity, T {});
    }

    template <typename T1, typename T2, typename... Ts>
        requires(std::is_default_constructible_v<T1> && std::is_default_constructible_v<T2> && (std::is_default_constructible_v<Ts> && ...))
    void Add(Entity entity) {
        Add<T1>(entity);
        Add<T2>(entity);
        (Add<Ts>(entity), ...);
    }

    template <typename T>
    auto Add(Entity entity, T&& component) -> T& {
        if (!IsAlive(entity)) {
            ZHLN::Panic("ECS: cannot add a component to a dead entity");
        }
        using DecayedT         = std::decay_t<T>;
        constexpr uint64_t key = Reflect::TypeKey<DecayedT>();
        SparseSet*         set = FindSparseSet(key);
        if (set == nullptr) [[unlikely]] {
            const ComponentOps ops = MakeTypedComponentOps<DecayedT>();
            set = ZHLN::Lock(sync.shadowLock, [&] -> SparseSet* { return EnsureLiveSetLocked(EnsureTypedSlotLocked(key, ops)); });
        }
        return InsertIntoSet<DecayedT>(*set, entity, std::forward<T>(component));
    }

    template <typename T1, typename T2, typename... Ts>
    void Add(Entity entity, T1&& c1, T2&& c2, Ts&&... cs) {
        Add(entity, std::forward<T1>(c1));
        Add(entity, std::forward<T2>(c2));
        (Add(entity, std::forward<Ts>(cs)), ...);
    }

    template <typename... Ts>
    void Add(Entity entity, Ts&&... components) {
        (Add(entity, std::forward<Ts>(components)), ...);
    }

    template <typename T>
        requires CompleteType<T>
    void Remove(Entity entity) {
        RemoveByKey(entity, Reflect::TypeKey<std::remove_cvref_t<T>>());
    }

    template <typename T>
        requires CompleteType<T>
    [[nodiscard]] auto Has(Entity entity) const noexcept -> bool {
        const SparseSet* set = FindSparseSet(Reflect::TypeKey<std::remove_cvref_t<T>>());
        return set != nullptr && set->Contains(entity);
    }

    template <typename T>
        requires CompleteType<T>
    [[nodiscard]] auto Get(Entity entity) noexcept -> ZHLN::Optional<T&> {
        SparseSet* set = FindSparseSet(Reflect::TypeKey<std::remove_cvref_t<T>>());
        if (set == nullptr) {
            return std::nullopt;
        }
        if (auto* ptr = static_cast<T*>(set->Get(entity))) {
            return *ptr;
        }
        return std::nullopt;
    }

    template <typename T>
        requires CompleteType<T>
    [[nodiscard]] auto Get(Entity entity) const noexcept -> ZHLN::Optional<const T&> {
        const SparseSet* set = FindSparseSet(Reflect::TypeKey<std::remove_cvref_t<T>>());
        if (set == nullptr) {
            return std::nullopt;
        }
        if (const auto* ptr = static_cast<const T*>(set->Get(entity))) {
            return *ptr;
        }
        return std::nullopt;
    }

    template <typename T>
        requires CompleteType<T>
    [[nodiscard]] auto GetRawArray() const noexcept -> ZHLN::RestrictSpan<T> {
        SparseSet* set = FindSparseSet(Reflect::TypeKey<std::remove_cvref_t<T>>());
        if (set == nullptr) {
            return ZHLN::RestrictSpan<T>(nullptr, 0);
        }
        return ZHLN::RestrictSpan<T>(static_cast<T*>(set->GetDataArray()), set->Count());
    }

    template <typename T>
        requires CompleteType<T>
    [[nodiscard]] auto GetEntitiesWith() const noexcept -> std::span<const Entity> {
        const SparseSet* set = FindSparseSet(Reflect::TypeKey<std::remove_cvref_t<T>>());
        if (set == nullptr) {
            return {};
        }
        return {set->GetDenseArray(), set->Count()};
    }

    template <typename T>
        requires CompleteType<T>
    [[nodiscard]] auto SingletonEntity() const noexcept -> Entity {
        auto entities = GetEntitiesWith<T>();
        return entities.empty() ? Entity::Null() : entities[0];
    }

    template <typename T>
        requires CompleteType<T>
    [[nodiscard]] auto GetSingleton() const noexcept -> ZHLN::Optional<const T&> {
        auto entities = GetEntitiesWith<T>();
        return entities.empty() ? ZHLN::Optional<const T&> {std::nullopt} : Get<T>(entities[0]);
    }

    template <typename T>
        requires CompleteType<T>
    [[nodiscard]] auto GetSingleton() noexcept -> ZHLN::Optional<T&> {
        auto entities = GetEntitiesWith<T>();
        return entities.empty() ? ZHLN::Optional<T&> {std::nullopt} : Get<T>(entities[0]);
    }

    template <typename T, typename... Args>
        requires CompleteType<T> && std::is_constructible_v<T, Args...>
    auto GetOrEmplaceSingleton(Args&&... args) -> T& {
        if (auto entities = GetEntitiesWith<T>(); !entities.empty()) {
            return *Get<T>(entities[0]);
        }
        Entity e = Create();
        return Add<T>(e, T {std::forward<Args>(args)...});
    }

    template <typename T, typename Pred>
        requires CompleteType<T>
    [[nodiscard]] auto FindWhere(Pred&& pred) noexcept -> ZHLN::Optional<T&> {
        SparseSet* set = FindSparseSet(Reflect::TypeKey<std::remove_cvref_t<T>>());
        if (set == nullptr) {
            return std::nullopt;
        }
        const size_t count = set->Count();
        auto*        data  = static_cast<T*>(set->GetDataArray());
        for (size_t i = 0; i < count; ++i) {
            T& comp = data[i];
            if (pred(comp)) {
                return comp;
            }
        }
        return std::nullopt;
    }

    template <typename T, typename Pred>
        requires CompleteType<T>
    [[nodiscard]] auto FindWhere(Pred&& pred) const noexcept -> ZHLN::Optional<const T&> {
        const SparseSet* set = FindSparseSet(Reflect::TypeKey<std::remove_cvref_t<T>>());
        if (set == nullptr) {
            return std::nullopt;
        }
        const size_t count = set->Count();
        const auto*  data  = static_cast<const T*>(set->GetDataArray());
        for (size_t i = 0; i < count; ++i) {
            const T& comp = data[i];
            if (pred(comp)) {
                return comp;
            }
        }
        return std::nullopt;
    }

    // Native inspection helpers (Inspection.hpp / editor / debug dump).
    [[nodiscard]] auto HasFamily(std::string_view name) const noexcept -> bool;
    [[nodiscard]] auto GetRawByName(Entity entity, std::string_view name) const noexcept -> void*;
    void               DebugDumpEntity(Entity entity, std::string& out) const;

    // Unified external/COM boundary raw operations keyed on canonical typeName
    // (Hash64(typeName)). Published typed components validate size, alignment,
    // and non-zero layoutHash and invoke onRetire before overwrite/removal;
    // unpublished native typed components and short-name aliases are refused;
    // unknown consumer types auto-register dynamic byte storage.
    [[nodiscard]] auto IsPublishedComponent(std::string_view typeName) const noexcept -> bool;
    [[nodiscard]] auto IsReadOnlyComponent(std::string_view typeName) const noexcept -> bool;
    [[nodiscard]] auto AttachRaw(Entity entity, std::string_view typeName, size_t size, size_t alignment, uint64_t layoutHash = 0) -> void*;
    auto               DetachRaw(Entity entity, std::string_view typeName) -> bool;
    [[nodiscard]] auto GetRaw(Entity entity, std::string_view typeName) const noexcept -> void*;
    [[nodiscard]] auto EntitiesWithRaw(std::string_view typeName) const noexcept -> std::span<const Entity>;

    template <typename T, typename Fn>
    auto Patch(Entity e, Fn&& fn) -> bool {
        if (auto c = Get<T>(e)) {
            std::forward<Fn>(fn)(*c);
            return true;
        }
        return false;
    }

    template <typename T, typename Fn>
    auto Patch(Entity e, Fn&& fn) const -> bool {
        if (auto c = Get<T>(e)) {
            std::forward<Fn>(fn)(*c);
            return true;
        }
        return false;
    }

    template <typename... Ts, typename Fn>
        requires(sizeof...(Ts) > 1)
    auto Patch(Entity e, Fn&& fn) -> bool {
        auto comps    = std::make_tuple(Get<Ts>(e)...);
        bool allValid = std::apply([](const auto&... p) -> bool { return (static_cast<bool>(p) && ...); }, comps);
        if (allValid) {
            std::apply([&](auto&... p) -> void { std::forward<Fn>(fn)(*p...); }, comps);
            return true;
        }
        return false;
    }

    template <typename... Ts, typename Fn>
        requires(sizeof...(Ts) > 1)
    auto Patch(Entity e, Fn&& fn) const -> bool {
        auto comps    = std::make_tuple(Get<Ts>(e)...);
        bool allValid = std::apply([](const auto&... p) -> bool { return (static_cast<bool>(p) && ...); }, comps);
        if (allValid) {
            std::apply([&](auto&... p) -> void { std::forward<Fn>(fn)(*p...); }, comps);
            return true;
        }
        return false;
    }

    template <typename T, typename... Args>
    auto Assign(Entity e, Args&&... args) -> bool {
        return Patch<T>(e, [&](auto& c) -> void {
            if constexpr (sizeof...(Args) == 1 && (std::is_assignable_v<T&, Args> && ...)) {
                c = (std::forward<Args>(args), ...);
            } else if constexpr (requires { c = T {std::forward<Args>(args)...}; }) {
                c = T {std::forward<Args>(args)...};
            } else if constexpr (requires { c = T(std::forward<Args>(args)...); }) {
                c = T(std::forward<Args>(args)...);
            }
        });
    }

  private:
    struct KeyEntry {
        uint64_t                key  = 0;
        uint32_t                slot = InvalidSlot;
        std::atomic<SparseSet*> set {nullptr};
    };

    struct KeyTable {
        KeyEntry* entries  = nullptr;
        size_t    capacity = 0;
        size_t    mask     = 0;
        size_t    count    = 0;
    };

    struct SchemaEntry {
        uint64_t key  = 0;
        uint32_t slot = InvalidSlot;
    };

    struct SchemaTable {
        SchemaEntry* entries  = nullptr;
        size_t       capacity = 0;
        size_t       mask     = 0;
        size_t       count    = 0;
    };

    struct NameEntry {
        uint64_t nameHash = 0;
        uint32_t slot     = InvalidSlot;
    };

    struct NameTable {
        NameEntry* entries  = nullptr;
        size_t     capacity = 0;
        size_t     mask     = 0;
        size_t     count    = 0;
    };

    template <typename Bare>
    static constexpr auto MakeTypedComponentOps(void (*debugDump)(const void*, std::string&) = nullptr) noexcept -> ComponentOps {
        ComponentOps ops {};
        ops.size      = sizeof(Bare);
        ops.alignment = alignof(Bare);
        if constexpr (std::is_trivially_copyable_v<Bare> && std::is_standard_layout_v<Bare>) {
            ops.layoutHash = Reflect::LayoutHash<Bare>();
        }
        if constexpr (std::is_default_constructible_v<Bare>) {
            ops.constructDefault = +[](void* p) { ::new (p) Bare(); };
        }
        if constexpr (!std::is_trivially_copyable_v<Bare>) {
            static_assert(std::is_move_constructible_v<Bare>, "Non-trivially-copyable ECS components must be move-constructible");
            ops.relocate = +[](void* dst, void* src) noexcept {
                ::new (dst) Bare(std::move(*static_cast<Bare*>(src)));
                static_cast<Bare*>(src)->~Bare();
            };
        }
        if constexpr (!std::is_trivially_destructible_v<Bare>) {
            ops.destroy = +[](void* p) noexcept { static_cast<Bare*>(p)->~Bare(); };
        }
        ops.debugDump = debugDump;
        ops.isDynamic = false;
        return ops;
    }

    template <typename DecayedT, typename U>
    static auto InsertIntoSet(SparseSet& set, Entity entity, U&& component) -> DecayedT& {
        if (set.HasRetireHook()) [[unlikely]] {
            if (void* existing = set.Get(entity)) {
                set.InvokeRetire(entity, existing);
            }
        }
        if constexpr (std::is_trivially_copyable_v<DecayedT>) {
            set.Insert(entity, &component);
            return *static_cast<DecayedT*>(set.Get(entity));
        } else {
            void* slot = set.ReserveSlot(entity);
            ::new (slot) DecayedT(std::forward<U>(component));
            return *static_cast<DecayedT*>(slot);
        }
    }

    [[nodiscard]] auto CreateSparseSet(size_t elementSize, size_t alignment, SparseSet::DestructorFn destructor = nullptr,
                                       SparseSet::RelocateFn relocate = nullptr) -> SparseSet*;
    void               EnsureEntityCapacity(uint32_t index);

    // Schema helpers; caller holds sync.shadowLock.
    [[nodiscard]] auto FindSlotIndexByKeyLocked(uint64_t typeKey) const noexcept -> uint32_t;
    [[nodiscard]] auto FindSlotIndexByNameLocked(std::string_view name) const noexcept -> uint32_t;
    [[nodiscard]] auto FindSlotIndexForInspectionLocked(std::string_view name) const noexcept -> uint32_t;
    [[nodiscard]] auto EnsureTypedSlotLocked(uint64_t typeKey, const ComponentOps& ops) -> uint32_t;
    [[nodiscard]] auto EnsureLiveSetLocked(uint32_t slotIndex) -> SparseSet*;
    [[nodiscard]] auto CreateDynamicSlotLocked(std::string_view name, uint64_t typeKey, size_t size, size_t alignment, uint64_t layoutHash) -> uint32_t;
    void               InsertSchemaTableLocked(uint64_t typeKey, uint32_t slotIndex);
    void               InsertKeyTableLocked(uint64_t typeKey, uint32_t slotIndex, SparseSet* set);
    auto               InsertNameTableLocked(std::string_view name, uint32_t slotIndex) -> bool;
    void               DestroyKeyTable(KeyTable* table) noexcept;
    void               RemoveByKey(Entity entity, uint64_t typeKey);

    std::pmr::memory_resource* _memory         = nullptr;
    uint32_t*                  _generations    = nullptr;
    uint32_t*                  _freeIndices    = nullptr;
    size_t                     _entityCount    = 0;
    size_t                     _entityCapacity = 0;
    size_t                     _freeCount      = 0;

    std::pmr::vector<ComponentSlot> _slots;
    SchemaTable                     _schemaTable {};
    std::atomic<KeyTable*>          _activeKeyTable {nullptr};
    std::pmr::vector<KeyTable*>     _retiredTables;
    NameTable                       _nameTable {};
    std::atomic<uint64_t>           _schemaEpoch {0};
};

} // namespace ZHLN::ECS
