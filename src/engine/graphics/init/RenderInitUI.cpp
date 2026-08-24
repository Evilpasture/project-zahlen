// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/engine/graphics/init/RenderInitUI.cpp
#include "../RenderInternal.hpp"
#include "../Resources.hpp"
#include "backends/imgui_impl_glfw.h"
#include "imgui.h"
#include "imgui_impl_vulkan_heap.h"
#include <Zahlen/Error.hpp>

namespace ZHLN {

auto RenderContext::Impl::SetupUI(GLFWwindow* glfwWindow) -> std::expected<void, Error> {
    using enum Resource::ShaderID;
    auto make_expected = [](bool success, Error err) -> std::expected<void, Error> {
        if (success) {
            return {};
        }
        return std::unexpected(err);
    };

    Vk::ShaderStages uiShaders;

    // VK_EXT_descriptor_heap: ImGui renders through the heaps via the
    // imgui_impl_vulkan_heap fork; no descriptor pool exists anymore. Reserve
    // two static sampler slots for the backend's linear/nearest samplers.
    auto imguiSamplerLinear  = heapManager.AllocateStaticSampler();
    auto imguiSamplerNearest = heapManager.AllocateStaticSampler();
    if (!imguiSamplerLinear || !imguiSamplerNearest) {
        return std::unexpected(RenderInitError::UISetupFailed);
    }

    return Vk::ShaderStages::Create(ctx.Device(), Resource::GetShaderProgram(Ui))
        .transform_error([](auto) -> Error { return RenderInitError::UISetupFailed; })
        .and_then([&](auto&& shaders) -> std::expected<void, Error> {
            uiShaders = std::forward<decltype(shaders)>(shaders);
            return {};
        })
        .and_then([&]() -> std::expected<void, Error> {
            // The UI batch pipeline is a descriptor-heap pipeline (scene
            // registry + push data). ImGui renders through the heaps too
            // (rendered last, after all other passes).
            uiPipelineLayout = emptyPipelineLayout;

            VkFormat swapchainFormat = presentation.GetPresentFormat();

            return Vk::PipelineBuilder {}
                .Shaders(uiShaders)
                .Layout(emptyPipelineLayout)
                .HeapMappings(&sceneHeapMappings.info, &sceneHeapMappings.info)
                .ColorFormats(std::array {swapchainFormat})
                .NoDepth()
                .AlphaBlend()
                .CullNone()
                .Build(ctx.Device())
                .transform_error([](auto) -> Error { return RenderInitError::UISetupFailed; })
                .transform([&](auto&& pipeline) -> auto { uiPipeline = std::forward<decltype(pipeline)>(pipeline); });
        })
        .and_then([&]() -> std::expected<void, Error> {
            if (glfwWindow != nullptr) {
                IMGUI_CHECKVERSION();
                ImGui::CreateContext();
                ImGui_ImplGlfw_InitForVulkan(glfwWindow, true);

                VkFormat swapchainFormat = presentation.GetPresentFormat();

                ImGui_ImplVulkanHeap_InitInfo init_info = {
                    .ApiVersion         = VK_API_VERSION_1_3,
                    .Instance           = ctx.Instance(),
                    .PhysicalDevice     = ctx.Physical(),
                    .Device             = ctx.Device(),
                    .QueueFamily        = ctx.PhysicalInfo().graphics_family,
                    .Queue              = ctx.GraphicsQueue(),
                    .DescriptorPool     = VK_NULL_HANDLE,
                    .DescriptorPoolSize = 0,
                    .MinImageCount      = 2,
                    .ImageCount         = 2,
                    .PipelineCache      = VK_NULL_HANDLE,
                    .PipelineInfoMain =
                        {
                            .RenderPass  = VK_NULL_HANDLE,
                            .Subpass     = 0,
                            .MSAASamples = VK_SAMPLE_COUNT_1_BIT,
                            .ExtraDynamicStates {},
                            .PipelineRenderingCreateInfo =
                                {.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                 .pNext                   = nullptr,
                                 .viewMask                = 0,
                                 .colorAttachmentCount    = 1,
                                 .pColorAttachmentFormats = &swapchainFormat,
                                 .depthAttachmentFormat   = VK_FORMAT_D32_SFLOAT_S8_UINT,
                                 .stencilAttachmentFormat = VK_FORMAT_UNDEFINED},
                        },
                    .UseDynamicRendering        = true,
                    .Allocator                  = nullptr,
                    .CheckVkResultFn            = nullptr,
                    .MinAllocationSize          = 0,
                    .CustomShaderVertCreateInfo = {},
                    .CustomShaderFragCreateInfo = {},
                    .HeapInfo                   = {
                        .HeapContext        = &ctx,
                        .HeapManager        = &heapManager,
                        .ResourceSlotBase   = imguiTextureHeapBase,
                        .ResourceSlotCount  = kImGuiTextureSlots,
                        .ResourceStride     = static_cast<uint32_t>(heapManager.ResourceStride()),
                        .SamplerSlotLinear  = imguiSamplerLinear->index,
                        .SamplerSlotNearest = imguiSamplerNearest->index,
                        .SamplerStride      = static_cast<uint32_t>(heapManager.SamplerStride()),
                    },
                };

                return make_expected(ImGui_ImplVulkanHeap_Init(&init_info), RenderInitError::UISetupFailed);
            }
            return {};
        });
}

auto RenderContext::Impl::InitUIDynamicBuffers() noexcept -> std::expected<void, Error> {
    return AllocateDynamicVertexBuffers(kMaxUiVertices, frames.uiVbos, frames.uiVboAddresses, 0, "UI");
}

} // namespace ZHLN
