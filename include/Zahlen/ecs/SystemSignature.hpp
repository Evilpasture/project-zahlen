// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Reflection/Callable.hpp>
#include <Zahlen/ecs/SystemAccess.hpp>
#include <Zahlen/ecs/SystemParameters.hpp>
#include <algorithm>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ZHLN::ECS {

namespace TemplatedDetail {

template <typename T>
inline constexpr bool IsQuery = false;
template <typename... Comps>
inline constexpr bool IsQuery<Query<Comps...>> = true;

template <typename T>
struct QueryArguments;

template <typename... Comps>
struct QueryArguments<Query<Comps...>> {
    template <typename F>
    static void ForEach(F&& f) { (f.template operator()<Comps>(), ...); }
};

template <typename Comp>
void AppendComponentAccess(std::vector<ComponentAccess>& accesses) {
    using Raw = std::remove_cvref_t<Comp>;
    const auto id   = ComponentFamily::GetTypeID<Raw>();
    const auto mode = std::is_const_v<std::remove_reference_t<Comp>> ? Access::Read : Access::Write;
    auto it = std::ranges::find_if(accesses, [id](const auto& access) { return access.familyId == id; });
    if (it == accesses.end()) {
        accesses.push_back({id, mode});
    } else if (mode == Access::Write) {
        it->mode = Access::Write; // multiple queries of the same family
    }
}

} // namespace TemplatedDetail

// ECS policy over a reusable callable inspector. The signature supplies the
// callable's name/invocation and the Query component hazards; no separate
// hand-maintained access declaration or runtime closure is needed.
template <auto SystemFn>
struct SystemSignature {
    using Inspector = Reflect::CallableInspector<SystemFn>;
    using Callable = typename Inspector::Callable;
    static_assert(!std::is_class_v<Callable> || std::is_empty_v<Callable>,
                  "System functors must be stateless (the graph stores a function pointer)");

    static consteval auto Name() -> std::string_view { return Inspector::Name(); }
    static auto NameCString() -> const char* { return Inspector::NameCString(); }

    static void PopulateAccessPattern(std::vector<ComponentAccess>& accesses) {
        Inspector::ForEachParameter([&]<typename Param>() {
            using Clean = std::remove_cvref_t<Param>;
            if constexpr (TemplatedDetail::IsQuery<Clean>) {
                TemplatedDetail::QueryArguments<Clean>::ForEach([&]<typename Comp>() {
                    TemplatedDetail::AppendComponentAccess<Comp>(accesses);
                });
            } else if constexpr (std::is_same_v<Param, Registry&> || std::is_same_v<Param, const Registry&>) {
                // Direct registry access can touch any component, including
                // families not listed in this system's Query parameters.
                accesses.push_back({AllComponents, Access::Write});
            }
        });
    }

    template <template <typename> class Resolver, typename Context>
    static void Invoke(Context& ctx) { Inspector::template Invoke<Resolver>(ctx); }
};

} // namespace ZHLN::ECS
