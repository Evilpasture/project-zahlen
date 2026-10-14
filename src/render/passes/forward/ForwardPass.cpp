// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/forward/ForwardPass.hpp"
#include "passes/SceneDraw.hpp"
#include <ShaderBindings.hpp>
#include <cstddef>

namespace ZHLN::Passes {

namespace {

struct ParticleRenderPushConstants {
    VkDeviceAddress particleBufferAddr;
    uint32_t        alignment;
    uint32_t        textureIndex;
};
static_assert(GpuAbi::ScenePassPayload<ParticleRenderPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

// Debug lines carry everything they need in the push block: the view-projection
// and the device address of their interleaved vertex buffer. No instance index,
// because a line is not an entry in the instance table -- debug_line.slang
// declares no descriptor set at all. The block is 80 bytes: 64 of matrix, 8 of
// address, padded to the matrix's 16-byte alignment, which is how
// UIObjectConstants already matches its shader half.
struct LinePushConstants {
    JPH::Mat44      viewProj;
    VkDeviceAddress vertexBufferAddress;
};
static_assert(sizeof(LinePushConstants) == 80);
static_assert(offsetof(LinePushConstants, vertexBufferAddress) == 64);

} // namespace

void ForwardPass::operator()(VkCommandBuffer cmd) const noexcept {
    auto& ctx = frame;

    ctx.BindHeapsAndPushFrame(cmd);

    PassContext passCtx(cmd, frame);
    const auto  sceneVp = frame.EffectiveViewport();

    const auto litColor = Vk::Assume<Vk::ColorWrite<Res_HdrSceneColor>>(frame.graphResources.hdrSceneColor);
    const auto depth    = Vk::Assume<Vk::DepthStencilWrite<Res_Depth>>(frame.presenter.depthTarget);

    Vk::DynamicPass(litColor.Extent())
        .Viewport(sceneVp)
        .AddColor(litColor, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
        .AddDepth(depth, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
        .Execute(cmd, [&]() {
            for (size_t i = 0; i < ctx.queues.Draws().size(); ++i) {
                const auto& drawCmd = ctx.queues.Draws()[i];

                if (!IsForwardOnly(drawCmd.instanceData.flags)) {
                    continue;
                }

                if (drawCmd.material->pipeline == VK_NULL_HANDLE) {
                    continue;
                }

                const ObjectConstants push = {.instanceId = static_cast<uint32_t>(i), .isShadowPass = 0};

                SubmitDrawInstanced(passCtx.encoder, drawCmd, static_cast<uint32_t>(i), push, ctx.MeshShadingActive());
            }

            if (forward.particles.Valid() && !ctx.queues.ParticleEmitters().empty()) {
                for (const auto& emitter: ctx.queues.ParticleEmitters()) {
                    auto* buffer = ctx.geometry.Resolve(emitter.gpuBuffer);
                    if (!buffer) {
                        continue;
                    }

                    const ParticleRenderPushConstants pc = {
                        .particleBufferAddr = ctx.BufferAddress(buffer->buffer.Handle()),
                        .alignment          = static_cast<uint32_t>(emitter.params.alignment),
                        .textureIndex       = emitter.params.textureIndex
                    };

                    passCtx.encoder.DrawInstanced<Shaders::Modules::ParticleRenderVS, Shaders::Modules::ParticleRenderPS>(
                        {.pipeline      = forward.particles.Get(),
                         .vertexCount   = 6,
                         .instanceCount = emitter.maxParticles,
                         .firstVertex   = 0,
                         .firstInstance = 0},
                        pc
                    );
                }
            }

            if (forward.line.Valid() && ctx.activeLineVertexCount > 0) {
                const LinePushConstants pc = {
                    .viewProj            = ctx.current_view_proj,
                    .vertexBufferAddress = ctx.frames.lineVboAddresses[ctx.presenter.frameIndex]
                };

                passCtx.encoder.DrawInstanced<Shaders::Modules::DebugLineVS, Shaders::Modules::DebugLinePS>(
                    {.pipeline      = forward.line.Get(),
                     .vertexCount   = ctx.activeLineVertexCount,
                     .instanceCount = 1,
                     .firstVertex   = 0,
                     .firstInstance = 0},
                    pc
                );
            }
        });
}

}
