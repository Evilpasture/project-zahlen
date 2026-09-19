// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/RenderInit.cpp
#include "RenderInternal.hpp"
#include "Resources.hpp"
#include <ShaderBindings.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <functional>
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

    return Vk::ShaderStages::Create(
        ctx.Device(), {.code = Vk::AsSpirV(vs_code), .size = vs_size, .entry_point = vs.entryPoint},
        {.code = Vk::AsSpirV(ps_code), .size = ps_size, .entry_point = ps.entryPoint}
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
    if (!CheckRayTracingSupport(ctx.Physical()) || !rtCtx.Init(ctx.Device())) {
        ZHLN::Log("WARNING: Raytracing context failed to initialize. RTR will be disabled.");
    } else {
        ZHLN::Log("Raytracing context initialized successfully.");
    }

    // The task/mesh statistic bits need meshShaderQueries ENABLED on the
    // device (VUID-VkQueryPoolCreateInfo-meshShaderQueries-07069); pass the
    // device-creation state, not a physical-device probe.
    if (auto res = gpuProfiler.Init(ctx.Device(), ctx.Physical(), ctx.PhysicalInfo().graphics_family, MeshShaderQueriesEnabled()); !res) {
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
            return CompileShadowPipeline(
                ctx.Device(), Vk::CreateShaderDesc<Shaders::Modules::BasicVSShadow>(), Vk::CreateShaderDesc<Shaders::Modules::ShadowPS>()
            );
        })
        .and_then([&]() {
            return CompilePunctualShadowPipeline(
                ctx.Device(), Vk::CreateShaderDesc<Shaders::Modules::PunctualShadowsVS>(), Vk::CreateShaderDesc<Shaders::Modules::PunctualShadowsPS>()
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

    return parallelRecorder[0]
        .Init(ctx.Device(), ctx.PhysicalInfo().graphics_family)
        .and_then([&]() { return parallelRecorder[1].Init(ctx.Device(), ctx.PhysicalInfo().graphics_family); });
}

std::expected<void, ErrorCode> RenderContext::Impl::InitSubsystems(const RenderConfig& cfg, int width, int height) {
    // Must exist before the first pipeline is built: every PipelineBuilder and
    // ComputePipelineBuilder further down this chain reads pipelineCache.Get().
    // Loading it first is also what lets the second run skip compiling them.
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
        // VK_EXT_descriptor_heap ordering: InitBindless must run FIRST. It
        // initializes the heaps and reserves the globalTextures[] region, and
        // every later pass binding allocates its slots AFTER that region.
        // Allocating pass slots first (the old order) let culling/cluster
        // descriptors land inside the texture array and clobber it.
        .and_then([&]() { return InitBindless(); })
        .and_then([&]() { return InitCullingResources(); })
        .and_then([&]() { return InitCorePipelines(); })
        .and_then([&]() {
            return session.Init(ctx, allocator, width, height, ctx.PhysicalInfo().graphics_family, cfg.vsync);
        })
        .and_then([&]() {
            computePools =
                Vk::CommandPools<2, Vk::QueueType::Compute>::Create(ctx.Device(), {.queueFamily = ctx.PhysicalInfo().compute_family, .buffersPerPool = 1});
            return InitPostProcessing();
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            auto* windowHandle = window.IsTTY() ? nullptr : static_cast<GLFWwindow*>(window.GetNativeHandle());
            return SetupUI(windowHandle);
        })
        .and_then([&]() { return InitParallelRecorders(); })
        .transform([&]() {
            deletionQueue.Init(2);
            auto fvb_res = CreateDoubleBuffered(
                allocator, sizeof(GPUVolumetricVolume) * 64, Vk::BufferUsage::Storage | Vk::BufferUsage::ShaderDeviceAddress,
                Vk::MemoryUsage::CPUToGPU
            );
            if (fvb_res) {
                frames.fogVolumesBuffer = std::move(*fvb_res);
            }
        });
}

} // namespace ZHLN
