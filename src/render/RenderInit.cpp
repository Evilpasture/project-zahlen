// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RenderInternal.hpp"
#include "pipeline/ComputePass.hpp"
#include "Resources.hpp"
#include <ShaderBindings.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <functional>
#include <utility>
#include <vector>

namespace ZHLN {

std::expected<Vk::ShaderStages, ErrorCode> RenderContext::Impl::LoadAndCreateShaders(VertexStageSource vs, FragmentStageSource ps) const noexcept {
    const void*           vs_code = nullptr;
    size_t                vs_size = 0;
    const void*           ps_code = nullptr;
    size_t                ps_size = 0;
    std::vector<uint32_t> disk_vs;
    std::vector<uint32_t> disk_ps;

    LoadShaderData(vs, vs_code, vs_size, disk_vs);
    LoadShaderData(ps, ps_code, ps_size, disk_ps);

    gpuDiagnostics.RegisterShader({.code = Vk::AsSpirV(vs_code), .size = vs_size, .entry_point = vs.entryPoint}, "VSMain");
    gpuDiagnostics.RegisterShader({.code = Vk::AsSpirV(ps_code), .size = ps_size, .entry_point = ps.entryPoint}, "PSMain");

    // Dev-mode shaders come from local vectors; transfer those buffers into
    // ShaderStages. Empty vectors mean the descriptors point at static fallback SPIR-V.
    return Vk::ShaderStages::CreateLoaded(
        {.code = Vk::AsSpirV(vs_code), .size = vs_size, .entry_point = vs.entryPoint}, std::move(disk_vs),
        {.code = Vk::AsSpirV(ps_code), .size = ps_size, .entry_point = ps.entryPoint}, std::move(disk_ps)
    );
}

std::expected<Vk::Pipeline, ErrorCode>
    RenderContext::Impl::LoadAndCreateComputeShader(ComputeStageSource cs, VkPipelineLayout layout, Vk::DynamicComputePass& pass) const noexcept {
    const void*           cs_code = nullptr;
    size_t                cs_size = 0;
    std::vector<uint32_t> disk_cs;

    LoadShaderData(cs, cs_code, cs_size, disk_cs);

    const ZHLN_ShaderDesc shader = {.code = Vk::AsSpirV(cs_code), .size = cs_size, .entry_point = cs.entryPoint};
    gpuDiagnostics.RegisterShader(shader, "CSMain");
    if (shader.code == nullptr || shader.size == 0) {
        return std::unexpected(Vk::ShaderStageCreationError::ShaderLoadingFailed);
    }
    if (!pass.ReflectDispatchLayout(shader)) {
        return std::unexpected(Vk::SpirvLayoutError::ModuleParseFailed);
    }

    return Vk::ComputePipelineBuilder().Shader(shader).Layout(layout).Cache(pipelineCache.Get()).Build(ctx.Device());
}

std::expected<void, ErrorCode> RenderContext::Impl::InitDiagnosticsAndProfiling() {
    if (!ctx.RayTracingSupported()) {
        ZHLN::Log("WARNING: Ray tracing not enabled on this device. RTR will be disabled.");
    } else {
        ZHLN::Log("Ray tracing enabled (acceleration structure + ray query).");
    }

    const bool meshShaderQueries = ctx.HasFeature<VkPhysicalDeviceMeshShaderFeaturesEXT>([](const VkPhysicalDeviceMeshShaderFeaturesEXT& f) -> bool {
        return f.meshShaderQueries == VK_TRUE;
    });
    if (auto res = gpuProfiler.Init(ctx.Device(), ctx.Physical(), ctx.PhysicalInfo().graphics_family, meshShaderQueries); !res) {
        return std::unexpected(res.error());
    }
    if (!gpuProfiler.Enabled()) {
        ZHLN::Log("WARNING: GPU timestamps unavailable on this device/queue family; frame profiling is disabled.");
    }

    return graphicsCmdRing.Init(ctx.Device(), ctx.PhysicalInfo().graphics_family)
        .and_then([&]() { return transferCmdRing.Init(ctx.Device(), ctx.PhysicalInfo().transfer_family); })
        .and_then([&]() { return computeCmdRing.Init(ctx.Device(), ctx.PhysicalInfo().compute_family); });
}

std::expected<void, ErrorCode> RenderContext::Impl::InitCorePipelines() {
    return InitLineBuffers()
        .and_then([&]() { return BuildLinePipeline(); })
        .and_then([&]() { return BuildHangGpuPipeline(); })
        .and_then([&]() { return BuildHiZPipeline(); })
        .and_then([&]() { return BuildProceduralBakePipeline(); })
        .and_then([&]() {
            return shadows.CompileCascadePipelines(
                *this, ctx.Device(), Vk::CreateShaderDesc<Shaders::Modules::BasicVSShadow>(), Vk::CreateShaderDesc<Shaders::Modules::ShadowPS>()
            );
        })
        .and_then([&]() {
            return shadows.CompilePunctualPipeline(
                *this, ctx.Device(), Vk::CreateShaderDesc<Shaders::Modules::PunctualShadowsVS>(),
                Vk::CreateShaderDesc<Shaders::Modules::PunctualShadowsPS>()
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
            pool     = Vk::CommandPool<Vk::QueueType::Graphics>(ctx.Device(), ctx.PhysicalInfo().graphics_family);
            auto res = pool.AllocateSecondary(256);
            if (!res) [[unlikely]] {
                return std::unexpected(res.error());
            }
        }
    }

    for (auto& recorder: parallelRecorders.data) {
        if (auto initialized = recorder.Init(ctx.Device(), ctx.PhysicalInfo().graphics_family); !initialized) {
            return std::unexpected(initialized.error());
        }
    }
    return {};
}

std::expected<void, ErrorCode> RenderContext::Impl::InitSubsystems(const RenderConfig& cfg, int width, int height) {
    pipelineCache = Vk::LoadPipelineCache(ctx.Device(), ctx.PhysicalInfo().properties.properties, pipelineCachePath);

    return allocator.Init(ctx)
        .and_then([&]() {
            return stagingRingBuffer.Init(
                allocator.Get(), ctx.Device(), ctx.GraphicsQueue(), ctx.PhysicalInfo().graphics_family, static_cast<VkDeviceSize>(64 * 1024 * 1024)
            );
        })
        .and_then([&]() {
            return transferRingBuffer.Init(
                allocator.Get(), ctx.Device(), ctx.TransferQueue(), ctx.PhysicalInfo().transfer_family, static_cast<VkDeviceSize>(64 * 1024 * 1024)
            );
        })
        .and_then([&]() { return InitDiagnosticsAndProfiling(); })
        .and_then([&]() { return InitShadowResources(); })
        .and_then([&]() { return InitBindless(); })
        .and_then([&]() { return InitCullingResources(); })
        .and_then([&]() { return InitCorePipelines(); })
        .and_then([&]() {
            return presenter.Init(ctx, allocator, width, height, ctx.PhysicalInfo().graphics_family, cfg.vsync);
        })
        .and_then([&]() {
            computePools =
                Vk::CommandPools<Vk::kFramesInFlight, Vk::QueueType::Compute>::Create(ctx.Device(), {.queueFamily = ctx.PhysicalInfo().compute_family, .buffersPerPool = 1});
            return InitPostProcessing();
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return SetupUI();
        })
        .and_then([&]() { return InitParallelRecorders(); })
        .transform([&]() {
            auto fvb_res = CreateDoubleBuffered(
                allocator, sizeof(GPUVolumetricVolume) * 64, Vk::BufferUsage::Storage | Vk::BufferUsage::ShaderDeviceAddress,
                Vk::MemoryUsage::CPUToGPU
            );
            if (fvb_res) {
                frames.fogVolumesBuffer = std::move(*fvb_res);
            }
        });
}

}
