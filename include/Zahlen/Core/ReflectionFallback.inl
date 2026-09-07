// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Core/ReflectionFallback.inl
//
// Degraded stand-ins for the P2996 reflection API. Included by
// Zahlen/Core/Reflection.hpp only when the compiler has no static reflection --
// which is not a supported build configuration, and the hard stop below is why.
//
// Several of these feed struct layout. InputStateComponent::keys is a
// std::bitset<Reflect::EnumCount<KeyCode>()>, and EnumCount is 72 with
// reflection and 0 here. A target compiled without the flag therefore lays
// shared structs out differently from every target compiled with it: mouseX at
// offset 4 instead of 16, sizeof 44 instead of 56. That is an ODR violation
// with no diagnostic at any level -- it compiled, linked, ran, and made
// Context::Button read the key bitset as the mouse position (mouse=(0,3e-45),
// 3e-45 being 0x00000002, i.e. bit 65, KeyCode::LButton). The target missing
// from the zahlen_enable_reflection list in the root CMakeLists.txt was the
// whole bug.
//
// clangd is exempt because it has no P2996 either and would otherwise mark the
// entire tree as broken; __CLANGD__ is defined by the language server itself, so
// nothing has to pass it. ZHLN_ALLOW_REFLECTION_STUBS is the explicit opt-out
// for host-only tooling that is deliberately built on a compiler without
// reflection and pins the values it needs (see tests/gui_harness).

