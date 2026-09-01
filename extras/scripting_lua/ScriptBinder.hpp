// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Error.hpp>
#include <expected>
#include <functional>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace ZHLN {

enum class ScriptError : uint8_t {
    EntityNotFound = 1,
    TypeNotFound,
    ComponentNotFound,
    PropertyNotFound,
    MethodNotFound,
    TypeMismatch,
    ArityMismatch,
    InvalidEnumString,
    IndexOutOfBounds,
    UnsupportedConversion
};

struct ScriptVal;
struct ScriptArray {
    std::vector<ScriptVal> elements;
};

struct BoxedObject {
    std::string_view typeName;
    Entity           ownerEntity = Entity::Null();
    std::string_view compName;
    std::string_view propName;
    size_t           elementIndex = SIZE_MAX;
    void*            rawPtr       = nullptr;
};

struct OwnedObject {
    std::string_view      typeName;
    std::shared_ptr<void> ptr;
};

using ScriptValVariant = std::variant<std::monostate, double, bool, std::string, BoxedObject, OwnedObject, ScriptArray>;

struct ScriptVal: ScriptValVariant {
    using ScriptValVariant::variant;
};

template <typename T>
auto ToScriptValOwned(T&& val) -> ScriptVal;

// ----------------------------------------------------------------------------
// Conversion Engine
// ----------------------------------------------------------------------------

template <typename T>
auto ToScriptVal(const T& val) -> ScriptVal {
    using Decayed = std::decay_t<T>;
    if constexpr (std::is_same_v<Decayed, bool>) {
        return val;
    } else if constexpr (std::is_arithmetic_v<Decayed>) {
        return static_cast<double>(val);
    } else if constexpr (std::is_constructible_v<std::string, Decayed>) {
        return std::string(val);
    } else if constexpr (std::is_enum_v<Decayed>) {
        return std::string(ZHLN::Reflect::EnumToString(val));
    } else if constexpr (std::ranges::input_range<Decayed>) {
        ScriptArray arr;
        for (const auto& elem: val) {
            arr.elements.push_back(ToScriptValOwned(elem));
        }
        return arr;
    } else if constexpr (std::is_class_v<Decayed>) {
        return BoxedObject {
            .typeName     = ZHLN::Reflect::TypeName<Decayed>(),
            .ownerEntity  = Entity::Null(),
            .compName     = {},
            .propName     = {},
            .elementIndex = SIZE_MAX,
            .rawPtr       = const_cast<void*>(static_cast<const void*>(&val))
        };
    } else {
        return std::monostate {};
    }
}

template <typename T>
auto ToScriptValOwned(T&& val) -> ScriptVal {
    using Decayed = std::decay_t<T>;
    if constexpr (std::is_same_v<Decayed, bool>) {
        return val;
    } else if constexpr (std::is_arithmetic_v<Decayed>) {
        return static_cast<double>(val);
    } else if constexpr (std::is_constructible_v<std::string, Decayed>) {
        return std::string(val);
    } else if constexpr (std::is_enum_v<Decayed>) {
        return std::string(ZHLN::Reflect::EnumToString(val));
    } else if constexpr (std::ranges::input_range<Decayed>) {
        ScriptArray arr;
        for (auto&& elem: val) {
            arr.elements.push_back(ToScriptValOwned(std::forward<decltype(elem)>(elem)));
        }
        return arr;
    } else if constexpr (std::is_class_v<Decayed>) {
        auto sharedPtr = std::make_shared<Decayed>(std::forward<T>(val));
        return OwnedObject {.typeName = ZHLN::Reflect::TypeName<Decayed>(), .ptr = std::static_pointer_cast<void>(sharedPtr)};
    } else {
        return std::monostate {};
    }
}

