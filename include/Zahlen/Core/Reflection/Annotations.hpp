// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Core/Reflection/Annotations.hpp
//
// P3394 attributes: the [[= ZHLN::Description<"..."> {}]] family and every tag
// modelled the same way (SignalSafe, Wire::Range, Wire::Version, Skip...).
// Two shapes only:
//
//   * value queries -- HasAnnotation, TypeHasAnnotation, GetAnnotation,
//     AnnotationCountOf, AnnotationTemplateArgument, GetDescriptionText,
//     AnnotatedName. Value-returning on purpose: such a query folds like any
//     other constant expression, where routing the caller's lambda through
//     std::meta would key a specialization on a local lambda type and leave an
//     undefined reference behind in a module importer.
//   * walks -- ForEachAnnotationType, ForEachAnnotatedType(InScope) -- for the
//     rarer caller that wants the tag type itself handed to a lambda.
//
// Enums.hpp includes this header: an enumerator's annotated message is where
// most of this vocabulary is actually consumed.

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

} // namespace TemplatedDetail

template <typename Tag>
consteval auto AnnotationHasType(std::meta::info annotation) -> bool {
    const auto actualType = std::meta::dealias(std::meta::type_of(annotation));
    // P3394 annotations represent constant values. GCC therefore reports
    // `const Tag`, while the Bloomberg/Clang implementation historically
    // reported `Tag`. Accept both without making either compiler's behavior
    // leak into callers.
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

/// True when a struct, class, or a functor/lambda's operator() carries Tag.
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

/// True when a callable NTTP carries annotation Tag on its type or operator().
///
/// `^^Fn` is not used: Clang requires a named entity, and an `auto` NTTP of
/// class type (`Handler{}`) or function-pointer type (`&foo`) is a value, not
/// a name. Annotations therefore live on the type / operator() and are found
/// through TypeHasAnnotation.
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
            // Materialize into a named local before constructing the
            // optional. Building std::optional<Tag> directly from the
            // extract prvalue (return std::meta::extract<Tag>(a);) fails
            // constant evaluation on some Clang-P2996/libc++ combinations
            // with 'read of object outside its lifetime' inside the
            // optional's inherited-constructor chain. The named local gets
            // the prvalue via guaranteed copy elision and the optional then
            // copies from a live object. This failure is not benign: it
            // makes constexpr EnumToMessage an immediate function, and the
            // runtime call in Error.hpp's category lambda then links as an
            // undefined symbol (observed on macOS/arm64).
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

/// Invokes f.template operator()<AnnotationType>() for every annotation on a
/// reflected entity (data member, type, enumerator...). The const qualifier
/// some implementations add (P3394) is stripped so the callback sees the tag
/// the source spelled.
///
/// The walk uses the same indexed form as GetDescriptionText: a range-for
/// variable over std::meta::annotations_of is not a constant expression on
/// some implementations, so the annotation handle must arrive as a non-type
/// template argument (ExtractDescriptionTextAt) or be spliced out of the
/// define_static_array (ExtractDescriptionText).
namespace TemplatedDetail {
template <std::meta::info Annotation, typename F>
consteval void InvokeAnnotationType(F&& f) {
    constexpr auto type  = std::meta::remove_const(std::meta::dealias(std::meta::type_of(Annotation)));
    using AnnotationType = typename[:type:];
    std::forward<F>(f).template operator()<AnnotationType>();
}
} // namespace TemplatedDetail

template <std::meta::info EntityInfo, typename F>
consteval void ForEachAnnotationType(F&& f) {
    constexpr auto annotations = TemplatedDetail::AnnotationsOf<EntityInfo>();
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        (TemplatedDetail::InvokeAnnotationType<annotations[Is]>(f), ...);
    }(std::make_index_sequence<annotations.size()>());
}

/// Same, for a type: walks the annotations of T. The walk is inlined because
/// the type handle is computed inside this function and cannot be passed as a
/// template argument to the handle overload.
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

} // namespace TemplatedDetail

/// Number of annotations on a reflected entity whose class template is
/// Template (e.g. Wire::Range<Min, Max> counts for Wire::Range).
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

/// Same, for a type.
template <template <auto...> class Template, typename T>
consteval std::size_t AnnotationCountOf() {
    constexpr auto entity = std::meta::dealias(^^std::remove_cvref_t<T>);
    return AnnotationCountOf<Template, entity>();
}

/// ArgumentIndex-th non-type template argument of the first annotation whose
/// class template is Template, converted to Value. Returns Value {} when no
/// such annotation exists.
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

/// Same, for a type.
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

#else // No C++26 static reflection: this module's degraded stand-ins.

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
consteval void ForEachAnnotationType(F&& /*f*/) {
}

template <typename T, typename F>
consteval void ForEachAnnotationType(F&& /*f*/) {
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
constexpr void ForEachAnnotatedTypeInScope(F&& /*unused*/) {
}

template <typename Tag, typename F>
constexpr void ForEachAnnotatedType(F&& /*unused*/) {
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

} // namespace ZHLN::Reflect
