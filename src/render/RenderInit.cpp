// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RenderInternal.hpp"
#include "pipeline/ComputePass.hpp"
#include <ShaderBindings.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <utility>

namespace ZHLN {

auto RenderContext::Impl::LoadAndCreateShaders(Vk::VertexStageSource vs, Vk::FragmentStageSource ps) const noexcept
    -> std::expected<Vk::OwnedShaderStages, ErrorCode> {
    auto vertex   = LoadShaderData(vs);
    auto fragment = LoadShaderData(ps);

    return Vk::OwnedShaderStages::Create(std::move(vertex), std::move(fragment), vs.entryPoint, ps.entryPoint);
}

std::expected<void, ErrorCode> RenderContext::Impl::InitDiagnosticsAndProfiling() {
    if (!ctx.RayTracingSupported()) {
        ZHLN::LogWarning("Ray tracing not enabled on this device. RTR will be disabled.");
    } else {
        ZHLN::Log("Ray tracing enabled (acceleration structure + ray query).");
    }

    const bool meshShaderQueries = ctx.HasFeature<VkPhysicalDeviceMeshShaderFeaturesEXT>([](const VkPhysicalDeviceMeshShaderFeaturesEXT& f) -> bool {
        return f.meshShaderQueries == VK_TRUE;
    });
    if (auto res = diagnostics.gpuProfiler.Init(ctx.Device(), ctx.Physical(), ctx.PhysicalInfo().graphicsFamily, meshShaderQueries); !res) {
        return std::unexpected(res.error());
    }
    if (!diagnostics.gpuProfiler.Enabled()) {
        ZHLN::LogWarning("GPU timestamps unavailable on this device/queue family; frame profiling is disabled.");
    }

    return graphicsCmdRing.Init(ctx.Device(), ctx.PhysicalInfo().graphicsFamily)
        .and_then([&]() { return transferCmdRing.Init(ctx.Device(), ctx.PhysicalInfo().transferFamily); })
        .and_then([&]() { return computeCmdRing.Init(ctx.Device(), ctx.PhysicalInfo().computeFamily); });
}

std::expected<void, ErrorCode> RenderContext::Impl::InitCorePipelines() {
    return InitLineBuffers()
        .and_then([&]() { return BuildLinePipeline(); })
        .and_then([&]() { return BuildEmissivePipeline(); })
        .and_then([&]() { return BuildHangGpuPipeline(); })
        .and_then([&]() { return BuildHiZPipeline(); })
        .and_then([&]() { return BuildProceduralBakePipeline(); })
        .and_then([&]() {
            return shadows.CompileCascadePipelines(
                *this, ctx.Device(), Vk::CreateShaderDesc<Shaders::Modules::BasicVSShadow>(), Vk::CreateShaderDesc<Shaders::Modules::ShadowPS>()
            );
        })
        .and_then([&]() {
            return shadows.CompileAtlasPipeline(
                *this, ctx.Device(), Vk::CreateShaderDesc<Shaders::Modules::ShadowAtlasVS>(), Vk::CreateShaderDesc<Shaders::Modules::ShadowPS>()
            );
        })
        .and_then([&]() { return InitCSGPipelines(); });
}

std::expected<void, ErrorCode> RenderContext::Impl::InitParallelRecorders() {
    uint32_t workerCount = TaskSystem::GetWorkerCount() + 1;
    if (workerCount == 0) {
        workerCount = 1;
    }
    workerCmds.resize(workerCount);

    for (auto& worker: workerCmds) {
        for (auto& pool: worker.pools) {
            auto created = Vk::CommandPool<Vk::QueueType::Graphics>::Create(ctx.Device(), ctx.PhysicalInfo().graphicsFamily);
            if (!created) [[unlikely]] {
                return std::unexpected(created.error());
            }
            pool = std::move(*created);
            if (auto res = pool.AllocateSecondary(256); !res) [[unlikely]] {
                return std::unexpected(res.error());
            }
        }
    }

    for (auto& recorder: parallelRecorders) {
        if (auto initialized = recorder.Init(ctx.Device(), ctx.PhysicalInfo().graphicsFamily); !initialized) {
            return std::unexpected(initialized.error());
        }
    }
    return {};
}

std::expected<void, ErrorCode> RenderContext::Impl::InitSubsystems(const RenderConfig& cfg, int width, int height) {
    pipelineBinaries.Init(ctx.Device(), ctx.PipelineBinariesSupported(), cfg.initialPipelineBinaryData);

    return allocator.Init(ctx)
        .and_then([&]() {
            deletionQueue.Init(allocator);
            return stagingRingBuffer.Init(
                allocator, ctx.Device(), ctx.GraphicsQueue(), ctx.PhysicalInfo().graphicsFamily, static_cast<VkDeviceSize>(64 * 1024 * 1024)
            );
        })
        .and_then([&]() {
            return transferRingBuffer.Init(
                allocator, ctx.Device(), ctx.TransferQueue(), ctx.PhysicalInfo().transferFamily, static_cast<VkDeviceSize>(64 * 1024 * 1024)
            );
        })
        .and_then([&]() { return InitDiagnosticsAndProfiling(); })
        .and_then([&]() { return InitShadowResources(); })
        .and_then([&]() { return InitBindless(); })
        .and_then([&]() { return InitCullingResources(); })
        .and_then([&]() { return InitCorePipelines(); })
        .and_then([&]() { return presenter.Init(ctx, allocator, width, height, ctx.PhysicalInfo().graphicsFamily, cfg.vsync); })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            auto created = Vk::CommandPools<Vk::kFramesInFlight, Vk::QueueType::Compute>::Create(
                ctx.Device(), {.queueFamily = ctx.PhysicalInfo().computeFamily, .buffersPerPool = 1}
            );
            if (!created) [[unlikely]] {
                return std::unexpected(created.error());
            }
            computePools = std::move(*created);
            return InitPostProcessing();
        })
        .and_then([&]() -> std::expected<void, ErrorCode> { return SetupUI(); })
        .and_then([&]() { return InitParallelRecorders(); });
}

} // namespace ZHLN
