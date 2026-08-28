// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/engine/graphics/init/RenderInitUI.cpp
#include "../RenderInternal.hpp"
#include "../Resources.hpp"
#include "backends/imgui_impl_glfw.h"
#include "imgui.h"
#include <Zahlen/Error.hpp>
#include <cstdint>

namespace ZHLN {

// Private UI subsystem setup failure (Tier 1): file-local to this TU.
enum class UISetupError : uint8_t {
    SetupFailed[[= ZHLN::Reflect::Description<"UI subsystem setup failed">{}]] = 1,
};

auto RenderContext::Impl::SetupUI(GLFWwindow* glfwWindow) -> std::expected<void, Error> {
    using enum Resource::ShaderID;
    Vk::ShaderStages uiShaders;

    return Vk::ShaderStages::Create(ctx.Device(), Resource::GetShaderProgram(Ui))
        .transform_error([](auto err) -> Error { return err; })
        .and_then([&](auto&& shaders) -> std::expected<void, Error> {
            uiShaders = std::forward<decltype(shaders)>(shaders);
            return {};
        })
        .and_then([&]() -> std::expected<void, Error> {
            // The UI batch pipeline is a descriptor-heap pipeline (scene
            // registry + push data). ImGui is consumed as another producer of
            // UI batches and therefore uses this same pipeline.
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
                .transform_error([](auto err) -> Error { return err; })
                .transform([&](auto&& pipeline) -> auto { uiPipeline = std::forward<decltype(pipeline)>(pipeline); });
        })
        .and_then([&]() -> std::expected<void, Error> {
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            if (glfwWindow != nullptr) {
                ImGui_ImplGlfw_InitForVulkan(glfwWindow, true);
            }

            unsigned char* pixels        = nullptr;
            int            width         = 0;
            int            height        = 0;
            int            bytesPerPixel = 0;
            ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height, &bytesPerPixel);
            if (pixels == nullptr || width <= 0 || height <= 0 || bytesPerPixel != 4) {
                return std::unexpected(UISetupError::SetupFailed);
            }

            return CreateTextureInternal(pixels, static_cast<uint32_t>(width), static_cast<uint32_t>(height), false)
                .transform_error([](auto err) -> Error { return err; })
                .transform([&](uint32_t bindlessIndex) {
                    textureManager.RegisterUploaded("ImGui_FontAtlas", bindlessIndex, false);
                    ImGui::GetIO().Fonts->SetTexID(static_cast<ImTextureID>(static_cast<uintptr_t>(bindlessIndex)));
                });
        });
}

auto RenderContext::Impl::InitUIDynamicBuffers() noexcept -> std::expected<void, Error> {
    return AllocateDynamicVertexBuffers(kMaxUiVertices, frames.uiVbos, frames.uiVboAddresses, 0, "UI");
}

} // namespace ZHLN