template <typename T>
auto FromScriptVal(const ScriptVal& sval) -> std::expected<T, Error> {
    using Decayed = std::decay_t<T>;
    if constexpr (std::is_same_v<Decayed, bool>) {
        if (const auto* b = std::get_if<bool>(&sval)) {
            return *b;
        }
        return std::unexpected(ScriptError::TypeMismatch);
    } else if constexpr (std::is_arithmetic_v<Decayed>) {
        if (const auto* d = std::get_if<double>(&sval)) {
            return static_cast<Decayed>(*d);
        }
        return std::unexpected(ScriptError::TypeMismatch);
    } else if constexpr (std::is_same_v<Decayed, std::string>) {
        if (const auto* s = std::get_if<std::string>(&sval)) {
            return *s;
        }
        return std::unexpected(ScriptError::TypeMismatch);
    } else if constexpr (std::is_enum_v<Decayed>) {
        if (const auto* s = std::get_if<std::string>(&sval)) {
            if (auto opt = ZHLN::Reflect::StringToEnum<Decayed>(*s)) {
                return *opt;
            }
            return std::unexpected(ScriptError::InvalidEnumString);
        }
        return std::unexpected(ScriptError::TypeMismatch);
    } else if constexpr (std::ranges::input_range<Decayed> && !std::is_same_v<Decayed, std::string>) {
        if (const auto* arr = std::get_if<ScriptArray>(&sval)) {
            Decayed container;
            using ElementType = typename Decayed::value_type;

            if constexpr (requires { container.reserve(arr->elements.size()); }) {
                container.reserve(arr->elements.size());
            }

            for (const auto& elemVal: arr->elements) {
                auto converted = FromScriptVal<ElementType>(elemVal);
                if (!converted) {
                    return std::unexpected(converted.error());
                }
                if constexpr (requires { container.push_back(std::move(converted.value())); }) {
                    container.push_back(std::move(converted.value()));
                } else if constexpr (requires { container.insert(container.end(), std::move(converted.value())); }) {
                    container.insert(container.end(), std::move(converted.value()));
                }
            }
            return container;
        }
        return std::unexpected(ScriptError::TypeMismatch);
    } else if constexpr (std::is_class_v<Decayed>) {
        if constexpr (std::is_copy_constructible_v<Decayed>) {
            if (const auto* obj = std::get_if<BoxedObject>(&sval)) {
                if (obj->typeName == ZHLN::Reflect::TypeName<Decayed>() && obj->rawPtr != nullptr) {
                    return *static_cast<const Decayed*>(obj->rawPtr);
                }
                return std::unexpected(ScriptError::TypeMismatch);
            }
            if (const auto* obj = std::get_if<OwnedObject>(&sval)) {
                if (obj->typeName == ZHLN::Reflect::TypeName<Decayed>() && obj->ptr != nullptr) {
                    return *static_cast<const Decayed*>(obj->ptr.get());
                }
                return std::unexpected(ScriptError::TypeMismatch);
            }
            return std::unexpected(ScriptError::TypeMismatch);
        } else {
            return std::unexpected(ScriptError::UnsupportedConversion);
        }
    } else if constexpr (std::is_pointer_v<Decayed>) {
        if (std::holds_alternative<std::monostate>(sval)) {
            return static_cast<Decayed>(nullptr);
        }
        if (const auto* obj = std::get_if<BoxedObject>(&sval)) {
            return static_cast<Decayed>(obj->rawPtr);
        }
        if (const auto* obj = std::get_if<OwnedObject>(&sval)) {
            return static_cast<Decayed>(obj->ptr.get());
        }
        return std::unexpected(ScriptError::TypeMismatch);
    } else {
        return std::unexpected(ScriptError::UnsupportedConversion);
    }
}

template <typename T>
struct function_traits;
template <typename R, typename C, typename... Args>
struct function_traits<R (C::*)(Args...)> {
    using return_type             = R;
    using args_tuple              = std::tuple<std::decay_t<Args>...>;
    static constexpr size_t arity = sizeof...(Args);
};
template <typename R, typename C, typename... Args>
struct function_traits<R (C::*)(Args...) const>: function_traits<R (C::*)(Args...)> {};

template <typename R, typename C, typename... Args>
struct function_traits<R (C::*)(Args...) noexcept>: function_traits<R (C::*)(Args...)> {};

