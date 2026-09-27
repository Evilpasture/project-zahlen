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

namespace TemplatedDetail {

template <std::meta::info EntityInfo>
consteval auto AnnotationsOf() {
    return std::define_static_array(std::meta::annotations_of(EntityInfo));
}

}

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

template <std::meta::info a>
consteval auto ExtractDescriptionText() -> std::string_view {
    constexpr auto type = std::meta::remove_const(std::meta::dealias(std::meta::type_of(a)));
    if constexpr (std::meta::has_template_arguments(type)) {
        if constexpr (std::meta::template_of(type) == ^^ZHLN::Description) {
            using DescType = typename[:type:];
            return DescType::message;
        }
    }
    return {};
}

template <std::meta::info EntityInfo, std::size_t Index>
consteval auto ExtractDescriptionTextAt() -> std::string_view {
    constexpr auto annotations = TemplatedDetail::AnnotationsOf<EntityInfo>();
    return ExtractDescriptionText<annotations[Index]>();
}

template <std::meta::info EntityInfo>
consteval auto GetDescriptionText() -> std::string_view {
    constexpr std::size_t count = TemplatedDetail::AnnotationsOf<EntityInfo>().size();
    std::string_view      result {};
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        ((result.empty() ? (result = ExtractDescriptionTextAt<EntityInfo, Is>(), 0) : 0), ...);
    }(std::make_index_sequence<count> {});
    return result;
}

namespace TemplatedDetail {
template <std::meta::info Annotation, typename F>
consteval void InvokeAnnotationType(F&& f) {
    constexpr auto type  = std::meta::remove_const(std::meta::dealias(std::meta::type_of(Annotation)));
    using AnnotationType = typename[:type:];
    std::forward<F>(f).template operator()<AnnotationType>();
}
}

template <std::meta::info EntityInfo, typename F>
consteval void ForEachAnnotationType(F&& f) {
    constexpr auto annotations = TemplatedDetail::AnnotationsOf<EntityInfo>();
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        (TemplatedDetail::InvokeAnnotationType<annotations[Is]>(f), ...);
    }(std::make_index_sequence<annotations.size()>());
}

template <typename T, typename F>
consteval void ForEachAnnotationType(F&& f) {
    constexpr auto entity      = std::meta::dealias(^^std::remove_cvref_t<T>);
    constexpr auto annotations = TemplatedDetail::AnnotationsOf<entity>();
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        (TemplatedDetail::InvokeAnnotationType<annotations[Is]>(f), ...);
    }(std::make_index_sequence<annotations.size()>());
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
    constexpr auto annotations = AnnotationsOf<EntityInfo>();
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
    constexpr auto annotations = TemplatedDetail::AnnotationsOf<EntityInfo>();
    constexpr auto match       = TemplatedDetail::FirstAnnotationIndex<Template, EntityInfo>();
    if constexpr (match < annotations.size()) {
        constexpr auto type = std::meta::remove_const(std::meta::dealias(std::meta::type_of(annotations[match])));
        constexpr auto args = std::define_static_array(std::meta::template_arguments_of(type));
        return static_cast<Value>([:args[ArgumentIndex]:]);
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

#else

template <typename Tag, auto EntityInfo>
consteval bool HasAnnotation() {
    return false;
}

template <typename Tag, auto Fn>
consteval auto FunctionHasAnnotation() -> bool {
    return true;
}

template <typename Tag, typename T>
consteval auto TypeHasAnnotation() -> bool {
    return true;
}

template <auto EntityInfo>
consteval std::string_view GetDescriptionText() {
    return {};
}

template <auto EntityInfo, typename F>
consteval void ForEachAnnotationType(F&& ) {
}

template <typename T, typename F>
consteval void ForEachAnnotationType(F&& ) {
}

template <template <auto...> class Template, auto EntityInfo>
consteval std::size_t AnnotationCountOf() {
    return 0;
}

template <template <auto...> class Template, typename T>
consteval std::size_t AnnotationCountOf() {
    return 0;
}

template <template <auto...> class Template, auto EntityInfo, std::size_t ArgumentIndex, typename Value = long double>
consteval Value AnnotationTemplateArgument() {
    return Value {};
}

template <template <auto...> class Template, typename T, std::size_t ArgumentIndex, typename Value = long double>
consteval Value AnnotationTemplateArgument() {
    return Value {};
}

template <auto ScopeInfo, typename Tag, typename F>
constexpr void ForEachAnnotatedTypeInScope(F&& ) {
}

template <typename Tag, typename F>
constexpr void ForEachAnnotatedType(F&& ) {
}

template <typename Tag, typename T>
consteval std::optional<Tag> GetAnnotation() {
    return std::nullopt;
}

template <typename T>
consteval std::string_view AnnotatedName() {
    return TypeName<T>();
}

#endif

}
