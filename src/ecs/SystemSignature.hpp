// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Reflection/Callable.hpp>
#include <Zahlen/Core/Reflection/TypeKey.hpp>
#include <ecs/SystemAccess.hpp>
#include <ecs/SystemParameters.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ZHLN::ECS {

namespace TemplatedDetail {

template <typename T>
inline constexpr bool IsQuery = false;
template <typename... Comps>
inline constexpr bool IsQuery<Query<Comps...>> = true;

template <typename T>
inline constexpr bool IsExclusiveQuery = false;
template <typename... Comps>
inline constexpr bool IsExclusiveQuery<ExclusiveQuery<Comps...>> = true;

template <typename T>
struct QueryArguments;

template <typename... Comps>
struct QueryArguments<Query<Comps...>> {
    static constexpr std::size_t kCount = sizeof...(Comps);

    template <typename F>
    static constexpr void ForEach(F&& f) {
        (f.template operator()<Comps>(), ...);
    }
};

template <typename... Comps>
struct QueryArguments<ExclusiveQuery<Comps...>> {
    static constexpr std::size_t kCount = sizeof...(Comps);

    template <typename F>
    static constexpr void ForEach(F&& f) {
        (f.template operator()<Comps>(), ...);
    }
};

template <typename Clean>
struct ResourceView {
    static constexpr bool value = false;
};

template <typename T>
struct ResourceView<Res<T>> {
    static constexpr bool value = true;
    using Target                = T;
};

template <typename T>
struct ResourceView<ResMut<T>> {
    static constexpr bool value = true;
    using Target                = T;
};

template <typename Clean>
struct EventParamTraits {
    static constexpr bool value = false;
};

template <typename T>
struct EventParamTraits<EventReader<T>> {
    static constexpr bool   value = true;
    using Event                   = T;
    static constexpr Access mode  = Access::Read;
};

template <typename T>
struct EventParamTraits<EventWriter<T>> {
    static constexpr bool   value = true;
    using Event                   = T;
    static constexpr Access mode  = Access::Write;
};

template <auto SystemFn>
consteval auto CollectAccessesVec() -> std::vector<ResourceAccess> {
    using Inspector = Reflect::CallableInspector<SystemFn>;
    std::vector<ResourceAccess> accesses;

    auto appendAccess = [&](ResourceAccess entry) {
        for (auto& existing: accesses) {
            if (existing.domain == entry.domain && existing.key == entry.key) {
                if (entry.mode == Access::Write) {
                    existing.mode = Access::Write; // strongest access wins
                }
                return;
            }
        }
        accesses.push_back(entry);
    };

    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        (
            [&] {
                using Param = typename Inspector::template ParameterType<Is>;
                using Clean = std::remove_cvref_t<Param>;
                if constexpr (IsExclusiveQuery<Clean>) {
                    // Immediate insertion can invalidate any component view and
                    // serializes against Component / AllComponents accesses.
                    appendAccess(AllComponentsWrite());
                } else if constexpr (IsQuery<Clean>) {
                    QueryArguments<Clean>::ForEach([&]<typename Comp>() {
                        using Raw             = std::remove_cvref_t<Comp>;
                        constexpr Access mode = std::is_const_v<std::remove_reference_t<Comp>> ? Access::Read : Access::Write;
                        appendAccess({AccessDomain::Component, mode, Reflect::TypeKey<Raw>()});
                    });
                } else if constexpr (ResourceView<Clean>::value) {
                    using Target = typename ResourceView<Clean>::Target;
                    if constexpr (CompleteType<Target>) {
                        constexpr Access mode = std::is_same_v<Clean, ResMut<Target>> ? Access::Write : Access::Read;
                        appendAccess({AccessDomain::Service, mode, Reflect::TypeKey<Target>()});
                    }
                } else if constexpr (OptionalResourceParam<Clean>) {
                    // Optional presence does not mean optional synchronization:
                    // if the service exists, a mutable view writes the same state
                    // as ResMut<T>. Track the underlying T, not Optional<T&>.
                    using Value  = typename Clean::value_type;
                    using Target = std::remove_const_t<Value>;
                    if constexpr (CompleteType<Target>) {
                        constexpr Access mode = std::is_const_v<Value> ? Access::Read : Access::Write;
                        appendAccess({AccessDomain::Service, mode, Reflect::TypeKey<Target>()});
                    }
                } else if constexpr (EventParamTraits<Clean>::value) {
                    using Event = typename EventParamTraits<Clean>::Event;
                    if constexpr (CompleteType<Event>) {
                        appendAccess({AccessDomain::Event, EventParamTraits<Clean>::mode, Reflect::TypeKey<Event>()});
                    }
                    appendAccess({AccessDomain::Service, Access::Read, Reflect::TypeKey<EventBus>()});
                } else {
                    static_assert(
                        !std::is_same_v<Clean, Registry>,
                        "System signatures cannot declare Registry; use Query, ExclusiveQuery or EntityCommandBuffer."
                    );
                }
            }(),
            ...);
    }(std::make_index_sequence<Inspector::ParameterCount()> {});

    return accesses;
}

template <auto SystemFn>
consteval auto CollectQueryKeysVec() -> std::vector<uint64_t> {
    using Inspector = Reflect::CallableInspector<SystemFn>;
    std::vector<uint64_t> keys;

    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        (
            [&] {
                using Param = typename Inspector::template ParameterType<Is>;
                using Clean = std::remove_cvref_t<Param>;
                if constexpr (IsQuery<Clean> || IsExclusiveQuery<Clean>) {
                    QueryArguments<Clean>::ForEach([&]<typename Comp>() {
                        keys.push_back(Reflect::TypeKey<std::remove_cvref_t<Comp>>());
                    });
                }
            }(),
            ...);
    }(std::make_index_sequence<Inspector::ParameterCount()> {});

    return keys;
}

