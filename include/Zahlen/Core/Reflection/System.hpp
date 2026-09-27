// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Reflection/Core.hpp>
#include <Zahlen/ecs/SystemAccess.hpp>
#include <Zahlen/ecs/SystemParameters.hpp>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <source_location>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace ZHLN::Reflect {

namespace TemplatedDetail {

template <typename Comp>
void AppendComponentAccess(std::vector<ECS::ComponentAccess>& accesses) {
    using Raw = std::remove_cvref_t<Comp>;
    const auto id   = ECS::ComponentFamily::GetTypeID<Raw>();
    const auto mode = std::is_const_v<std::remove_reference_t<Comp>> ? ECS::Access::Read : ECS::Access::Write;
    auto it = std::ranges::find_if(accesses, [id](const auto& access) { return access.familyId == id; });
    if (it == accesses.end()) {
        accesses.push_back({id, mode});
    } else if (mode == ECS::Access::Write) {
        it->mode = ECS::Access::Write; // multiple queries of the same family
    }
}

#if !ZHLN_REFLECTION_AVAILABLE
// The normal build requires P2996/P3096. This branch also permits the
// project's explicit ZHLN_ALLOW_REFLECTION_STUBS mode (e.g. clangd and
// isolated tests without a reflection compiler).
template <typename T>
struct QueryArguments;

template <typename... Comps>
struct QueryArguments<ECS::Query<Comps...>> {
    template <typename F>
    static void ForEach(F&& f) {
        (f.template operator()<Comps>(), ...);
    }
};

template <typename T>
inline constexpr bool IsQuery = false;
template <typename... Comps>
inline constexpr bool IsQuery<ECS::Query<Comps...>> = true;

template <typename T>
struct FunctionParameters;

template <typename R, typename... Params>
struct FunctionParameters<R (*)(Params...)> {
    template <typename F>
    static void ForEach(F&& f) { (f.template operator()<Params>(), ...); }

    template <auto Fn, template <typename> class Resolver, typename Context>
    static void Invoke(Context& ctx) {
        auto callable = Fn;
        std::invoke(callable, Resolver<Params>::Resolve(ctx)...);
    }
};

template <typename R, typename... Params>
struct FunctionParameters<R (*)(Params...) noexcept>: FunctionParameters<R (*)(Params...)> {};

template <typename C, typename R, typename... Params>
struct FunctionParameters<R (C::*)(Params...) const>: FunctionParameters<R (*)(Params...)> {};

template <typename C, typename R, typename... Params>
struct FunctionParameters<R (C::*)(Params...)>: FunctionParameters<R (*)(Params...)> {};

template <typename C, typename R, typename... Params>
struct FunctionParameters<R (C::*)(Params...) const noexcept>: FunctionParameters<R (*)(Params...)> {};

template <typename C, typename R, typename... Params>
struct FunctionParameters<R (C::*)(Params...) noexcept>: FunctionParameters<R (*)(Params...)> {};

template <typename T, typename = void>
struct CallableParameters: FunctionParameters<T> {};

template <typename T>
struct CallableParameters<T, std::void_t<decltype(&T::operator())>>: FunctionParameters<decltype(&T::operator())> {};
#endif

} // namespace TemplatedDetail

// The only home for P2996/P3096 system-signature inspection. The graph sees
// just a name, component accesses and a SystemFunc-compatible thunk; it does
// not spell reflection tokens or store a runtime callable/closure.
template <auto SystemFn>
struct SystemInspector {
    using Callable = std::remove_cvref_t<decltype(SystemFn)>;
    static_assert(!std::is_class_v<Callable> || std::is_empty_v<Callable>,
                  "System functors must be stateless (the graph stores a function pointer)");
    static_assert(!std::is_member_function_pointer_v<Callable>,
                  "Register an unbound member function via a static/free function with a ResMut<T> receiver");

#if ZHLN_REFLECTION_AVAILABLE
  private:
    static consteval auto FunctionEntity() -> std::meta::info {
        if constexpr (std::is_pointer_v<Callable> && std::is_function_v<std::remove_pointer_t<Callable>>) {
            static_assert(SystemFn != nullptr, "Cannot register a null system function");
            return std::meta::reflect_function(*SystemFn);
        } else if constexpr (std::is_class_v<Callable> && requires { &Callable::operator(); }) {
            return ^^Callable::operator();
        } else {
            static_assert(std::is_function_v<Callable>, "System must be a free/static function or stateless callable");
            return std::meta::info {};
        }
    }

    static constexpr auto fnEntity = FunctionEntity();

    template <typename F>
    static void ForEachParameter(F&& f) {
        constexpr auto params = std::define_static_array(std::meta::parameters_of(fnEntity));
        [:Expand(params):] >> [&]<auto param>() {
            using Param = typename[:std::meta::type_of(param):];
            f.template operator()<Param>();
        };
    }

