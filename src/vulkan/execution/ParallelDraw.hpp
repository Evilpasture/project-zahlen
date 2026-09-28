// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

namespace ZHLN::Vk {

namespace detail {
struct ParallelForCallback {
    void operator()([[maybe_unused]] uint32_t start, [[maybe_unused]] uint32_t end, [[maybe_unused]] uint32_t chunkIdx) const noexcept {
    }
};
}

template <typename S>
concept ParallelScheduler = requires(S&& s, uint32_t count, uint32_t chunkSize) { s.ParallelFor(count, chunkSize, detail::ParallelForCallback {}); };

template <ParallelScheduler SchedulerT, typename CmdProviderFn, typename RecordFn>
inline void ParallelDrawDispatch(
    VkCommandBuffer             primaryCmd,
    const SecondaryInheritance& inheritDesc,
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

    const VkCommandBufferInheritanceDescriptorHeapInfoEXT heap_inherit = {
        .sType                 = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_DESCRIPTOR_HEAP_INFO_EXT,
        .pNext                 = nullptr,
        .pSamplerHeapBindInfo  = inheritDesc.samplerHeapBindInfo,
        .pResourceHeapBindInfo = inheritDesc.resourceHeapBindInfo,
    };

    const auto colorFormats = inheritDesc.ColorFormats();
    VkCommandBufferInheritanceRenderingInfo inherit = {
        .sType                   = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO,
        .pNext                   = nullptr,
        .flags                   = 0,
        .viewMask                = inheritDesc.viewMask,
        .colorAttachmentCount    = static_cast<uint32_t>(colorFormats.size()),
        .pColorAttachmentFormats = colorFormats.empty() ? nullptr : colorFormats.data(),
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

    std::forward<SchedulerT>(scheduler).ParallelFor(drawCount, chunkSize, [&](uint32_t start, uint32_t end, uint32_t chunkIdx) noexcept {
        VkCommandBuffer sec_cmd = std::forward<CmdProviderFn>(cmdProvider)(chunkIdx);

        const VkCommandBufferBeginInfo begin_info = {
            .sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .pNext            = nullptr,
            .flags            = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT | VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
            .pInheritanceInfo = &p_inherit
        };

        CommandBufferGuard recordGuard(sec_cmd, begin_info);

        if (!inheritDesc.pushDataFrameAddresses.empty()) {
            PushHeapFrameAddresses(sec_cmd, inheritDesc.pushDataFrameOffsets, inheritDesc.pushDataFrameAddresses);
        }

        CommandEncoder encoder(sec_cmd);

        const VkViewport viewport = inheritDesc.viewport;
        const VkRect2D scissor = {
            .offset = {.x = static_cast<int32_t>(viewport.x), .y = static_cast<int32_t>(viewport.y)},
            .extent = {.width = static_cast<uint32_t>(viewport.width), .height = static_cast<uint32_t>(viewport.height)}
        };
        vkCmdSetViewport(sec_cmd, 0, 1, &viewport);
        vkCmdSetScissor(sec_cmd, 0, 1, &scissor);

        for (uint32_t i = start; i < end; ++i) {
            recordFn(encoder, i);
        }

        recordGuard.End();
        secondaries[chunkIdx] = sec_cmd;
    });

    Vk::ExecuteCommands(primaryCmd, secondaries);
}

}
