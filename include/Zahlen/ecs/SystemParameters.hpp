// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Optional.hpp>
#include <Zahlen/Core/SoA.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <ranges>
#include <span>
#include <tuple>
#include <type_traits>
#include <vector>
#include <utility>

namespace ZHLN {

// Unlike float/uint64_t, these ambient values cannot be confused with one
// another by the system parameter resolver.
struct FrameDt {
    float value;
    constexpr operator float() const noexcept { return value; }
};
struct FrameAlpha {
    float value;
    constexpr operator float() const noexcept { return value; }
};
struct FrameIndex {
    uint64_t value;
    constexpr operator uint64_t() const noexcept { return value; }
};

namespace ECS {

template <typename T>
struct Res {
    using Target = T;
    const T* ptr = nullptr;
    [[nodiscard]] const T* operator->() const noexcept { return ptr; }
    [[nodiscard]] const T& operator*() const noexcept { return *ptr; }
    [[nodiscard]] bool Valid() const noexcept { return ptr != nullptr; }
};

template <typename T>
struct ResMut {
    using Target = T;
    static_assert(!std::is_const_v<T>, "ResMut<T> requires a mutable resource");
    T* ptr = nullptr;
    [[nodiscard]] T* operator->() const noexcept { return ptr; }
    [[nodiscard]] T& operator*() const noexcept { return *ptr; }
    [[nodiscard]] bool Valid() const noexcept { return ptr != nullptr; }
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
    static_assert(!std::is_const_v<T> && !std::is_reference_v<T>,
                  "Local<T> owns a T: pass the state type, not a reference, and keep it mutable so the system can advance it");
    static_assert(std::is_object_v<T>, "Local<T> stores a T");

    T* ptr = nullptr;

    [[nodiscard]] T* operator->() const noexcept { return ptr; }
    [[nodiscard]] T& operator*() const noexcept { return *ptr; }
    [[nodiscard]] bool Valid() const noexcept { return ptr != nullptr; }
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

template <typename... Ts>
struct UniqueComponents: std::true_type {};

template <typename T, typename... Ts>
struct UniqueComponents<T, Ts...>:
    std::bool_constant<(!std::is_same_v<RawComponent<T>, RawComponent<Ts>> && ...) && UniqueComponents<Ts...>::value> {};

// Is this parameter a Local<...>? Answered on the written type, like every other
// parameter classification in the resolver table.
template <typename Param>
inline constexpr bool IsLocalParam = false;
template <typename T>
inline constexpr bool IsLocalParam<Local<T>> = true;

// One slot of a node's state: where it is in the block, what lives there, and
// how to bring it to life and take it down. The construct/destroy pair is
// instantiated per T by the same walk that computes the offset, so a slot
// cannot describe one type and construct another.
struct LocalSlotDesc {
    uint32_t familyId = 0;
    uint32_t offset   = 0;

    void (*construct)(void* slot) = nullptr;
    void (*destroy)(void* slot)   = nullptr;
};

// The layout of one node's state: one slot per Local<T> in the signature, laid
// out in declaration order, with the total size and alignment the graph allocates
// from. Built once per node from the signature; the node's block keeps its copy so
// teardown destroys exactly what was constructed.
struct LocalLayout {
    std::vector<LocalSlotDesc> slots;
    size_t                     size  = 0;
    size_t                     align = 1;

    [[nodiscard]] bool Empty() const noexcept { return slots.empty(); }
};

} // namespace TemplatedDetail

// Each argument names a component family and its access mode: const T (or
// const T&) is read-only; T (or T&) is writable. Get/Raw/Entities only accept
// families in this list. ForEach requires all listed families to be present,
// while Get/Entities allow optional cross-entity lookups and separate passes.
// Structural changes must use an explicit Registry& system parameter, which
// the graph conservatively serialises against all other component accesses.
template <typename... Comps>
class Query {
    static_assert(sizeof...(Comps) > 0, "Query needs at least one component");
    static_assert(((!std::is_pointer_v<TemplatedDetail::RawComponent<Comps>> && !std::is_rvalue_reference_v<Comps> &&
                    !std::is_volatile_v<std::remove_reference_t<Comps>>) && ...),
                  "Query arguments must be component types or lvalue references");
    static_assert(TemplatedDetail::UniqueComponents<Comps...>::value, "Query cannot contain the same component twice");

    template <typename T>
    static constexpr bool Declared = (std::is_same_v<T, TemplatedDetail::RawComponent<Comps>> || ...);

    template <typename T>
    static constexpr bool ReadOnly =
        ((std::is_same_v<T, TemplatedDetail::RawComponent<Comps>> && std::is_const_v<std::remove_reference_t<Comps>>) || ...);

    template <typename T>
    using Element = std::conditional_t<ReadOnly<T>, const T, T>;

  public:
    explicit Query(Registry& registry) noexcept: _registry(&registry) {}

    template <typename T>
        requires Declared<T>
    [[nodiscard]] auto Entities() const noexcept -> std::span<const Entity> {
        return _registry->GetEntitiesWith<T>();
    }

    // Alias for helpers shared with Registry (which has the same named API).
    template <typename T>
        requires Declared<T>
    [[nodiscard]] auto GetEntitiesWith() const noexcept -> std::span<const Entity> { return Entities<T>(); }

    template <typename T>
        requires Declared<T>
    [[nodiscard]] auto Get(Entity entity) const noexcept -> ZHLN::Optional<Element<T>&> {
        return _registry->Get<T>(entity);
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
    // Projection may drop write access but cannot manufacture it.
    template <typename... Subset>
        requires (sizeof...(Subset) > 0 && ((Declared<TemplatedDetail::RawComponent<Subset>> &&
                   (!ReadOnly<TemplatedDetail::RawComponent<Subset>> || std::is_const_v<std::remove_reference_t<Subset>>)) && ...))
    [[nodiscard]] auto Select() const noexcept -> Query<Subset...> { return Query<Subset...>(*_registry); }

    template <typename T>
        requires Declared<T>
    [[nodiscard]] auto Raw() const noexcept -> ZHLN::RestrictSpan<Element<T>> {
        return _registry->GetRawArray<T>();
    }

    [[nodiscard]] auto IsAlive(Entity entity) const noexcept -> bool { return _registry->IsAlive(entity); }
    [[nodiscard]] auto AliveQuery() const noexcept -> EntityAliveQuery { return _registry->AliveQuery(); }

    template <typename Fn>
    void ForEach(Fn&& fn) const {
        // The smallest set bounds the iteration even when a query has many
        // components; a missing set makes the intersection empty.
        const std::array sets {Entities<TemplatedDetail::RawComponent<Comps>>()...};
        const auto& primary = *std::ranges::min_element(sets, {}, &std::span<const Entity>::size);
        for (Entity entity: primary) {
            const std::tuple<ZHLN::Optional<Element<TemplatedDetail::RawComponent<Comps>>&>...> components {
                Get<TemplatedDetail::RawComponent<Comps>>(entity)...
            };
            if (std::apply([](const auto&... opts) { return (static_cast<bool>(opts) && ...); }, components)) {
                std::apply([&](const auto&... opts) { std::invoke(fn, entity, *opts...); }, components);
            }
        }
    }

  private:
    Registry* _registry;
};

} // namespace ECS
} // namespace ZHLN
