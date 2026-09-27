// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/forward/ForwardPass.hpp"
#include "passes/SceneDraw.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

namespace {

struct ParticleRenderPushConstants {
    VkDeviceAddress particleBufferAddr;
    uint32_t        alignment;
    uint32_t        textureIndex;
};
static_assert(GpuAbi::ScenePassPayload<ParticleRenderPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

} // namespace

void ForwardPass::operator()(VkCommandBuffer cmd) const noexcept {
    auto& ctx = impl;

    ctx.BindHeapsAndPushFrame(cmd);

    FrameRecorder recorder(cmd, impl);
    const auto    sceneVp = impl.EffectiveViewport();

    const auto litColor = Vk::Assume<Vk::ColorWrite<Res_HdrSceneColor>>(impl.graphResources.hdrSceneColor);
    const auto depth    = Vk::Assume<Vk::DepthStencilWrite<Res_Depth>>(impl.presenter.depthTarget);

    Vk::DynamicPass(litColor.extent)
        .Viewport(sceneVp)
        .AddColor(litColor, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
        .AddDepth(depth, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
        .Execute(cmd, [&]() {
            for (size_t i = 0; i < ctx.queues.Draws().size(); ++i) {
                const auto& drawCmd = ctx.queues.Draws()[i];

                if ((drawCmd.instanceData.flags & 0xFF) != 2) {
                    continue;
                }

                if (!drawCmd.material->pipeline.Valid()) {
                    continue;
                }

                const RenderContext::Impl::ObjectConstants push = {.instanceId = static_cast<uint32_t>(i), .isShadowPass = 0};

                SubmitDrawInstanced(recorder.encoder, drawCmd, static_cast<uint32_t>(i), push, ctx.MeshShadingActive());
            }

            if (ctx.particleRenderPipeline.Valid() && !ctx.queues.ParticleEmitters().empty()) {
                for (const auto& emitter: ctx.queues.ParticleEmitters()) {
                    auto* buffer = ctx.geometry.Resolve(emitter.gpuBuffer).value_or(nullptr);
                    if (!buffer) {
                        continue;
                    }

                    const ParticleRenderPushConstants pc = {
                        .particleBufferAddr = ctx.BufferAddress(buffer->buffer.Handle()),
                        .alignment          = static_cast<uint32_t>(emitter.params.alignment),
                        .textureIndex       = emitter.params.textureIndex
                    };

                    recorder.encoder.DrawInstanced<Shaders::Modules::ParticleRenderVS, Shaders::Modules::ParticleRenderPS>(
                        {.pipeline      = ctx.particleRenderPipeline.Get(),
                         .layout        = ctx.particleRenderLayout,
                         .heap          = true,
                         .vertexCount   = 6,
                         .instanceCount = emitter.maxParticles,
                         .firstVertex   = 0,
                         .firstInstance = 0},
                        pc, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
                    );
                }
            }

            if (ctx.linePipeline.Valid() && ctx.activeLineVertexCount > 0) {
                const RenderContext::Impl::ObjectConstants pc = {.instanceId = ctx.lineInstanceId, .isShadowPass = 0};

                recorder.encoder.DrawInstanced<Shaders::Modules::BasicVSForward>(
                    {.pipeline      = ctx.linePipeline.Get(),
                     .layout        = ctx.linePipelineLayout,
                     .heap          = true,
                     .vertexCount   = ctx.activeLineVertexCount,
                     .instanceCount = 1,
                     .firstVertex   = 0,
                     .firstInstance = ctx.lineInstanceId},
                    pc, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
                );
            }
        });
}

}
