// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <engine/Frame.hpp>  // FrameDt / FrameAlpha / FrameIndex
#include <Zahlen/Core/Reflection/TypeKey.hpp>
#include <Zahlen/Core/Optional.hpp>
#include <Zahlen/Core/SoA.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/Log.hpp>
#include <ecs/ECS.hpp>
#include <ecs/EventBus.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace ZHLN {

// FrameDt, FrameAlpha and FrameIndex live in Engine/Frame.hpp: they are the
// ambient values Frame itself carries, not ECS machinery. The resolvers below
// only project them out of the carrier.

namespace ECS {

template <typename T>
struct Res {
    using Target               = T;
    const T*               ptr = nullptr;
    [[nodiscard]] const T* operator->() const noexcept {
        return ptr;
    }
    [[nodiscard]] const T& operator*() const noexcept {
        return *ptr;
    }
    [[nodiscard]] bool Valid() const noexcept {
        return ptr != nullptr;
    }
};

template <typename T>
struct ResMut {
    using Target = T;
    static_assert(!std::is_const_v<T>, "ResMut<T> requires a mutable resource");
    T*               ptr = nullptr;
    [[nodiscard]] T* operator->() const noexcept {
        return ptr;
    }
    [[nodiscard]] T& operator*() const noexcept {
        return *ptr;
    }
    [[nodiscard]] bool Valid() const noexcept {
        return ptr != nullptr;
    }
};

// Nullable services (for example, audio in an ECS-only/headless graph).
// Prefer ZHLN::Optional<T&> or ZHLN::Optional<const T&> in system signatures.
template <typename T>
using OptionRes = ZHLN::Optional<T&>;

// Session state owned by the *node*, not by the world and not by the frame:
// the system's own scratch and memo, alive from the moment the node is added to
// the moment it is removed, and reachable by nothing else. Think of a culler's
// derived planes or a solver's working set.
//
// Where the state lives
// ---------------------
// The graph allocates one block per node when the node is added, constructs each
// Local<T> slot in it, hands that block to this node's invocation, and destroys
// the slots when the node is removed or the graph is cleared. Resolution is by
// type, so a signature may name each Local<T> once; naming the same T twice is
// rejected where the system is added, because both would resolve to one slot.
//
// Why not a service
// -----------------
// A service is shared state: putting a system's private scratch in the service
// bundle makes it nameable by every other system in the domain, which is what
// this parameter exists to avoid. And why not a component: components are world
// data -- trivially relocatable, queryable by gameplay, and part of every hazard
// the scheduler derives. A culler's planes are none of those things.
template <typename T>
struct Local {
    using Target = T;
    static_assert(
        !std::is_const_v<T> && !std::is_reference_v<T>,
        "Local<T> owns a T: pass the state type, not a reference, and keep it mutable so the system can advance it"
    );
    static_assert(std::is_object_v<T>, "Local<T> stores a T");

    T* ptr = nullptr;

    [[nodiscard]] T* operator->() const noexcept {
        return ptr;
    }
    [[nodiscard]] T& operator*() const noexcept {
        return *ptr;
    }
    [[nodiscard]] bool Valid() const noexcept {
        return ptr != nullptr;
    }
};

// A per-worker scratch block of parallel streams, resolved from the worker's own
// arena (see ZHLN::WorkerScratchPool). Capacity is part of the type rather than a
// constructor argument because it is a promise the system makes about how much
// of this data one frame can hold: the arena allocation is exactly the byte
// count the layout asks for, and an overrun refuses instead of growing.
//
// The scratch is a view onto arena memory that the graph resets between frames,
// so a system may read what it wrote this frame and nothing from the last one.
template <typename T, size_t Capacity>
class SoAScratch {
  public:
    using Block    = SoABlock<T>;
    using Streams  = typename Block::Streams;
    using ProxyRef = typename Block::ProxyRef;

    explicit SoAScratch(void* memory) noexcept: _block(Block::Bind(memory, Capacity)) {
    }

    [[nodiscard]] auto operator[](size_t index) noexcept -> ProxyRef {
        return _block[index];
    }

    [[nodiscard]] auto Get(size_t index) const noexcept -> T {
        return _block.Get(index);
    }

    void Set(size_t index, const T& value) noexcept {
        _block.Set(index, value);
    }

    // The whole layout at once, for the passes that walk one dense array: a
    // system that only reads positions reads `GetStreams().position` and touches
    // nothing else the elements hold.
    [[nodiscard]] auto GetStreams() noexcept -> Streams& {
        return _block.GetStreams();
    }

    [[nodiscard]] auto GetStreams() const noexcept -> const Streams& {
        return _block.GetStreams();
    }

