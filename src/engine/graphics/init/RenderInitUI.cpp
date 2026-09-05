// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/engine/graphics/init/RenderInitUI.cpp
#include "../RenderInternal.hpp"
#include "../Resources.hpp"
#include <Zahlen/Error.hpp>
#include <cstdint>

namespace ZHLN {

// Private UI subsystem setup failure (Tier 1): file-local to this TU.
enum class UISetupError : uint8_t {
    SetupFailed ZHLN_ANNOTATION(ZHLN::Description<"UI subsystem setup failed">{}) = 1,
};

auto RenderContext::Impl::SetupUI([[maybe_unused]] GLFWwindow* glfwWindow) -> std::expected<void, Error> {
    using enum Resource::ShaderID;
    Vk::ShaderStages uiShaders;

    return Vk::ShaderStages::Create(ctx.Device(), Resource::GetShaderProgram(Ui))
        .transform_error([](auto err) -> Error { return err; })
        .and_then([&](auto&& shaders) -> std::expected<void, Error> {
            uiShaders = std::forward<decltype(shaders)>(shaders);
            return {};
        })
        .and_then([&]() -> std::expected<void, Error> {
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
        });
}

auto RenderContext::Impl::InitUIDynamicBuffers() noexcept -> std::expected<void, Error> {
    return AllocateDynamicVertexBuffers(kMaxUiVertices, frames.uiVbos, frames.uiVboAddresses, 0, "UI");
}

} // namespace ZHLN
