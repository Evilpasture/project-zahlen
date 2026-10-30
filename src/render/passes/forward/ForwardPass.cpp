// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/forward/ForwardPass.hpp"
#include "Renderer.hpp"
#include "passes/SceneDraw.hpp"
#include "passes/forward/ForwardResources.hpp"
#include <GeneratedGpuTypes.hpp>
#include <ShaderBindings.hpp>
#include <cstddef>

namespace ZHLN::Passes {

namespace {

using ParticleRenderPushConstants = Slang::ParticleRenderPushConstants;
static_assert(GpuAbi::ScenePassPayload<ParticleRenderPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

using LinePushConstants = Slang::LinePushConstants;

} // namespace

void ForwardPass::operator()(VkCommandBuffer cmd) const noexcept {
    auto& ctx = frame;

    Vk::CommandEncoder encoder(cmd);
    const auto         sceneVp = frame.viewport;

    const auto litColor = Vk::Assume<Vk::ColorWrite<Res_HdrSceneColor>>(frame.graphResources.hdrSceneColor);
    const auto depth    = Vk::Assume<Vk::DepthStencilWrite<Res_Depth>>(frame.ActiveDepthTarget());

    Vk::DynamicPass(litColor.Extent()).Viewport(sceneVp).LoadColor(litColor).LoadDepth(depth).Execute(cmd, [&]() {
        for (size_t i = 0; i < ctx.renderer.queues.Draws().size(); ++i) {
            const auto& drawCmd = ctx.renderer.queues.Draws()[i];

            if (!IsForwardOnly(drawCmd.instanceData.flags) || drawCmd.material->pipeline == VK_NULL_HANDLE) {
                continue;
            }

            const ObjectConstants push = {.instanceId = static_cast<uint32_t>(i), .isShadowPass = 0};

            SubmitDrawInstanced(encoder, drawCmd, static_cast<uint32_t>(i), push, ctx.meshShading);
        }

        if (forward.particles.Valid() && !ctx.renderer.queues.ParticleEmitters().empty()) {
            for (const auto& emitter: ctx.renderer.queues.ParticleEmitters()) {
                const auto* buffer = ctx.renderer.geometry.Resolve(emitter.gpuBuffer);
                if (!buffer) {
                    continue;
                }

                const ParticleRenderPushConstants pc = {
                    .particleBufferAddr = ctx.renderer.BufferAddress(buffer->buffer.Handle()),
                    .alignment          = emitter.params.alignment,
                    .textureIndex       = emitter.params.textureIndex
                };

                encoder.DrawInstanced<Shaders::Modules::ParticleRenderVS, Shaders::Modules::ParticleRenderPS>(
                    {.pipeline = forward.particles.Get(), .vertexCount = 6, .instanceCount = emitter.maxParticles, .firstVertex = 0, .firstInstance = 0}, pc
                );
            }
        }

        if (forward.line.Valid() && ctx.activeLineVertexCount > 0) {
            const LinePushConstants pc = {.viewProj = ctx.viewProj, .vertexBufferAddress = ctx.lineVboAddresses};

            encoder.DrawInstanced<Shaders::Modules::DebugLineVS, Shaders::Modules::DebugLinePS>(
                {.pipeline = forward.line.Get(), .vertexCount = ctx.activeLineVertexCount, .instanceCount = 1, .firstVertex = 0, .firstInstance = 0}, pc
            );
        }
    });
}

} // namespace ZHLN::Passes