    // How much of the block was filled. GetStreams() hands out the whole
    // capacity; this is what a consumer iterates.
    [[nodiscard]] auto size() const noexcept -> size_t {
        return _size;
    }

    void set_size(size_t size) noexcept {
        Assert(size <= Capacity, "SoAScratch size exceeds its declared capacity");
        _size = size;
    }

    [[nodiscard]] static constexpr auto capacity() noexcept -> size_t {
        return Capacity;
    }

  private:
    Block  _block;
    size_t _size = 0;
};

namespace TemplatedDetail {

// A system parameter of the form Optional<T&> / OptionRes<T>: a service the
// graph may or may not provide. Whether it is provided is answered while
// compiling (see ParameterResolver), so the absent case is a nullopt the caller
// asked for rather than a runtime null check.
template <typename Param>
concept OptionalResourceParam = requires { typename Param::value_type; } && std::is_object_v<typename Param::value_type> &&
                                std::is_same_v<Param, ZHLN::Optional<typename Param::value_type&>>;

template <typename T>
using RawComponent = std::remove_cvref_t<T>;

// Specialised for Query below; used by Query::As to reuse Select's
// component and access checks without exposing the underlying Registry.
template <typename Target>
struct QueryProjection;

template <typename... Ts>
struct UniqueComponents: std::true_type {};

template <typename T, typename... Ts>
struct UniqueComponents<T, Ts...>: std::bool_constant<(!std::is_same_v<RawComponent<T>, RawComponent<Ts>> && ...) && UniqueComponents<Ts...>::value> {};

// Is this parameter a Local<...>? Answered on the written type, like every other
// parameter classification in the resolver table.
template <typename Param>
inline constexpr bool IsLocalParam = false;
template <typename T>
inline constexpr bool IsLocalParam<Local<T>> = true;

} // namespace TemplatedDetail

// One slot of a node's state: where it is in the block, what lives there, and
// how to bring it to life and take it down. The construct/destroy pair is
// instantiated per T by the same walk that computes the offset, so a slot
// cannot describe one type and construct another.
struct LocalSlotDesc {
    uint64_t typeKey = 0;
    uint32_t offset  = 0;

    void (*construct)(void* slot) = nullptr;
    void (*destroy)(void* slot)   = nullptr;
};

// The layout of one node's state: one slot per Local<T> in the signature, laid
// out in declaration order, with the total size and alignment the graph allocates
// from. Points directly at static constexpr slot metadata produced from the
// signature, with zero per-node metadata heap allocations.
struct LocalLayout {
    std::span<const LocalSlotDesc> slots {};
    size_t                         size  = 0;
    size_t                         align = 1;

    [[nodiscard]] constexpr auto Empty() const noexcept -> bool {
        return slots.empty();
    }
};

// Each argument names a component family and its access mode: const T (or
// const T&) is read-only; T (or T&) is writable. Get/Raw/Entities only accept
// families in this list. ForEach requires all listed families to be present,
// while Get/Entities allow optional cross-entity lookups and separate passes.
// Structural changes should use EntityCommandBuffer& and become visible at
// graph playback. Explicit Registry& remains available for exclusive access;
// the graph conservatively serialises it against other component accesses.
template <typename... Comps>
class Query {
    template <typename...>
    friend class Query;
    template <typename...>
    friend class ExclusiveQuery;

    static_assert(sizeof...(Comps) > 0, "Query needs at least one component");
    static_assert(
        ((!std::is_pointer_v<TemplatedDetail::RawComponent<Comps>> && !std::is_rvalue_reference_v<Comps> &&
          !std::is_volatile_v<std::remove_reference_t<Comps>>) &&
         ...),
        "Query arguments must be component types or lvalue references"
    );
    static_assert(TemplatedDetail::UniqueComponents<Comps...>::value, "Query cannot contain the same component twice");

    template <typename T>
    static constexpr bool Declared = (std::is_same_v<T, TemplatedDetail::RawComponent<Comps>> || ...);

    template <typename T>
    static constexpr bool ReadOnly = ((std::is_same_v<T, TemplatedDetail::RawComponent<Comps>> && std::is_const_v<std::remove_reference_t<Comps>>) || ...);

    template <typename T>
    using Element = std::conditional_t<ReadOnly<T>, const T, T>;

    template <typename T>
    static consteval auto IndexOf() noexcept -> size_t {
        constexpr std::array<bool, sizeof...(Comps)> matches {std::is_same_v<T, TemplatedDetail::RawComponent<Comps>>...};
        for (size_t i = 0; i < matches.size(); ++i) {
            if (matches[i]) {
                return i;
            }
        }
        return matches.size();
    }

