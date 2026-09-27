// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Entity.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <ranges>
#include <span>
#include <tuple>
#include <type_traits>
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
    const T* ptr = nullptr;
    [[nodiscard]] const T* operator->() const noexcept { return ptr; }
    [[nodiscard]] const T& operator*() const noexcept { return *ptr; }
    [[nodiscard]] bool Valid() const noexcept { return ptr != nullptr; }
};

template <typename T>
struct ResMut {
    static_assert(!std::is_const_v<T>, "ResMut<T> requires a mutable resource");
    T* ptr = nullptr;
    [[nodiscard]] T* operator->() const noexcept { return ptr; }
    [[nodiscard]] T& operator*() const noexcept { return *ptr; }
    [[nodiscard]] bool Valid() const noexcept { return ptr != nullptr; }
};

// Nullable services (for example, audio in an ECS-only/headless graph).
// Like ResMut, this is a mutable handle; use Res<T> for required read-only data.
template <typename T>
struct OptionRes {
    static_assert(!std::is_const_v<T>, "OptionRes<T> requires an unqualified resource type");
    T* ptr = nullptr;
    [[nodiscard]] T* operator->() const noexcept { return ptr; }
    [[nodiscard]] T& operator*() const noexcept { return *ptr; }
    [[nodiscard]] bool HasValue() const noexcept { return ptr != nullptr; }
    explicit operator bool() const noexcept { return HasValue(); }
};

namespace TemplatedDetail {

template <typename T>
using RawComponent = std::remove_cvref_t<T>;

template <typename... Ts>
struct UniqueComponents: std::true_type {};

template <typename T, typename... Ts>
struct UniqueComponents<T, Ts...>:
    std::bool_constant<(!std::is_same_v<RawComponent<T>, RawComponent<Ts>> && ...) && UniqueComponents<Ts...>::value> {};

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
    [[nodiscard]] auto Get(Entity entity) const noexcept -> Element<T>* {
        return _registry->Get<T>(entity);
    }

    template <typename T>
        requires Declared<T>
    [[nodiscard]] auto GetSingleton() const noexcept -> Element<T>* {
        const auto entities = Entities<T>();
        return entities.empty() ? nullptr : Get<T>(entities.front());
    }

    template <typename T, typename Fn>
        requires Declared<T>
    auto Patch(Entity entity, Fn&& fn) const -> bool {
        if (auto* component = Get<T>(entity)) {
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
            const std::tuple<Element<TemplatedDetail::RawComponent<Comps>>*...> components {
                Get<TemplatedDetail::RawComponent<Comps>>(entity)...
            };
            if (std::apply([](auto*... ptrs) { return (static_cast<bool>(ptrs) && ...); }, components)) {
                std::apply([&](auto*... ptrs) { std::invoke(fn, entity, *ptrs...); }, components);
            }
        }
    }

  private:
    Registry* _registry;
};

} // namespace ECS
} // namespace ZHLN