template <typename R, typename C, typename... Args>
struct function_traits<R (C::*)(Args...) const noexcept>: function_traits<R (C::*)(Args...)> {};

// ----------------------------------------------------------------------------
// Registry Definitions
// ----------------------------------------------------------------------------

struct ScriptProperty {
    std::string_view                                                                  name;
    std::function<std::expected<ScriptVal, Error>(const void* instance)>              get;
    std::function<std::expected<void, Error>(void* instance, const ScriptVal& value)> set;

    std::function<std::expected<ScriptVal, Error>(const void* instance, size_t index)>              get_element_at = nullptr;
    std::function<std::expected<void, Error>(void* instance, size_t index, const ScriptVal& value)> set_element_at = nullptr;
};

struct ScriptMethod {
    std::string_view                                                                                name;
    size_t                                                                                          arity;
    std::function<std::expected<ScriptVal, Error>(void* instance, std::span<const ScriptVal> args)> invoke;
};

struct ScriptClassInfo {
    std::string_view                                                name;
    size_t                                                          size;
    size_t                                                          alignment;
    std::unordered_map<std::string_view, ScriptProperty>            properties;
    std::unordered_map<std::string_view, std::vector<ScriptMethod>> methods;

    [[nodiscard]] auto InvokeMethod(void* instance, std::string_view methodName, std::span<const ScriptVal> args) const -> std::expected<ScriptVal, Error> {
        auto it = methods.find(methodName);
        if (it == methods.end()) {
            return std::unexpected(ScriptError::MethodNotFound);
        }

        Error lastError = ScriptError::ArityMismatch;
        for (const auto& overload: it->second) {
            if (overload.arity != args.size()) {
                continue;
            }

            auto result = overload.invoke(instance, args);
            if (result) {
                return result.value();
            }
            lastError = result.error();
        }
        return std::unexpected(lastError);
    }
};

class ScriptBinder {
  public:
    static auto Get() -> ScriptBinder& {
        static ScriptBinder instance;
        return instance;
    }

    std::unordered_map<std::string_view, ScriptClassInfo> classes;

    template <typename T>
    void Register() {
        static_assert(!ZHLN::Reflect::HasVirtualBases<T>(), "ScriptBinder: Virtual base classes are not supported due to dynamic pointer offset adjustment.");

        ScriptClassInfo classInfo {.name = ZHLN::Reflect::TypeName<T>(), .size = sizeof(T), .alignment = alignof(T), .properties = {}, .methods = {}};
        PopulateClassInfo<T, T>(classInfo);
        classes[classInfo.name] = std::move(classInfo);
    }