  public:
    explicit Query(Registry& registry) noexcept:
        _registry(&registry), _sets {registry.FindSparseSet(Reflect::TypeKey<TemplatedDetail::RawComponent<Comps>>())...} {
    }

    Query(Registry& registry, std::array<SparseSet*, sizeof...(Comps)> sets) noexcept: _registry(&registry), _sets(sets) {
    }

    template <typename T>
        requires Declared<T>
    [[nodiscard]] auto Entities() const noexcept -> std::span<const Entity> {
        const SparseSet* set = _sets[IndexOf<T>()];
        return set != nullptr ? std::span<const Entity>(set->GetDenseArray(), set->Count()) : std::span<const Entity> {};
    }

    // Alias for helpers shared with Registry (which has the same named API).
    template <typename T>
        requires Declared<T>
    [[nodiscard]] auto GetEntitiesWith() const noexcept -> std::span<const Entity> {
        return Entities<T>();
    }

    template <typename T>
        requires Declared<T>
    [[nodiscard]] auto Get(Entity entity) const noexcept -> ZHLN::Optional<Element<T>&> {
        SparseSet* set = _sets[IndexOf<T>()];
        if (set == nullptr) {
            return std::nullopt;
        }
        if (auto* ptr = static_cast<Element<T>*>(set->Get(entity))) {
            return *ptr;
        }
        return std::nullopt;
    }

    template <typename T>
        requires Declared<T>
    [[nodiscard]] auto GetSingleton() const noexcept -> ZHLN::Optional<Element<T>&> {
        const auto entities = Entities<T>();
        return entities.empty() ? ZHLN::Optional<Element<T>&> {std::nullopt} : Get<T>(entities.front());
    }

    template <typename T, typename Fn>
        requires Declared<T>
    auto Patch(Entity entity, Fn&& fn) const -> bool {
        if (auto component = Get<T>(entity)) {
            std::invoke(std::forward<Fn>(fn), *component);
            return true;
        }
        return false;
    }

    // Project a larger query into a smaller view without exposing Registry&.
    // Projection may drop write access but cannot manufacture it. Reuses
    // already-resolved SparseSet pointers with zero hash lookups.
    template <typename... Subset>
        requires(
            sizeof...(Subset) > 0 && ((Declared<TemplatedDetail::RawComponent<Subset>> &&
                                       (!ReadOnly<TemplatedDetail::RawComponent<Subset>> || std::is_const_v<std::remove_reference_t<Subset>>)) &&
                                      ...)
        )
    [[nodiscard]] auto Select() const noexcept -> Query<Subset...> {
        return Query<Subset...>(*_registry, std::array<SparseSet*, sizeof...(Subset)> {_sets[IndexOf<TemplatedDetail::RawComponent<Subset>>()]...});
    }

    // Projection to a narrower query type (Select<...> with a named target).
    // A target can only drop access or families: it cannot add a family or
    // upgrade a read to a write.
    template <typename TargetQuery>
        requires requires(const Query& source) { TemplatedDetail::QueryProjection<TargetQuery>::Project(source); }
    [[nodiscard]] auto Project() const noexcept -> TargetQuery {
        return TemplatedDetail::QueryProjection<TargetQuery>::Project(*this);
    }

    template <typename T>
        requires Declared<T>
    [[nodiscard]] auto Raw() const noexcept -> ZHLN::RestrictSpan<Element<T>> {
        SparseSet* set = _sets[IndexOf<T>()];
        if (set == nullptr) {
            return ZHLN::RestrictSpan<Element<T>>(nullptr, 0);
        }
        return ZHLN::RestrictSpan<Element<T>>(static_cast<Element<T>*>(set->GetDataArray()), set->Count());
    }

    [[nodiscard]] auto IsAlive(Entity entity) const noexcept -> bool {
        return _registry->IsAlive(entity);
    }
    [[nodiscard]] auto AliveQuery() const noexcept -> EntityAliveQuery {
        return _registry->AliveQuery();
    }

    template <typename Fn>
    void ForEach(Fn&& fn) const {
        for (const SparseSet* set: _sets) {
            if (set == nullptr || set->Count() == 0) {
                return;
            }
        }

        if constexpr (sizeof...(Comps) == 1) {
            using Raw0             = std::tuple_element_t<0, std::tuple<TemplatedDetail::RawComponent<Comps>...>>;
            using Elem0            = Element<Raw0>;
            SparseSet*    set      = _sets[0];
            const size_t  count    = set->Count();
            const Entity* entities = set->GetDenseArray();
            auto*         data     = static_cast<Elem0*>(set->GetDataArray());
            for (size_t i = 0; i < count; ++i) {
                std::invoke(fn, entities[i], data[i]);
            }
            return;
        } else {
            size_t bestIdx   = 0;
            size_t bestCount = _sets[0]->Count();
            for (size_t i = 1; i < sizeof...(Comps); ++i) {
                const size_t c = _sets[i]->Count();
                if (c < bestCount) {
                    bestCount = c;
                    bestIdx   = i;
                }
            }
            DispatchForEach<0>(bestIdx, fn);
        }
    }

