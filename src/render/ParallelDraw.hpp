// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/ParallelDraw.hpp
#pragma once
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

namespace ZHLN::Vk {

struct SecondaryInheritance {
    std::span<const VkFormat> colorFormats;
    VkFormat                  depthFormat = VK_FORMAT_UNDEFINED;
    // Must match the render pass the secondary is executed in:
    // VUID-vkCmdExecuteCommands-pStencilAttachment-06775. Only set this when
    // the pass actually binds a stencil attachment (the depth format alone is
    // NOT enough — most depth passes have pStencilAttachment == NULL).
    VkFormat stencilFormat = VK_FORMAT_UNDEFINED;

    // VK_EXT_descriptor_heap: when non-null, the secondary inherits the
    // primary's bound heaps so it can draw with heap-based pipelines.
    const VkBindHeapInfoEXT* samplerHeapBindInfo  = nullptr;
    const VkBindHeapInfoEXT* resourceHeapBindInfo = nullptr;

    // Required for heap-mode draws: vkCmdPushDataEXT dispatch goes through the
    // context's loaded entry points.
    const Context* context = nullptr;

    // Optional per-frame push-data fields (e.g. the scene registry's device
    // addresses): pushed once into every secondary right after it begins.
    // Offsets are reflected independently so Slang-inserted padding is kept.
    std::span<const uint32_t>        pushDataFrameOffsets;
    std::span<const VkDeviceAddress> pushDataFrameAddresses;
};

namespace detail {
// Archetype callback to test scheduler invocation without using lambdas in unevaluated contexts
struct ParallelForCallback {
    void operator()([[maybe_unused]] uint32_t start, [[maybe_unused]] uint32_t end, [[maybe_unused]] uint32_t chunkIdx) const noexcept {
    }
};
} // namespace detail

/**
 * @brief Concept enforcing that the scheduler policy provides a valid ParallelFor loop
 * with the correct signature.
 */
template <typename S>
concept ParallelScheduler = requires(S&& s, uint32_t count, uint32_t chunkSize) { s.ParallelFor(count, chunkSize, detail::ParallelForCallback {}); };

/**
 * @brief Fully decoupled, template-driven parallel command recorder.
 * Enforces the ParallelScheduler concept at compile-time for friendly error reporting.
 */
template <ParallelScheduler SchedulerT, typename CmdProviderFn, typename RecordFn>
inline void ParallelDrawDispatch(
    VkCommandBuffer             primaryCmd,
    const SecondaryInheritance& inheritDesc,
    VkExtent2D                  extent,
    uint32_t                    drawCount,
    uint32_t                    chunkSize,
    SchedulerT&&                scheduler,
    CmdProviderFn&&             cmdProvider,
    RecordFn&&                  recordFn
) {
    uint32_t num_chunks = (drawCount + chunkSize - 1) / chunkSize;
    if (num_chunks == 0) {
        return;
    }

    std::vector<VkCommandBuffer> secondaries(num_chunks, VK_NULL_HANDLE);

    // VK_EXT_descriptor_heap: heap-binding inheritance for heap-based pipelines.
    // Only chain it when the caller actually supplies heap bind descriptors.
    const VkCommandBufferInheritanceDescriptorHeapInfoEXT heap_inherit = {
        .sType                 = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_DESCRIPTOR_HEAP_INFO_EXT,
        .pNext                 = nullptr,
        .pSamplerHeapBindInfo  = inheritDesc.samplerHeapBindInfo,
        .pResourceHeapBindInfo = inheritDesc.resourceHeapBindInfo,
    };

    VkCommandBufferInheritanceRenderingInfo inherit = {
        .sType                   = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO,
        .pNext                   = nullptr,
        .flags                   = 0,
        .viewMask                = 0,
        .colorAttachmentCount    = static_cast<uint32_t>(inheritDesc.colorFormats.size()),
        .pColorAttachmentFormats = inheritDesc.colorFormats.data(),
        .depthAttachmentFormat   = inheritDesc.depthFormat,
        .stencilAttachmentFormat = inheritDesc.stencilFormat,
        .rasterizationSamples    = VK_SAMPLE_COUNT_1_BIT
    };
    if (inheritDesc.samplerHeapBindInfo != nullptr || inheritDesc.resourceHeapBindInfo != nullptr) {
        inherit.pNext = &heap_inherit;
    }

    const VkCommandBufferInheritanceInfo p_inherit = {
        .sType                = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
        .pNext                = &inherit,
        .renderPass           = VK_NULL_HANDLE,
        .subpass              = 0,
        .framebuffer          = VK_NULL_HANDLE,
        .occlusionQueryEnable = VK_FALSE,
        .queryFlags           = 0,
        .pipelineStatistics   = 0
    };

    // Parallel execution delegated to the compile-time injected scheduler policy
    std::forward<SchedulerT>(scheduler).ParallelFor(drawCount, chunkSize, [&](uint32_t start, uint32_t end, uint32_t chunkIdx) noexcept {
        // Fetch the task-local command buffer via the callback lambda
        VkCommandBuffer sec_cmd = std::forward<CmdProviderFn>(cmdProvider)(chunkIdx);

        const VkCommandBufferBeginInfo begin_info = {
            .sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .pNext            = nullptr,
            .flags            = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT | VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
            .pInheritanceInfo = &p_inherit
        };

        vkBeginCommandBuffer(sec_cmd, &begin_info);

        // VK_EXT_descriptor_heap: push data does not carry over from the
        // primary, so re-push the per-frame block once per secondary.
        if (!inheritDesc.pushDataFrameAddresses.empty() && inheritDesc.context != nullptr) {
            PushHeapFrameAddresses(*inheritDesc.context, sec_cmd, inheritDesc.pushDataFrameOffsets, inheritDesc.pushDataFrameAddresses);
        }

        // Instantiated locally on the thread's stack.
        // No thread_local, no global shared state.
        CommandEncoder encoder(sec_cmd, inheritDesc.context);

        // Standard, un-flipped viewport
        const VkViewport viewport = {
            .x = 0.0F, .y = 0.0F, .width = static_cast<float>(extent.width), .height = static_cast<float>(extent.height), .minDepth = 0.0F, .maxDepth = 1.0F
        };
        const VkRect2D scissor = {.offset = {.x = 0, .y = 0}, .extent = extent};
        vkCmdSetViewport(sec_cmd, 0, 1, &viewport);
        vkCmdSetScissor(sec_cmd, 0, 1, &scissor);

        for (uint32_t i = start; i < end; ++i) {
            recordFn(encoder, i);
        }

        vkEndCommandBuffer(sec_cmd);
        secondaries[chunkIdx] = sec_cmd;
    });

    Vk::ExecuteCommands(primaryCmd, secondaries);
}

} // namespace ZHLN::Vk
