// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// ZHLN::VkD3D frame-graph state analysis -- the compile-time hazard
// simulation. Everything here is consteval graph theory over the neutral
// Usage vocabulary (PassDecl.hpp): resource identity, the write/read masks,
// the previous-state hazard rule, and the state table that falls out of
// simulating the pass list. Both backend emission layers read the same
// table; no backend value appears here.
//
// The execution-side composition machinery (pass merging, fork planning,
// the graph class itself) lives in src/vulkan/graph/RenderGraph.hpp and
// consumes this analysis.

#include <vkd3d/graph/PassDecl.hpp>

#include <Zahlen/Core/Reflection/Enums.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ZHLN::VkD3D::TemplatedDetail {

struct DummyResource {
    static constexpr auto name = ResourceName("DummyResource");
};

template <size_t Capacity>
struct ConstexprString {
    std::array<char, Capacity> dataBuffer {};
    size_t                     length = 0;

    constexpr void append(std::string_view sv) noexcept {
        const size_t to_copy = sv.size() < (Capacity - 1 - length) ? sv.size() : (Capacity - 1 - length);
        if (to_copy > 0) {
            std::char_traits<char>::copy(dataBuffer.data() + length, sv.data(), to_copy);
            length += to_copy;
            dataBuffer[length] = '\0';
        }
    }

    constexpr void append_int(size_t val) noexcept {
        if (val == 0) {
            append("0");
            return;
        }
        std::array<char, 24> temp {};
        size_t               i = 0;
        while (val > 0 && i < 23) {
            temp[i++] = static_cast<char>('0' + (val % 10));
            val /= 10;
        }
        for (size_t j = 0; j < i / 2; ++j) {
            const char c    = temp[j];
            temp[j]         = temp[i - 1 - j];
            temp[i - 1 - j] = c;
        }
        append(std::string_view(temp.data(), i));
    }

    template <typename E>
        requires std::is_enum_v<E>
    constexpr void append_enum(E value) noexcept {
        using Under     = std::underlying_type_t<E>;
        const auto bits = static_cast<Under>(value);
        bool       any  = false;
        for (const auto& entry : Reflect::kEnumEntries<E>) {
            const auto v = static_cast<Under>(entry.value);
            if (v != 0 && (bits & v) == v) {
                if (any) {
                    append(" | ");
                }
                any = true;
                append(entry.name);
            }
        }
        if (!any) {
            append(Reflect::EnumToString(value));
        }
    }

    [[nodiscard]] constexpr auto string_view() const noexcept -> std::string_view {
        return std::string_view(dataBuffer.data(), length);
    }
};

template <typename Target, typename... Ts>
consteval auto GetResourceIndexImpl(TypeList<Ts...> ) -> size_t;

template <typename ResourceList, typename Target>
consteval auto GetResourceIndex() -> size_t;

struct ResourceState {
    ImageLayout    layout            = ImageLayout::Undefined;
    PipelineStage  stage             = PipelineStage::None;
    Access         access            = Access::None;
    size_t         lastWritePass     = 999999;
    bool           fromPreviousFrame = false;
};

constexpr Access WriteMask = Access::ShaderWrite | Access::ColorWrite | Access::DepthWrite |
                             Access::TransferWrite | Access::HostWrite | Access::MemoryWrite;

template <typename U>
struct IsAnyWrite: std::bool_constant<Has(U::access, WriteMask)> {};

template <typename U>
struct IsAnyRead: std::bool_constant<!Has(U::access, WriteMask)> {};

template <typename ListA, typename ListB>
struct HasIntersection: std::false_type {};

template <typename... As, typename... Bs>
struct HasIntersection<TypeList<As...>, TypeList<Bs...>>: std::bool_constant<(IsInList<TypeList<Bs...>, As>::value || ...)> {};

template <ResourceState Prev, typename Usage, size_t PassIndex>
struct NeedsBarrier {
    static constexpr bool is_prev_write = Has(Prev.access, WriteMask);
    static constexpr bool is_curr_write = Has(Usage::access, WriteMask);

    static constexpr bool value = (is_curr_write && (Prev.fromPreviousFrame || (Prev.lastWritePass < PassIndex))) ||
                                  (is_prev_write && !is_curr_write && (Prev.lastWritePass < PassIndex)) || (Prev.layout != Usage::layout) ||
                                  (Prev.layout == ImageLayout::Undefined);
};

template <typename ResourceList, typename... Passes>
consteval auto ComputeStateTable();

} // namespace ZHLN::VkD3D::TemplatedDetail

#include "StateTable.inl"