  protected:
    Registry*                                       _registry;
    mutable std::array<SparseSet*, sizeof...(Comps)> _sets;

  private:
    template <size_t DriveIdx, typename Fn>
    void DispatchForEach(size_t bestIdx, Fn&& fn) const {
        if (bestIdx == DriveIdx) {
            RunWithDriver<DriveIdx>(fn, std::make_index_sequence<sizeof...(Comps)> {});
            return;
        }
        if constexpr (DriveIdx + 1 < sizeof...(Comps)) {
            DispatchForEach<DriveIdx + 1>(bestIdx, fn);
        }
    }

    template <size_t DriveIdx, typename Fn, size_t... Is>
    void RunWithDriver(Fn&& fn, std::index_sequence<Is...>) const {
        using RawTuple  = std::tuple<TemplatedDetail::RawComponent<Comps>...>;
        using DriveRaw  = std::tuple_element_t<DriveIdx, RawTuple>;
        using DriveElem = Element<DriveRaw>;

        SparseSet*    driver   = _sets[DriveIdx];
        const size_t  count    = driver->Count();
        const Entity* entities = driver->GetDenseArray();
        auto*         driveArr = static_cast<DriveElem*>(driver->GetDataArray());

        for (size_t i = 0; i < count; ++i) {
            const Entity                        entity = entities[i];
            std::array<void*, sizeof...(Comps)> ptrs {};
            bool                                allPresent = true;
            (
                [&] {
                    if constexpr (Is == DriveIdx) {
                        ptrs[Is] = const_cast<void*>(static_cast<const void*>(&driveArr[i]));
                    } else if (allPresent) {
                        void* p  = _sets[Is]->Get(entity);
                        ptrs[Is] = p;
                        if (p == nullptr) {
                            allPresent = false;
                        }
                    }
                }(),
                ...);
            if (allPresent) {
                std::invoke(fn, entity, *static_cast<Element<std::tuple_element_t<Is, RawTuple>>*>(ptrs[Is])...);
            }
        }
    }
};

// Transitional view for systems that must make an immediate structural add
// or create a bare entity immediately.
// A graph schedules it with the same AllComponents write hazard as Registry&;
// it is NOT a way to make structural mutations safe in a parallel query. Only
// declared writable families can be added, and existing components are never
// replaced (resource-owning replacements need their explicit owner APIs).
// An insertion can invalidate component views just like Registry::Add.
template <typename... Comps>
class ExclusiveQuery : public Query<Comps...> {
    template <typename T>
    static constexpr bool Writable = ((std::is_same_v<T, TemplatedDetail::RawComponent<Comps>> &&
                                       !std::is_const_v<std::remove_reference_t<Comps>>) || ...);

  public:
    explicit ExclusiveQuery(Registry& registry) noexcept: Query<Comps...>(registry) {
    }

    ExclusiveQuery(Registry& registry, std::array<SparseSet*, sizeof...(Comps)> sets) noexcept: Query<Comps...>(registry, sets) {
    }

    // Creates a live, component-free entity immediately; component attachment
    // remains either AddIfMissing or explicitly deferred through an ECB.
    [[nodiscard]] auto CreateEntity() const -> Entity {
        return this->_registry->Create();
    }

    template <typename T>
        requires(Writable<T> && std::is_same_v<T, std::remove_cvref_t<T>>)
    auto AddIfMissing(Entity entity, T value) const -> T& {
        if (auto existing = this->template Get<T>(entity)) {
            return *existing;
        }
        T&               added = this->_registry->Add(entity, std::move(value));
        constexpr size_t idx   = Query<Comps...>::template IndexOf<T>();
        if (this->_sets[idx] == nullptr) {
            this->_sets[idx] = this->_registry->FindSparseSet(Reflect::TypeKey<T>());
        }
        return added;
    }
};

namespace TemplatedDetail {
template <typename... Components>
struct QueryProjection<Query<Components...>> {
    template <typename Source>
        requires requires(const Source& source) { source.template Select<Components...>(); }
    [[nodiscard]] static auto Project(const Source& source) noexcept -> Query<Components...> {
        return source.template Select<Components...>();
    }
};
} // namespace TemplatedDetail

} // namespace ECS
} // namespace ZHLN
