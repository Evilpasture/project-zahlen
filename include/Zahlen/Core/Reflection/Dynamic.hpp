// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Core/Reflection/Dynamic.hpp
//
// Reflection that produces code instead of reading it: TypeDescriptor,
// AggregateBuilder and Define, all built on std::meta::define_aggregate.
// Nothing here exists at runtime -- the work happens entirely in a constant
// expression -- and this is the only module in the directory whose bodies want
// <vector> (member specs) and <array> (the fixed-array binding).
//
// AnonymousNode and FixedArrayBinding are the declaration helpers the builder
// needs: a nested object's type has to be named before it is defined.

#pragma once

#include <Zahlen/Core/Reflection/Core.hpp>
#include <Zahlen/Core/Reflection/Structs.hpp>
#include <array>
#include <cstddef>
#include <string_view>
#include <vector>

namespace ZHLN::Reflect {

#if ZHLN_REFLECTION_AVAILABLE

namespace TemplatedDetail {

template <size_t ID>
struct AnonymousNode {
    struct type;
};

template <typename T, size_t N>
struct FixedArrayBinding {
    using type = std::array<T, N>;
};

} // namespace TemplatedDetail

class TypeDescriptor {
  public:
    consteval TypeDescriptor() noexcept = default;

    template <typename T>
    static consteval auto Of() noexcept -> TypeDescriptor {
        return TypeDescriptor(^^std::remove_cvref_t<T>);
    }

    static consteval auto String() noexcept -> TypeDescriptor {
        return Of<std::string_view>();
    }

    static consteval auto Int64() noexcept -> TypeDescriptor {
        return Of<int64_t>();
    }

    static consteval auto Float64() noexcept -> TypeDescriptor {
        return Of<double>();
    }

    static consteval auto Boolean() noexcept -> TypeDescriptor {
        return Of<bool>();
    }

    static consteval auto Null() noexcept -> TypeDescriptor {
        return Of<std::nullptr_t>();
    }

    static consteval auto Void() noexcept -> TypeDescriptor {
        return Of<void>();
    }

    static consteval auto ArrayOf(TypeDescriptor elemType, size_t count) noexcept -> TypeDescriptor {
        std::vector<std::meta::info> template_args = {elemType.m_handle, std::meta::reflect_constant(count)};
        return TypeDescriptor(std::meta::substitute(^^std::array, template_args));
    }

    [[nodiscard]] consteval auto Handle() const noexcept {
        return m_handle;
    }

  private:
    explicit consteval TypeDescriptor(std::meta::info handle) noexcept: m_handle(handle) {
    }
    std::meta::info m_handle = {};

    template <typename TargetStruct>
    friend class AggregateBuilder;
};

template <typename TargetStruct>
class AggregateBuilder {
  public:
    consteval AggregateBuilder() noexcept: m_targetInfo(std::meta::dealias(^^TargetStruct)) {
    }

    template <typename T>
    consteval auto AddField(std::string_view name) noexcept -> AggregateBuilder& {
        return AddField(name, TypeDescriptor::Of<T>());
    }

    consteval auto AddField(std::string_view name, TypeDescriptor typeDesc) noexcept -> AggregateBuilder& {
        std::meta::data_member_options opts;
        opts.name = name;
        m_specs.push_back(std::meta::data_member_spec(typeDesc.Handle(), opts));
        return *this;
    }

    template <typename T>
    consteval auto AddArrayField(std::string_view name, size_t count) noexcept -> AggregateBuilder& {
        return AddField(name, TypeDescriptor::ArrayOf(TypeDescriptor::Of<T>(), count));
    }

    template <size_t NodeID, typename ConfigFn>
    consteval auto AddNestedObject(std::string_view name, ConfigFn&& configFn) noexcept -> TypeDescriptor {
        using NestedType = typename TemplatedDetail::AnonymousNode<NodeID>::type;
        AggregateBuilder<NestedType> nestedBuilder;
        configFn(nestedBuilder);
        TypeDescriptor nestedDesc = nestedBuilder.Build();

        AddField(name, nestedDesc);
        return nestedDesc;
    }

    consteval auto Build() noexcept -> TypeDescriptor {
        std::meta::define_aggregate(m_targetInfo, m_specs);
        return TypeDescriptor(m_targetInfo);
    }

  private:
    std::meta::info              m_targetInfo;
    std::vector<std::meta::info> m_specs;
};

template <StringLiteral Name, typename... Fields>
struct Define {
    struct type;

    friend constexpr auto GetSchemaName(type* /*unused*/) -> std::string_view {
        return Name;
    }

    consteval {
        constexpr size_t             NumFields = sizeof...(Fields);
        std::vector<std::meta::info> specs;
        specs.reserve(NumFields);

        auto build_field = [&]<typename F>() -> auto {
            std::meta::data_member_options opts;
            opts.name = static_cast<std::string_view>(F::name);
            specs.push_back(std::meta::data_member_spec(^^typename F::type, opts));
        };

        (build_field.template operator()<Fields>(), ...);
        std::meta::define_aggregate(std::meta::dealias(^^type), specs);
    }
};

#else // No C++26 static reflection: this module's degraded stand-ins.

template <StringLiteral Name, typename... Fields>
struct Define {
    struct type {};
    friend constexpr std::string_view GetSchemaName(type* /*unused*/) {
        return Name;
    }
};

#endif

} // namespace ZHLN::Reflect
