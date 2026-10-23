// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "graph/DynamicRendering.hpp"
#include "pipeline/DescriptorHeap.hpp"
#include "pipeline/HeapBindings.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace ZHLN::Vk {

/// The frame's push-constant rows, owned by value.
///
/// A secondary command buffer inherits neither the heap bindings nor the push
/// range from the primary it is executed in, so every secondary begins with the
/// per-frame device addresses pushed into it. Those addresses are produced per
/// frame (they are buffer addresses), which means they are a *temporary*: a span
/// over them would dangle the moment the frame context returned. This type
/// copies them into its own array, at a capacity the push layout itself
/// declares, so a context can outlive the call that made it.
class FramePushState {
  public:
    constexpr FramePushState() noexcept = default;

    constexpr FramePushState(std::span<const uint32_t> offsets, std::span<const VkDeviceAddress> addresses) noexcept {
        const size_t count = std::min({offsets.size(), addresses.size(), kMaxAddresses});
        _count             = static_cast<uint32_t>(count);
        for (size_t i = 0; i < count; ++i) {
            _offsets[i]   = offsets[i];
            _addresses[i] = addresses[i];
        }
    }

    [[nodiscard]] constexpr auto Valid() const noexcept -> bool {
        return _count > 0;
    }

    /// Tells the command buffer to receive the frame address block.
    void Push(VkCommandBuffer cmd) const noexcept {
        if (Valid()) {
            PushHeapFrameAddresses(cmd, std::span<const uint32_t> {_offsets.data(), _count}, std::span<const VkDeviceAddress> {_addresses.data(), _count});
        }
    }

  private:
    // The push layout's own ceiling, not a number of this type's choosing: a
    // frame that grows a field cannot be silently truncated here.
    static constexpr size_t kMaxAddresses = HeapPushDataLayout::kMaxAddresses;

    std::array<uint32_t, kMaxAddresses>        _offsets {};
    std::array<VkDeviceAddress, kMaxAddresses> _addresses {};
    uint32_t                                   _count = 0;
};

/// Everything a secondary command buffer must be told before it can record, in
/// one value: the rendering state it inherits, the heap bindings it does not
/// inherit, and the frame rows it must push for itself.
///
/// This is the whole point of the type. Assembling a Vulkan inheritance chain is
/// three unrelated domains meeting at one struct, and it used to be written out
/// twice -- once per chunked draw dispatch, once per parallel fork -- with each
/// copy reaching into a different subsystem for its fields. Here it is one
/// `Begin`, and the chain is built inside it, from sources that own their
/// memory: the formats live in the rendering inheritance, the heap pointers
/// point at the manager's persistent bind infos, and the addresses are copied in
/// by FramePushState.
class SecondaryPassContext {
  public:
    constexpr SecondaryPassContext() noexcept = default;

    /// A raster secondary: dynamic rendering state plus heap and frame state.
    constexpr SecondaryPassContext(RenderingInheritance rendering, HeapInheritance heap, FramePushState framePush) noexcept:
        _rendering(rendering), _heap(heap), _framePush(framePush) {
    }

    /// A non-raster secondary (a compute fork): no rendering state to inherit.
    constexpr SecondaryPassContext(HeapInheritance heap, FramePushState framePush) noexcept: _heap(heap), _framePush(framePush) {
    }

    /// Tells `cmd` to begin as a secondary, with its inheritance chain linked,
    /// its frame rows pushed, and -- for a raster secondary -- its viewport and
    /// scissor set. The returned recorder is the only handle on the begun
    /// buffer, so a secondary cannot be recorded without this having run.
    [[nodiscard]] auto Begin(VkCommandBuffer cmd, VkCommandBufferUsageFlags flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) const noexcept
        -> std::expected<CommandRecorder, ErrorCode> {
        // Both storage structs outlive the vkBeginCommandBuffer call below and
        // are dead the moment it returns; the chain is a call-scoped thing.
        VkCommandBufferInheritanceDescriptorHeapInfoEXT heapInherit {};
        VkCommandBufferInheritanceRenderingInfo         renderInherit {};

        void* pNext = nullptr;
        pNext       = _heap.Link(pNext, heapInherit);
        pNext       = _rendering.Link(pNext, renderInherit);

        const VkCommandBufferInheritanceInfo inheritInfo {
            .sType                = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
            .pNext                = pNext,
            .renderPass           = VK_NULL_HANDLE,
            .subpass              = 0,
            .framebuffer          = VK_NULL_HANDLE,
            .occlusionQueryEnable = VK_FALSE,
            .queryFlags           = 0,
            .pipelineStatistics   = 0,
        };

        auto recorder = CommandRecorder::Begin(cmd, flags, &inheritInfo);
        if (!recorder) {
            return recorder;
        }

        _framePush.Push(cmd);
        _rendering.ApplyViewportAndScissor(cmd);
        return recorder;
    }

    [[nodiscard]] constexpr auto HasRenderingState() const noexcept -> bool {
        return _rendering.Valid();
    }

  private:
    RenderingInheritance _rendering {};
    HeapInheritance      _heap {};
    FramePushState       _framePush {};
};

} // namespace ZHLN::Vk
