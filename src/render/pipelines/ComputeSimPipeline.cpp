// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ComputeSimPipeline.hpp"
#include "../passes/culling/ClusterCullingPass.hpp"
#include "../passes/simulation/MeshParticleUpdatePass.hpp"
#include "../passes/simulation/ParticleUpdatePass.hpp"
#include "../passes/volumetric/VolumetricFogInjectPass.hpp"
#include "../passes/volumetric/VolumetricIntegrationPass.hpp"
#include "../passes/volumetric/VolumetricLightInjectPass.hpp"
#include "../passes/volumetric/VolumetricTemporalPass.hpp"
#include "graph/RenderGraph.hpp"
#include <ShaderBindings.hpp>
#include <Zahlen/Log.hpp>

namespace ZHLN::Pipelines {

namespace {

void RebuildClusterBounds(RenderContext::Impl& impl, VkCommandBuffer cmd, uint32_t fIdx) noexcept {
    if (!impl.frameState.clusterBoundsDirty || !impl.culling.clusterBoundsPass.Valid() || !impl.culling.clusterBoundsPass.HasFixedDispatchDomain()) {
        return;
    }

    const Vk::HeapBlockBase block = impl.heapManager.WriteHeapParameters<Shaders::ClusterBounds>(
        impl.culling.clusterBoundsHeapBindings, Vk::Slot<"out_Bounds">(impl.clusterBoundsBuffer), Vk::Slot<"frame">(impl.frames.frameUniformBuffers[fIdx])
    );
    impl.culling.clusterBoundsPass.DispatchHeapIndexed(cmd, block);
    Vk::MemoryBarrier(cmd, Vk::BarrierStage::Compute, Vk::BarrierAccess::ShaderWrite, Vk::BarrierStage::Compute, Vk::BarrierAccess::ShaderRead);
    impl.frameState.clusterBoundsDirty = false;
}

template <typename Resources, typename Tag, typename Binder, typename RefFn>
void BindExternalReflected(Binder& binder, RefFn&& makeRef) {
    if constexpr (Vk::IsInList<Resources, Tag>::value) {
        auto ref = std::forward<RefFn>(makeRef)();
        binder.template Bind<Tag>(ref);
    }
}

void RecordComputeFrame(RenderContext::Impl& impl, VkCommandBuffer cmd, float dt) noexcept {
    const uint32_t fIdx = impl.presenter.frameIndex;
    impl.BindHeapsAndPushFrame(cmd);
    RebuildClusterBounds(impl, cmd, fIdx);

    auto computeGraph = Vk::MakePassPack(
                            Passes::ClusterCullingPass {.cull = impl.culling, .frame = impl}, Passes::VolumetricFogInjectPass {.frame = impl},
                            Passes::VolumetricLightInjectPass {.frame = impl}, Passes::VolumetricIntegrationPass {.frame = impl},
                            Passes::VolumetricTemporalPass {.frame = impl},
                            Passes::ParticleUpdatePass {.particles = impl.particleUpdate, .frame = impl, .dt = dt},
                            Passes::MeshParticleUpdatePass {.particles = impl.particleUpdate, .frame = impl, .dt = dt}
    )
                            .BuildGraph();

    using ComputeResources = typename decltype(computeGraph)::Resources;
    typename decltype(computeGraph)::Binder compBinder;

    compBinder.AutoBind(impl);

    BindExternalReflected<ComputeResources, Res_ShadowMap>(compBinder, [&] { return Vk::MakeRef<Res_ShadowMap>(impl.targets.ShadowMapPrev()); });

    auto* diagnostics = impl.diagnostics.gpuDiagnostics.IsActive() ? &impl.diagnostics.gpuDiagnostics : nullptr;
    computeGraph.Execute(cmd, compBinder, impl.presenter.frameIndex, &impl.diagnostics.gpuProfiler, diagnostics);
}

} // namespace

auto ComputeSimPipeline::Submit(RenderContext::Impl& impl, float dt) noexcept -> RenderResult {
    if (impl.ctx.Device() == VK_NULL_HANDLE) {
        return {};
    }

    const uint32_t slot = impl.presenter.frameIndex;

    impl.currentDt = dt;
    auto recording = Vk::CommandRecorder::Begin(impl.computePools[slot][0]);
    if (!recording) {
        return std::unexpected(recording.error());
    }
    RecordComputeFrame(impl, recording->Handle(), dt);
    auto executable = std::move(*recording).End();
    if (!executable) {
        return std::unexpected(executable.error());
    }

    const uint64_t signalValue = impl.presenter.sync.GetTimelineValue(slot);
    auto           submitted   = Vk::QueueSubmit(
        impl.ctx.ComputeQueue(), std::move(*executable), VK_NULL_HANDLE, 0, VK_PIPELINE_STAGE_2_NONE, impl.presenter.sync.ComputeTimeline(slot), signalValue,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
    );

    if (!submitted) [[unlikely]] {
        if (Vk::IsDeviceLost(submitted.error())) {
            impl.ctx.Instance().IncrementDeviceLost();
        } else {
            ZHLN::Log("[DispatchSimulations] Compute submission failed ({}).", submitted.error());
        }
        return std::unexpected(AsFrameError(submitted.error()));
    }

    impl.presenter.sync.MarkComputeSubmitted(slot);
    impl.frameState.computeSubmitted = true;
    return {};
}

} // namespace ZHLN::Pipelines
