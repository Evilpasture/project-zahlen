// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// The compute frame.
//
// Simulation is a separate queue submission from the scene, so it is a separate
// graph: the light-cluster culling that the deferred lighting pass will read,
// the volumetric fog ladder, and the particle updates. It is compiled and bound
// here rather than in RenderGraphBuilder.cpp because this is where the command
// buffer it records into is acquired, reset and submitted.

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

// The cluster volume's froxel bounds only change when the projection does, so
// the bounds pass runs on demand rather than every frame.
void RebuildClusterBounds(RenderContext::Impl& impl, VkCommandBuffer cmd, uint32_t fIdx) noexcept {
    if (!impl.frameState.clusterBoundsDirty || !impl.clusterBoundsPass.Valid() || !impl.clusterBoundsPass.HasFixedDispatchDomain()) {
        return;
    }

    const Vk::HeapBlockBase block = impl.heapManager.WriteHeapParameters<Shaders::ClusterBounds>(
        impl.ctx, impl.clusterBoundsHeapBindings, Vk::Slot<"out_Bounds">(impl.clusterBoundsBuffer), Vk::Slot<"frame">(impl.frames.frameUniformBuffers[fIdx])
    );
    impl.clusterBoundsPass.DispatchHeapIndexed(impl.ctx, cmd, block);
    Vk::MemoryBarrier(cmd, Vk::BarrierStage::Compute, Vk::BarrierAccess::ShaderWrite, Vk::BarrierStage::Compute, Vk::BarrierAccess::ShaderRead);
    impl.frameState.clusterBoundsDirty = false;
}

// Binds a resource the reflected target bundle does not own. Guarded by a
// membership test on the graph's resource list so the binder is never asked
// about a tag it has no slot for.
template <typename Resources, typename Tag, typename Binder, typename RefFn>
void BindExternalReflected(Binder& binder, RefFn&& makeRef) {
    if constexpr (Vk::IsInList<Resources, Tag>::value) {
        auto ref = std::forward<RefFn>(makeRef)();
        binder.template Bind<Tag>(ref.handle, ref.view, ref.extent);
    }
}

// Recording the compute frame is a function of its own, not a block inside
// `Submit`, because `Vk::CommandBufferGuard` ends the command buffer when it
// goes out of scope: the buffer has to leave the recording state before
// `vkQueueSubmit2` will accept it, which means the guard has to die first.
void RecordComputeFrame(RenderContext::Impl& impl, float dt) noexcept {
    const uint32_t fIdx = impl.presenter.frameIndex;

    Vk::CommandBufferGuard guard(impl.current_compute_cmd);

    impl.BindHeapsAndPushFrame(impl.current_compute_cmd);

    RebuildClusterBounds(impl, impl.current_compute_cmd, fIdx);

    // The simulation passes themselves. Every one of them is a struct that
    // names its own resources, so the graph decides what can overlap: the
    // volumetric ladder is strictly ordered through the voxel grid, while the
    // two particle updates touch disjoint buffers and are free to fork.
    auto computeGraph = Vk::MakePassPack(
                            Passes::ClusterCullingPass {.impl = impl},
                            Passes::VolumetricFogInjectPass {.impl = impl},
                            Passes::VolumetricLightInjectPass {.impl = impl},
                            Passes::VolumetricIntegrationPass {.impl = impl},
                            Passes::VolumetricTemporalPass {.impl = impl},
                            Passes::ParticleUpdatePass {.impl = impl, .dt = dt},
                            Passes::MeshParticleUpdatePass {.impl = impl, .dt = dt}
    )
                            .BuildGraph();

    using ComputeResources = typename decltype(computeGraph)::Resources;
    typename decltype(computeGraph)::Binder compBinder;

    compBinder.AutoBind(impl);

    // The light injection pass shadows against last frame's cascade map: the
    // current one is being written by the scene graph on the graphics queue,
    // and the two are swapped at `EndFrame`.
    BindExternalReflected<ComputeResources, Res_ShadowMap>(compBinder, [&] { return Vk::MakeRef<Res_ShadowMap>(impl.targets.ShadowMapPrev()); });

    auto* diagnostics = impl.gpuDiagnostics.IsActive() ? &impl.gpuDiagnostics : nullptr;
    computeGraph.Execute(impl.current_compute_cmd, compBinder, impl.presenter.frameIndex, &impl.gpuProfiler, diagnostics);
}

} // namespace

auto ComputeSimPipeline::Submit(RenderContext::Impl& impl, float dt) noexcept -> RenderResult {
    if (impl.ctx.Device() == VK_NULL_HANDLE) {
        return {};
    }

    const uint32_t slot = impl.presenter.frameIndex;

    impl.currentDt           = dt;
    impl.current_compute_cmd = impl.computePools[slot][0];

    RecordComputeFrame(impl, dt);

    // The guard inside `RecordComputeFrame` ended the command buffer when that
    // function returned, so the buffer is executable now and safe to hand to
    // the queue. Submitting it from a scope where the guard is still alive
    // would hand `vkQueueSubmit2` a buffer that is still recording.
    const uint64_t signalValue = impl.presenter.sync.GetTimelineValue(slot);
    auto           submitted   = Vk::QueueSubmit(
        impl.ctx, impl.current_compute_cmd, VK_NULL_HANDLE, 0, VK_PIPELINE_STAGE_2_NONE, impl.presenter.sync.ComputeTimeline(slot), signalValue,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
    );

    if (!submitted) [[unlikely]] {
        if (submitted.error().Is(FrameResult::DeviceLost)) {
            Vk::Instance::IncrementNumericalDeviceLoss();
        } else {
            ZHLN::Log("[DispatchSimulations] Compute submission failed ({}).", submitted.error());
        }
        return std::unexpected(submitted.error());
    }

    impl.frameState.computeSubmitted = true;
    return {};
}

}