namespace ZHLN::Reflect {

#if !defined(__CLANGD__) && !defined(ZHLN_ALLOW_REFLECTION_STUBS)
static_assert(ReflectionAvailable,
              "ZHLN requires C++26 static reflection (-freflection); "
              "give the CMake target zahlen_enable_reflection(<target>)");
#endif


template <std::ranges::range R>
consteval int Expand(R&& /*unused*/) {
    return 0;
}

template <typename T, typename F>
constexpr void ForEachField(T&& /*unused*/, F&& /*unused*/) {
}

template <typename T, typename F>
constexpr void ForEachFieldWithName(T&& /*unused*/, F&& /*unused*/) {
}

template <typename T, typename F>
constexpr void ForEachDataMember(F&& /*unused*/) {
}

template <typename T, typename F>
constexpr void ForEachMemberFunction(F&& /*unused*/) {
}

template <typename T, typename F>
constexpr void ForEachFieldInfo(F&& /*unused*/) {
}

template <typename T>
constexpr auto TieFields(T&& /*unused*/) {
    return std::tuple {};
}

template <typename E>
    requires std::is_enum_v<E>
constexpr std::string_view EnumToString(E /*unused*/) {
    return "Unknown";
}

template <typename E>
    requires std::is_enum_v<E>
consteval auto EnumHasValue(std::underlying_type_t<E> /*targetValue*/) noexcept -> bool {
    return false; // Safe fallback when compiler reflection is disabled
}

template <typename E>
    requires std::is_enum_v<E>
constexpr std::optional<E> StringToEnum(std::string_view /*unused*/) {
    return std::nullopt;
}

template <typename T>
constexpr auto ZipFieldsWithNames(T&& /*unused*/) {
    return std::tuple {};
}

template <typename T>
constexpr std::size_t FieldCount() {
    return 0;
}

template <std::size_t N, typename T>
constexpr decltype(auto) GetField(T&& /*unused*/) {
    struct Dummy {};
    static Dummy d;
    return d;
}

template <typename T, typename F>
constexpr bool VisitFieldByName(T&& /*unused*/, std::string_view /*unused*/, F&& /*unused*/) {
    return false;
}

template <typename T>
consteval auto FieldNames() {
    return std::array<std::string_view, 0> {};
}

template <typename T>
consteval bool HasField(std::string_view /*unused*/) {
    return false;
}

template <typename E>
    requires std::is_enum_v<E>
consteval std::size_t EnumCount() {
    return 0;
}

template <typename E>
    requires std::is_enum_v<E>
consteval auto EnumNames() {
    return std::array<std::string_view, 0> {};
}

template <typename T, typename F>
constexpr void ForEachFieldIndexed(T&& /*unused*/, F&& /*unused*/) {
}

template <typename Tag, typename T>
consteval bool HasTag(std::string_view /*unused*/) {
    return false;
}

template <std::size_t N, typename T>
using FieldType = void;

template <typename Tag, auto EntityInfo>
consteval bool HasAnnotation() {
    return false;
}

template <auto EntityInfo>
consteval std::string_view GetDescriptionText() {
    return {};
}

template <auto MemberInfo>
consteval std::string_view MemberName() {
    return {};
}

template <auto MemberInfo>
using MemberType = void;

template <auto MemberInfo, typename T>
constexpr decltype(auto) MemberValue(T&& /*object*/) {
    struct Dummy {};
    static Dummy d;
    return d;
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

template <typename T>
consteval auto BaseClasses() {
    return std::array<int, 0> {};
}

template <typename T>
consteval bool HasVirtualBases() {
    return false;
}

template <StringLiteral NameConst, typename T>
constexpr decltype(auto) GetFieldByName(T&& /*unused*/) {
    struct Dummy {};
    static Dummy d;
    return d;
}

namespace detail {

template <typename T>
consteval auto ExtractTypeName() noexcept -> std::string_view {
#if defined(__clang__)
    std::string_view p     = __PRETTY_FUNCTION__;
    auto             start = p.find("[T = ");
    if (start != std::string_view::npos) {
        start += 5;
        auto end = p.find(']', start);
        if (end != std::string_view::npos) {
            std::string_view raw = p.substr(start, end - start);
            for (std::string_view prefix: {"enum class ", "enum ", "struct ", "class "}) {
                if (raw.starts_with(prefix)) {
                    raw.remove_prefix(prefix.size());
                    break;
                }
            }
            return raw;
        }
    }
#elif defined(__GNUC__)
    std::string_view p     = __PRETTY_FUNCTION__;
    auto             start = p.find("[with T = ");
    if (start != std::string_view::npos) {
        start += 10;
        auto end = p.find(';', start);
        if (end == std::string_view::npos)
            end = p.find(']', start);
        if (end != std::string_view::npos) {
            std::string_view raw = p.substr(start, end - start);
            for (std::string_view prefix: {"enum class ", "enum ", "struct ", "class "}) {
                if (raw.starts_with(prefix)) {
                    raw.remove_prefix(prefix.size());
                    break;
                }
            }
            return raw;
        }
    }
#elif defined(_MSC_VER)
    std::string_view p     = __FUNCSIG__;
    auto             start = p.find("ExtractTypeName<");
    if (start != std::string_view::npos) {
        start += 16;
        auto end = p.rfind(">(void)");
        if (end != std::string_view::npos && end > start) {
            std::string_view raw = p.substr(start, end - start);
            for (std::string_view prefix: {"enum class ", "enum ", "struct ", "class "}) {
                if (raw.starts_with(prefix)) {
                    raw.remove_prefix(prefix.size());
                    break;
                }
            }
            return raw;
        }
    }
#endif
    return "";
}

} // namespace detail

template <typename T>
consteval std::string_view TypeName() {
    return detail::ExtractTypeName<std::remove_cvref_t<T>>();
}

/// TypeName with an optional rename predicate (fallback build). The compiler
/// has no reflection, so the predicate receives an empty spelling and may
/// still supply a name; returning nullptr yields the same empty spelling as
/// the no-argument form above.
template <typename T, typename NameOverride>
consteval auto TypeName(NameOverride rename) -> std::string_view {
    const std::string_view spelling   = TypeName<T>();
    const char*            overridden = rename(spelling);
    if (overridden != nullptr) {
        return overridden;
    }
    return spelling;
}

template <typename T, typename F>
constexpr void ForEachBase(F&& /*unused*/) {
}

template <typename E>
    requires std::is_enum_v<E>
constexpr std::string_view EnumToFlagsString(E /*unused*/, std::string& out_buffer) {
    out_buffer.clear();
    return "";
}

template <StringLiteral NameConst, typename T>
consteval std::size_t IndexOfField() {
    return static_cast<std::size_t>(-1);
}

template <typename T>
consteval std::size_t MemberFunctionCount() {
    return 0;
}

template <typename T>
consteval auto MemberFunctionNames() {
    return std::array<std::string_view, 0> {};
}

template <StringLiteral NameConst, typename T, typename ValueType>
constexpr bool SetFieldByName(T& /*unused*/, ValueType&& /*unused*/) {
    return false;
}

template <typename T, typename Tuple>
constexpr T MakeFromTuple(Tuple&& /*unused*/) {
    return T {};
}

template <typename E>
    requires std::is_enum_v<E>
consteval std::string_view EnumUnderlyingTypeName() {
    return "";
}

template <typename T, typename F>
constexpr void ForEachFieldAdaptive(T&& /*unused*/, F&& /*unused*/) {
}

template <typename Tag, typename T>
consteval bool ValidateSerializability() {
    return true;
}

template <typename T, typename F>
constexpr void ForEachNestedType(F&& /*unused*/) {
}

template <StringLiteral Name, typename... Fields>
struct Define {
    struct type {};
    friend constexpr std::string_view GetSchemaName(type* /*unused*/) {
        return Name;
    }
};

template <typename Meta, typename T, typename F>
constexpr void ForEachReflectedField(T&& /*unused*/, F&& /*unused*/) {
}

template <typename E, typename F>
    requires std::is_enum_v<E>
constexpr void ForEachEnumerator(F&& /*unused*/) {
}

template <typename T, typename F>
constexpr void ForEachFieldAccessor(F&& /*unused*/) {
}

template <typename T, typename F>
constexpr void ForEachMethodPointer(F&& /*unused*/) {
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

template <typename Tag, typename E>
    requires std::is_enum_v<E>
constexpr std::optional<Tag> GetEnumeratorAnnotation(E /*unused*/) {
    return std::nullopt;
}

template <typename E>
    requires std::is_enum_v<E>
constexpr std::string_view EnumMessageOf(E /*unused*/) {
    return {};
}

template <typename T>
consteval std::size_t GetFloatFieldsCount() {
    return 0;
}

template <typename T>
constexpr auto CollectMethodResults(const T& /*inst*/) {
    return std::tuple {};
}

} // namespace ZHLN::Reflect
