// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Core/Reflection/Core.hpp>
#include <cstddef>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ZHLN::Reflect {

#if ZHLN_REFLECTION_AVAILABLE

// NOTE: Never wrap `std::meta::annotations_of(EntityInfo)` in
// `std::define_static_array(...)` or pass an annotation `std::meta::info` as a
// template argument. `std::define_static_array` delegates to
// `std::meta::reflect_constant_array`, whose `_ZTAX...` template-parameter
// object mangling in GCC 16 encodes only the per-TU annotation ordinal for
// `REFLECT_ANNOTATION` elements -- causing single-annotation entities imported
// from two C++20 module BMIs (e.g. `ZHLN.Wire::WireError` and
// `ZHLN.Network::MessageType`) to alias the same static annotation array.
// Indexing `std::meta::annotations_of(EntityInfo)[Index]` directly inside
// `consteval` helpers keyed on `<EntityInfo, Index>` avoids `reflect_constant_array`
// completely while keeping all splices valid on both Clang (P2996) and GCC 16.

template <typename Tag>
consteval auto AnnotationHasType(std::meta::info annotation) -> bool {
    const auto actualType = std::meta::dealias(std::meta::type_of(annotation));
    return actualType == std::meta::dealias(^^Tag) || actualType == std::meta::dealias(^^std::add_const_t<Tag>);
}

template <typename Tag, std::meta::info EntityInfo>
consteval auto HasAnnotation() -> bool {
    for (auto a: std::meta::annotations_of(EntityInfo)) {
        if (AnnotationHasType<Tag>(a)) {
            return true;
        }
    }
    return false;
}

template <typename Tag, typename T>
consteval auto TypeHasAnnotation() -> bool {
    using CleanT            = std::remove_cvref_t<T>;
    constexpr auto typeInfo = std::meta::dealias(^^CleanT);

    if constexpr (HasAnnotation<Tag, typeInfo>()) {
        return true;
    } else if constexpr (requires { &CleanT::operator(); }) {
        constexpr auto opInfo = std::meta::dealias(^^CleanT::operator());
        return HasAnnotation<Tag, opInfo>();
    }
    return false;
}

template <typename Tag, auto Fn>
consteval auto FunctionHasAnnotation() -> bool {
    return TypeHasAnnotation<Tag, decltype(Fn)>();
}

template <std::meta::info ScopeInfo, typename Tag, typename F>
constexpr void ForEachAnnotatedTypeInScope(F&& f) {
    [:Expand(std::define_static_array(std::meta::members_of(ScopeInfo, std::meta::access_context::current()))):] >> [&]<auto m>() -> auto {
        if constexpr (std::meta::is_type(m)) {
            if constexpr (HasAnnotation<Tag, m>()) {
                using TargetType = typename[:m:];
                f.template operator()<TargetType>();
            }
        }
    };
}

template <typename Tag, typename F>
constexpr void ForEachAnnotatedType(F&& f) {
    ForEachAnnotatedTypeInScope<std::meta::parent_of(^^Tag), Tag>(std::forward<F>(f));
}

template <typename Tag, std::meta::info EntityInfo>
consteval auto GetAnnotation() -> std::optional<Tag> {
    for (auto a: std::meta::annotations_of(EntityInfo)) {
        if (AnnotationHasType<Tag>(a)) {
            const Tag value = std::meta::extract<Tag>(a);
            return value;
        }
    }
    return std::nullopt;
}

template <std::meta::info EntityInfo, std::size_t Index>
consteval auto ExtractDescriptionTextAt() -> std::string_view {
    constexpr auto type = std::meta::remove_const(std::meta::dealias(std::meta::type_of(std::meta::annotations_of(EntityInfo)[Index])));
    if constexpr (std::meta::has_template_arguments(type)) {
        if constexpr (std::meta::template_of(type) == ^^ZHLN::Description) {
            using DescType = typename[:type:];
            return DescType::message;
        }
    }
    return {};
}

