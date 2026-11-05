// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// ZHLN::VkD3D -- the frame-graph vocabulary every backend speaks.
//
// What a pass *declares* about the images it touches -- the layout it wants,
// the stages that touch it, the access it performs, where the image must end
// up, and which channels it writes -- is graph theory, not backend knowledge.
// The same declaration drives a VkImageMemoryBarrier2 on one side and a
// D3D12_TEXTURE_BARRIER on the other. It lives here so there is one copy; the
// backends consume it and neither re-declares it.
//
// The stage/access/layout enums are template parameters, not fixed types. A
// usage says "write color while the early and late fragment stages run", and
// which constants express that in a translation unit is that translation
// unit's business -- ZHLN::Vk::PipelineStage for Vulkan sources,
// ZHLN::VkD3D::PipelineStage for neutral ones. Naming one here would make this
// header a Vulkan header, which is the thing this file exists to avoid.
//
// What deliberately does NOT live here: the Tell-Don't-Ask pass contexts. A
// RasterPassContextBase carries a VkCommandBuffer and issues vkCmdSetViewport;
// a DynamicPass opens vkCmdBeginRendering. Those synthesize backend commands,
// so they stay in src/vulkan with the backend that consumes them. This header
// is declaration only -- no command is recorded from anything in it.

#include <Zahlen/Core/FunctionRef.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

namespace ZHLN::VkD3D {

// A resource's name as a compile-time string. The graph compares and prints
// these; it never depends on what they name.
template <size_t N>
struct ResourceName {
    std::array<char, N> value {};

    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays)
    constexpr ResourceName(const char (&str)[N]) {
        for (size_t i = 0; i < N; ++i) {
            value[i] = str[i];
        }
    }

    [[nodiscard]] constexpr std::string_view string_view() const noexcept {
        return std::string_view(value.data(), N > 0 && value[N - 1] == '\0' ? N - 1 : N);
    }
};

template <typename... Ts>
struct TypeList {
    static constexpr size_t size = sizeof...(Ts);

    template <size_t I>
    using type = Ts...[I];
};

template <typename List, typename Target>
struct IsInList;

template <typename... Ts, typename Target>
struct IsInList<TypeList<Ts...>, Target> {
    static constexpr bool value = (std::is_same_v<Ts, Target> || ...);
};

namespace TemplatedDetail {

template <typename T = void>
struct EmptyResourceTag {
    static constexpr auto name          = ResourceName("DummyResource");
    static constexpr bool is_swapchain  = false;
    static constexpr bool is_persistent = false;
};

// The stand-in a graph reaches for when a pass declares nothing for a slot.
// It carries no layout/stage/access because those are the backend's types and
// this header does not name them; a usage tag only ever has to be comparable
// and printable.
template <typename T = void>
struct EmptyUsageTag {
    using Resource                                = EmptyResourceTag<T>;
    static constexpr auto                  name   = ResourceName("DummyResource");
    static constexpr bool                  is_swapchain  = false;
    static constexpr bool                  is_persistent = false;
};

} // namespace TemplatedDetail

template <>
struct TypeList<> {
    static constexpr size_t size = 0;

    template <size_t I>
    using type = TemplatedDetail::EmptyUsageTag<>;
};

// One declared use of one image by one pass.
//
// Layout, Stage and Access are non-type template parameters of deduced type, so
// the enums stay the backend's: a Vulkan pass declares
// Usage<Img, Vk::ImageLayout::General, Vk::PipelineStage::Compute, ...> and the
// graph reads the same fields it always read. ChannelMask defaults to all four
// channels, matching the VK_COLOR_COMPONENT_* all-bits mask, without this
// header including a Vulkan constant.
template <
    typename Image, auto Layout, auto Stage, auto Access, auto FinalLayout = Layout, auto FinalStage = Stage,
    auto FinalAccess = Access, auto ChannelMask = std::uint32_t {0xF}>
struct Usage {
    using Resource                                      = Image;
    static constexpr auto layout                        = Layout;
    static constexpr auto stage                         = Stage;
    static constexpr auto access                        = Access;
    static constexpr auto final_layout                  = FinalLayout;
    static constexpr auto final_stage                   = FinalStage;
    static constexpr auto final_access                  = FinalAccess;
    static constexpr auto channel_mask                  = ChannelMask;
};

// A pass's declaration: a name and the usages it owns. The walk over this list
// is identical for every backend, so it is stated once.
template <ResourceName Name, typename... UsagesList>
struct RenderPass {
    static constexpr auto name = Name;
    using Usages               = TypeList<UsagesList...>;
};

template <typename T>
concept FrameGraphPass = requires {
    { T::name.string_view() } -> std::convertible_to<std::string_view>;
    typename T::Usages;
};

// Fork recording is parameterized on the backend's command type: the executor
// supplied at the call site is whatever records that backend's commands, so a
// neutral pass declaration can describe parallel bodies without naming a
// command buffer.
template <typename Command>
using ForkCall = ZHLN::FunctionRef<void(Command) const>;

template <typename Executor, typename Command>
concept ForkRecorder = requires(Executor& executor, Command cmd, std::span<const ForkCall<Command>> bodies) {
    { executor.ExecuteFork(cmd, bodies) } noexcept;
};

template <typename Command>
struct SequentialFork {
    static constexpr void ExecuteFork(Command cmd, std::span<const ForkCall<Command>> bodies) noexcept {
        for (const ForkCall<Command>& body : bodies) {
            body(cmd);
        }
    }
};

} // namespace ZHLN::VkD3D