  public:
    static consteval auto Name() -> std::string_view {
        constexpr auto parent = std::meta::parent_of(fnEntity);
        if constexpr (std::meta::is_type(parent) && std::meta::has_identifier(parent)) {
            // Class::Update keeps the graph's established "Class" checkpoint.
            return std::meta::identifier_of(parent);
        } else if constexpr (std::is_class_v<Callable>) {
            // Lambdas lack an identifier; use the closure type's display
            // spelling, with its source location appended in NameCString().
            return std::meta::display_string_of(parent);
        } else if constexpr (std::meta::has_identifier(fnEntity)) {
            return std::meta::identifier_of(fnEntity);
        } else {
            return "AnonymousSystem";
        }
    }

    static auto NameCString() -> const char* {
        static const std::string name = [] {
            std::string result {Name()}; // identifier_of returns a view, not necessarily a C string
            if constexpr (std::is_class_v<Callable>) {
                constexpr auto parent = std::meta::parent_of(fnEntity);
                if constexpr (!std::meta::has_identifier(parent)) {
                    // Closure spellings need not be unique; the definition's
                    // location distinguishes two same-signature lambdas.
                    constexpr auto loc = std::meta::source_location_of(parent);
                    result += "@";
                    result += loc.file_name();
                    result += ":" + std::to_string(loc.line()) + ":" + std::to_string(loc.column());
                }
            }
            return result;
        }();
        return name.c_str();
    }

    static void PopulateAccessPattern(std::vector<ECS::ComponentAccess>& accesses) {
        ForEachParameter([&]<typename Param>() {
            using Clean = std::remove_cvref_t<Param>;
            constexpr auto type = std::meta::dealias(^^Clean);
            if constexpr (std::meta::has_template_arguments(type)) {
                if constexpr (std::meta::template_of(type) == ^^ECS::Query) {
                    constexpr auto args = std::define_static_array(std::meta::template_arguments_of(type));
                    [:Expand(args):] >> [&]<auto arg>() {
                        using Comp = typename[:arg:];
                        TemplatedDetail::AppendComponentAccess<Comp>(accesses);
                    };
                }
            }
            if constexpr (std::is_same_v<Param, ECS::Registry&> || std::is_same_v<Param, const ECS::Registry&>) {
                accesses.push_back({ECS::AllComponents, ECS::Access::Write});
            }
        });
    }

    template <template <typename> class Resolver, typename Context>
    static void Invoke(Context& ctx) {
        constexpr auto params = std::define_static_array(std::meta::parameters_of(fnEntity));
        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            auto callable = SystemFn;
            std::invoke(callable, Resolver<typename[:std::meta::type_of(params[Is]):]>::Resolve(ctx)...);
        }(std::make_index_sequence<params.size()>{});
    }

#else
  private:
    using Params = TemplatedDetail::CallableParameters<Callable>;
    static inline char uniqueTag; // fallback identity for same-signature lambdas

  public:
    static consteval auto Name() -> std::string_view {
        // Only for reflection stubs. Native reflection above obtains the
        // actual entity and its canonical identifier instead of parsing text.
        std::string_view pretty = __PRETTY_FUNCTION__;
        auto start = pretty.find("SystemFn = ");
        if (start == std::string_view::npos) return "AnonymousSystem";
        pretty.remove_prefix(start + sizeof("SystemFn = ") - 1);
        auto end = pretty.find_first_of(";]");
        auto name = pretty.substr(0, end);
        if (name.find("<lambda") != std::string_view::npos) return name; // NameCString adds identity
        if (name.starts_with('&')) name.remove_prefix(1);
        if (name.ends_with("()")) name.remove_suffix(2);
        if (name.ends_with("{}")) name.remove_suffix(2);
        auto last = name.rfind("::");
        if (last != std::string_view::npos) {
            auto suffix = name.substr(last + 2);
            if (suffix == "Update" || suffix == "GraphUpdate") {
                name = name.substr(0, last);
                last = name.rfind("::");
                return name.substr(last == std::string_view::npos ? 0 : last + 2);
            }
            return suffix;
        }
        return name;
    }

    static auto NameCString() -> const char* {
        static const std::string name = [] {
            std::string result {Name()};
            if constexpr (std::is_class_v<Callable>) {
                if (Name().find("<lambda") != std::string_view::npos) {
                    // GCC's pretty-function text omits the closure location.
                    // The tag is unique for each NTTP in stub builds.
                    result += "@" + std::to_string(reinterpret_cast<std::uintptr_t>(&uniqueTag));
                }
            }
            return result;
        }();
        return name.c_str();
    }

    static void PopulateAccessPattern(std::vector<ECS::ComponentAccess>& accesses) {
        Params::ForEach([&]<typename Param>() {
            using Clean = std::remove_cvref_t<Param>;
            if constexpr (TemplatedDetail::IsQuery<Clean>) {
                TemplatedDetail::QueryArguments<Clean>::ForEach([&]<typename Comp>() {
                    TemplatedDetail::AppendComponentAccess<Comp>(accesses);
                });
            } else if constexpr (std::is_same_v<Param, ECS::Registry&> || std::is_same_v<Param, const ECS::Registry&>) {
                accesses.push_back({ECS::AllComponents, ECS::Access::Write});
            }
        });
    }

    template <template <typename> class Resolver, typename Context>
    static void Invoke(Context& ctx) { Params::template Invoke<SystemFn, Resolver>(ctx); }
#endif
};

} // namespace ZHLN::Reflect