template <std::meta::info EntityInfo>
consteval auto GetDescriptionText() -> std::string_view {
    constexpr std::size_t count = std::meta::annotations_of(EntityInfo).size();
    std::string_view      result {};
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        ((result.empty() ? (result = ExtractDescriptionTextAt<EntityInfo, Is>(), 0) : 0), ...);
    }(std::make_index_sequence<count> {});
    return result;
}

namespace TemplatedDetail {
template <std::meta::info EntityInfo, std::size_t Index, typename F>
consteval void InvokeAnnotationTypeAt(F&& f) {
    constexpr auto type  = std::meta::remove_const(std::meta::dealias(std::meta::type_of(std::meta::annotations_of(EntityInfo)[Index])));
    using AnnotationType = typename[:type:];
    std::forward<F>(f).template operator()<AnnotationType>();
}
}

template <std::meta::info EntityInfo, typename F>
consteval void ForEachAnnotationType(F&& f) {
    constexpr std::size_t count = std::meta::annotations_of(EntityInfo).size();
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        (TemplatedDetail::InvokeAnnotationTypeAt<EntityInfo, Is>(f), ...);
    }(std::make_index_sequence<count>());
}

template <typename T, typename F>
consteval void ForEachAnnotationType(F&& f) {
    constexpr auto entity = std::meta::dealias(^^std::remove_cvref_t<T>);
    ForEachAnnotationType<entity>(std::forward<F>(f));
}

namespace TemplatedDetail {

template <template <auto...> class Template>
consteval bool IsAnnotationOfTemplate(std::meta::info annotation) {
    const std::meta::info type = std::meta::remove_const(std::meta::dealias(std::meta::type_of(annotation)));
    if (std::meta::has_template_arguments(type)) {
        return std::meta::template_of(type) == ^^Template;
    }
    return false;
}

template <template <auto...> class Template, std::meta::info EntityInfo>
consteval std::size_t FirstAnnotationIndex() {
    const auto annotations = std::meta::annotations_of(EntityInfo);
    for (std::size_t index = 0; index < annotations.size(); ++index) {
        if (IsAnnotationOfTemplate<Template>(annotations[index])) {
            return index;
        }
    }
    return annotations.size();
}

}

template <template <auto...> class Template, std::meta::info EntityInfo>
consteval std::size_t AnnotationCountOf() {
    std::size_t count = 0;
    for (auto annotation: std::meta::annotations_of(EntityInfo)) {
        if (TemplatedDetail::IsAnnotationOfTemplate<Template>(annotation)) {
            ++count;
        }
    }
    return count;
}

template <template <auto...> class Template, typename T>
consteval std::size_t AnnotationCountOf() {
    constexpr auto entity = std::meta::dealias(^^std::remove_cvref_t<T>);
    return AnnotationCountOf<Template, entity>();
}

template <template <auto...> class Template, std::meta::info EntityInfo, std::size_t ArgumentIndex, typename Value = long double>
consteval Value AnnotationTemplateArgument() {
    constexpr std::size_t count = std::meta::annotations_of(EntityInfo).size();
    constexpr auto        match = TemplatedDetail::FirstAnnotationIndex<Template, EntityInfo>();
    if constexpr (match < count) {
        constexpr auto type = std::meta::remove_const(std::meta::dealias(std::meta::type_of(std::meta::annotations_of(EntityInfo)[match])));
        constexpr auto arg  = std::meta::template_arguments_of(type)[ArgumentIndex];
        return static_cast<Value>([:arg:]);
    }
    return Value {};
}

template <template <auto...> class Template, typename T, std::size_t ArgumentIndex, typename Value = long double>
consteval Value AnnotationTemplateArgument() {
    constexpr auto entity = std::meta::dealias(^^std::remove_cvref_t<T>);
    return AnnotationTemplateArgument<Template, entity, ArgumentIndex, Value>();
}

template <typename T>
consteval auto AnnotatedName() -> std::string_view {
    constexpr auto info = ^^std::remove_cvref_t<T>;
    constexpr auto desc = GetDescriptionText<info>();
    if (!desc.empty()) {
        return desc;
    }
    return TypeName<T>();
}

#endif

}