template <auto SystemFn, std::size_t ParamIdx>
consteval auto QueryKeyOffsetForParam() noexcept -> std::size_t {
    using Inspector    = Reflect::CallableInspector<SystemFn>;
    std::size_t offset = 0;
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        (
            [&] {
                if constexpr (Is < ParamIdx) {
                    using Clean = std::remove_cvref_t<typename Inspector::template ParameterType<Is>>;
                    if constexpr (IsQuery<Clean> || IsExclusiveQuery<Clean>) {
                        offset += QueryArguments<Clean>::kCount;
                    }
                }
            }(),
            ...);
    }(std::make_index_sequence<Inspector::ParameterCount()> {});
    return offset;
}

template <typename QueryT>
struct CachedQueryBuilder;

template <typename... Comps>
struct CachedQueryBuilder<Query<Comps...>> {
    [[nodiscard]] static auto Build(Registry& reg, SparseSet* const* ptr) noexcept -> Query<Comps...> {
        return [&]<std::size_t... Js>(std::index_sequence<Js...>) noexcept {
            return Query<Comps...>(reg, std::array<SparseSet*, sizeof...(Comps)> {ptr[Js]...});
        }(std::make_index_sequence<sizeof...(Comps)> {});
    }
};

template <typename... Comps>
struct CachedQueryBuilder<ExclusiveQuery<Comps...>> {
    [[nodiscard]] static auto Build(Registry& reg, SparseSet* const* ptr) noexcept -> ExclusiveQuery<Comps...> {
        return [&]<std::size_t... Js>(std::index_sequence<Js...>) noexcept {
            return ExclusiveQuery<Comps...>(reg, std::array<SparseSet*, sizeof...(Comps)> {ptr[Js]...});
        }(std::make_index_sequence<sizeof...(Comps)> {});
    }
};

} // namespace TemplatedDetail

// ECS policy over a reusable callable inspector. The signature supplies the
// callable's name/invocation, compile-time deduplicated ResourceAccess array,
// and flat query TypeKey list for per-node SparseSet caching.
template <auto SystemFn>
struct SystemSignature {
    using Inspector = Reflect::CallableInspector<SystemFn>;
    using Callable  = typename Inspector::Callable;
    static_assert(!std::is_class_v<Callable> || std::is_empty_v<Callable>,
                  "System functors must be stateless (the graph stores a function pointer)");

    static constexpr std::size_t kAccessCount   = TemplatedDetail::CollectAccessesVec<SystemFn>().size();
    static constexpr std::size_t kQueryKeyCount = TemplatedDetail::CollectQueryKeysVec<SystemFn>().size();

    static consteval auto ComputeAccessPattern() -> std::array<ResourceAccess, kAccessCount> {
        const auto vec = TemplatedDetail::CollectAccessesVec<SystemFn>();
        std::array<ResourceAccess, kAccessCount> out {};
        for (std::size_t i = 0; i < vec.size(); ++i) {
            out[i] = vec[i];
        }
        return out;
    }

    static consteval auto ComputeQueryKeys() -> std::array<uint64_t, kQueryKeyCount> {
        const auto vec = TemplatedDetail::CollectQueryKeysVec<SystemFn>();
        std::array<uint64_t, kQueryKeyCount> out {};
        for (std::size_t i = 0; i < vec.size(); ++i) {
            out[i] = vec[i];
        }
        return out;
    }

    static constexpr std::array<ResourceAccess, kAccessCount> kAccessPattern = ComputeAccessPattern();
    static constexpr std::array<uint64_t, kQueryKeyCount>     kQueryKeys     = ComputeQueryKeys();

    static consteval auto Name() -> std::string_view {
        return Inspector::Name();
    }
    static auto NameCString() -> const char* {
        return Inspector::NameCString();
    }

    [[nodiscard]] static constexpr auto AccessPattern() noexcept -> std::span<const ResourceAccess> {
        return kAccessPattern;
    }

    [[nodiscard]] static constexpr auto QueryKeys() noexcept -> std::span<const uint64_t> {
        return kQueryKeys;
    }

    template <template <typename> class Resolver, typename Context>
    static void Invoke(Context& ctx) {
        InvokeWithIndices<Resolver>(ctx, std::make_index_sequence<Inspector::ParameterCount()> {});
    }

  private:
    template <template <typename> class Resolver, std::size_t ParamIdx, typename Context>
    static decltype(auto) ResolveParam(Context& ctx) {
        using Param = typename Inspector::template ParameterType<ParamIdx>;
        if constexpr (TemplatedDetail::IsQuery<Param> || TemplatedDetail::IsExclusiveQuery<Param>) {
            if constexpr (requires { ctx.querySets; }) {
                if (ctx.querySets != nullptr) {
                    constexpr std::size_t base = TemplatedDetail::QueryKeyOffsetForParam<SystemFn, ParamIdx>();
                    return TemplatedDetail::CachedQueryBuilder<Param>::Build(ctx.registry, ctx.querySets + base);
                }
            }
        }
        return Resolver<Param>::Resolve(ctx);
    }

    template <template <typename> class Resolver, typename Context, std::size_t... Is>
    static void InvokeWithIndices(Context& ctx, std::index_sequence<Is...>) {
        auto callable = SystemFn;
        std::invoke(callable, ResolveParam<Resolver, Is>(ctx)...);
    }
};

} // namespace ZHLN::ECS