  private:
    template <typename ClassT, typename CurrentT>
    static void PopulateClassInfo(ScriptClassInfo& classInfo) {
        ZHLN::Reflect::ForEachBase<CurrentT>([&]<typename Base>() -> auto { PopulateClassInfo<ClassT, Base>(classInfo); });

        ZHLN::Reflect::ForEachFieldAccessor<CurrentT>([&]<typename FieldT>(std::string_view name, auto const_getter, auto mut_getter, auto setter) -> auto {
            ScriptProperty prop {
                .name = name,
                .get  = [const_getter](const void* inst) -> std::expected<ScriptVal, Error> {
                    const auto* typedInst = static_cast<const CurrentT*>(static_cast<const ClassT*>(inst));
                    return ToScriptVal(const_getter(*typedInst));
                },
                .set = [setter](void* inst, const ScriptVal& val) -> std::expected<void, Error> {
                    auto* typedInst = static_cast<CurrentT*>(static_cast<ClassT*>(inst));
                    if constexpr (std::is_array_v<FieldT> || !std::is_copy_constructible_v<FieldT> || !std::is_copy_assignable_v<FieldT>) {
                        return std::unexpected(ScriptError::UnsupportedConversion);
                    } else {
                        auto converted = FromScriptVal<FieldT>(val);
                        if (!converted) {
                            return std::unexpected(converted.error());
                        }
                        setter(*typedInst, converted.value());
                        return {};
                    }
                },
                .get_element_at = nullptr,
                .set_element_at = nullptr
            };

            using DecayedField = std::decay_t<FieldT>;
            if constexpr (std::ranges::random_access_range<DecayedField> && !std::is_same_v<DecayedField, std::string>) {
                using ElementType = typename DecayedField::value_type;

                prop.get_element_at = [const_getter](const void* inst, size_t index) -> std::expected<ScriptVal, Error> {
                    const auto* typedInst = static_cast<const CurrentT*>(static_cast<const ClassT*>(inst));
                    const auto& container = const_getter(*typedInst);
                    if (index >= std::ranges::size(container)) {
                        return std::unexpected(ScriptError::IndexOutOfBounds);
                    }

                    if constexpr (std::is_same_v<ElementType, bool>) {
                        bool boolVal = container[index];
                        return ToScriptVal(boolVal);
                    } else {
                        return ToScriptVal(container[index]);
                    }
                };

                prop.set_element_at = [mut_getter](void* inst, size_t index, const ScriptVal& val) -> std::expected<void, Error> {
                    auto* typedInst = static_cast<CurrentT*>(static_cast<ClassT*>(inst));
                    auto& container = mut_getter(*typedInst);
                    if (index >= std::ranges::size(container)) {
                        return std::unexpected(ScriptError::IndexOutOfBounds);
                    }
                    auto converted = FromScriptVal<ElementType>(val);
                    if (!converted) {
                        return std::unexpected(converted.error());
                    }
                    container[index] = std::move(converted.value());
                    return {};
                };
            }

            classInfo.properties[name] = std::move(prop);
        });

        ZHLN::Reflect::ForEachMethodPointer<CurrentT>([&](std::string_view name, auto pmf) -> auto {
            using Traits = function_traits<decltype(pmf)>;

            ScriptMethod method {
                .name = name, .arity = Traits::arity, .invoke = [pmf](void* inst, std::span<const ScriptVal> args) -> std::expected<ScriptVal, Error> {
                    if (args.size() != Traits::arity) {
                        return std::unexpected(ScriptError::ArityMismatch);
                    }

                    auto* typedInst = static_cast<CurrentT*>(static_cast<ClassT*>(inst));

                    return [&]<size_t... Is>(std::index_sequence<Is...>) -> std::expected<ScriptVal, Error> {
                        std::tuple<std::expected<std::tuple_element_t<Is, typename Traits::args_tuple>, Error>...> convertedArgs = {
                            FromScriptVal<std::tuple_element_t<Is, typename Traits::args_tuple>>(args[Is])...
                        };

                        std::optional<Error> firstError;

                        (
                            [&]<size_t I>() -> auto {
                                if (!std::get<I>(convertedArgs) && !firstError) {
                                    firstError = std::get<I>(convertedArgs).error();
                                }
                            }.template operator()<Is>(),
                            ...);

                        if (firstError) {
                            return std::unexpected(*firstError);
                        }

                        using RetType = typename Traits::return_type;
                        if constexpr (std::is_same_v<RetType, void>) {
                            (typedInst->*pmf)(std::get<Is>(convertedArgs).value()...);
                            return ScriptVal {std::monostate {}};
                        } else {
                            decltype(auto) ret = (typedInst->*pmf)(std::get<Is>(convertedArgs).value()...);
                            using RawRet       = decltype(ret);

                            if constexpr (std::is_reference_v<RawRet>) {
                                return ToScriptVal(ret);
                            } else {
                                return ToScriptValOwned(std::move(ret));
                            }
                        }
                    }(std::make_index_sequence<Traits::arity> {});
                }
            };
            classInfo.methods[name].push_back(std::move(method));
        });
    }
};

// ----------------------------------------------------------------------------
// Pure C++ Manifest Registration (ZERO Reflection Tokens)
// ----------------------------------------------------------------------------

template <typename Manifest>
void RegisterManifest() {
    ZHLN::Reflect::ForEachNestedType<Manifest>([]<typename T>() -> auto { ScriptBinder::Get().Register<T>(); });
}

} // namespace ZHLN
