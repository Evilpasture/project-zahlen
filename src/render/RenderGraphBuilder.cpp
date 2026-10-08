// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// The frame pipeline, as a declaration.
//
// Nothing is recorded here. Each pass is a struct that knows which resources it
// touches and how to record itself, so this file's whole job is to list them in
// the right order and let the graph engine work out the hazards, the barriers
// and which ones can be replayed concurrently. A pass that needs frame state
// reads it from the `RenderContext::Impl&` it is handed at construction; a pass
// that needs per-frame tuning values carries them as members.

#include "RenderInternal.hpp"
#include "Zahlen/Math3D.hpp"
#include "graph/RenderGraph.hpp"
#include "passes/aa/FXAAPass.hpp"
#include "passes/aa/MLAAPass.hpp"
#include "passes/aa/SMAAPasses.hpp"
#include "passes/aa/TAAPass.hpp"
#include "passes/culling/HiZGeneratePass.hpp"
#include "passes/forward/ForwardPass.hpp"
#include "passes/forward/OpaqueSceneCopyPass.hpp"
#include "passes/forward/TranslucentPrePass.hpp"
#include "passes/gbuffer/DecalPass.hpp"
#include "passes/gbuffer/GBufferBasePass.hpp"
#include "passes/gbuffer/GBufferResolvePass.hpp"
#include "passes/gbuffer/ViewmodelPass.hpp"
#include "passes/lighting/ClusteredLightingPass.hpp"
#include "passes/lighting/GtaoPass.hpp"
#include "passes/lighting/ReflectionPass.hpp"
#include "passes/lighting/RtrHalfTracePass.hpp"
#include "passes/lighting/ShadowPass.hpp"
#include "passes/postprocess/BloomPass.hpp"
#include "passes/postprocess/HdrDenoisePass.hpp"
#include "passes/postprocess/TonemapBlitPass.hpp"
#include <ShaderBindings.hpp>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <Zahlen/Core/Reflection/Structs.hpp>
#include <Zahlen/GraphicsSettings.hpp>
#include <Zahlen/Render/View.hpp>
#include <utility>

namespace ZHLN {

namespace Vk {

template <>
struct ResourceResolver<Res_Depth> {
    [[nodiscard]] static constexpr auto Resolve(RenderContext::Impl& impl) noexcept {
        return MakeRef<Res_Depth>(impl.ActivePresentation().depthTarget);
    }
};

template <>
struct ResourceResolver<Res_ShadowMap> {
    [[nodiscard]] static constexpr auto Resolve(RenderContext::Impl& impl) noexcept {
        return MakeRef<Res_ShadowMap>(impl.graphResources.shadowMap);
    }
};

template <>
struct ResourceResolver<Res_TransLighting> {
    [[nodiscard]] static auto Resolve(RenderContext::Impl& impl) noexcept -> ImageSlice {
        const auto& target = impl.graphResources.transLightingTarget;
        // The graph uses mip 0 as a color attachment; the scene descriptor
        // separately samples the full mip chain (WriteTransLightingToHeap).
        return ImageSlice {target.image.Handle(), target.mipViews[0], target.extent, Res_TransLighting::format};
    }
};

template <>
struct ResourceResolver<Res_AccumPrevious> {
    [[nodiscard]] static constexpr auto Resolve(RenderContext::Impl& impl) noexcept {
        return MakeRef<Res_AccumPrevious>(impl.accumulationHistory.Previous());
    }
};

template <>
struct ResourceResolver<Res_AccumCurrent> {
    [[nodiscard]] static constexpr auto Resolve(RenderContext::Impl& impl) noexcept {
        return MakeRef<Res_AccumCurrent>(impl.accumulationHistory.Current());
    }
};

template <>
struct ResourceResolver<Res_Swapchain> {
    [[nodiscard]] static constexpr auto Resolve(RenderContext::Impl& impl) noexcept {
        return *impl.sceneTarget;
    }
};

} // namespace Vk

namespace {

using SwapchainImage = Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>;

// The one payload shared by every fullscreen scene pass: the matrices the
// deferred shaders reconstruct position with, the camera position and time, and
// the GI tuning the lighting, AO and reflection passes all read.
using ScenePushConstants = GeneratedGpu::ScenePassPushConstants;

[[nodiscard]] auto
    AssembleScenePushConstants(const RenderContext::Impl& impl, const SceneView& view, const GraphicsSettings& settings) noexcept -> ScenePushConstants {
    return ScenePushConstants {
        .invViewProj = view.invViewProjMatrix,
        .viewProj    = view.viewProjMatrix,
        .camPos      = {view.worldPosition.GetX(), view.worldPosition.GetY(), view.worldPosition.GetZ(), view.time},
        .giMode      = settings.post.mode,
        .aoRadius    = settings.post.aoRadius,
        .aoBias      = settings.post.aoBias,
        .aoPower     = settings.post.aoPower,
        .giIntensity = settings.post.giIntensity,
        .giSamples   = settings.post.giSamples,
        .enableSSR   = settings.post.enableSSR,
        .enableRTR   = (impl.frames.tlas[impl.presenter.frameIndex] && settings.rayTracing.enableReflections) ? settings.post.enableRTR : 0,
        ._pad        = {},
    };
}

// The scene graph for one anti-aliasing configuration.
//
// The declaration order below is the order the passes run in, but the
// dependencies between them come from their declared usages, not from this
// list: `GBufferResolvePass` reads the HiZ pyramid, so it follows the base pass
// that fills the depth buffer and the pass that builds the pyramid from it; the
// reflection passes read the lighting target, so they follow the lighting
// pass. The engine proves all of that at compile time and emits the barriers;
// nothing here encodes a dependency twice.
template <AAMode Mode, typename GetSwapchainImageT>
[[nodiscard]] auto BuildFrameGraph(RenderContext::Impl& self, const ScenePushConstants& pc, GetSwapchainImageT&& getSwapchain) {
    using OpaqueReflectionPass = Passes::ReflectionPass<Passes::OpaqueSurface, Res_HdrSceneColor, "Reflection">;
    using TranslucentReflectionPass = Passes::ReflectionPass<Passes::TranslucentSurface, Res_TransLighting, "TransReflection">;

    // 1. Rasterization and deferred lighting DAG.
    //
    // The designators are load-bearing, not decoration. Every pass inherits
    // `Vk::RenderPass<...>`, which is an aggregate with no data members, so a
    // positional `{self}` would be read as an initializer for that base rather
    // than for the pass's own `impl`. Naming the member leaves the base to be
    // default-initialized and keeps the list below readable as what it is: one
    // pass per line, in the order they run.
    auto core = Vk::MakePassPack(
        Passes::ShadowPass {.impl = self}, Passes::GBufferBasePass {.impl = self}, Passes::HiZGeneratePass {.impl = self},
        Passes::GBufferResolvePass {.impl = self}, Passes::DecalPass {.impl = self}, Passes::ViewmodelPass {.impl = self},
        Passes::TranslucentPrePass {.impl = self}, Passes::GtaoPass {.impl = self, .pc = pc}, Passes::ClusteredLightingPass {.impl = self, .pc = pc},
        Passes::RtrHalfTracePass {.impl = self}, OpaqueReflectionPass {.impl = self, .pc = pc},
        TranslucentReflectionPass {.impl = self, .pc = pc}, Passes::OpaqueSceneCopyPass {.impl = self}, Passes::ForwardPass {.impl = self},
        Passes::HdrDenoisePass {.impl = self}, Passes::BloomPass {.impl = self}
    );

    // 2. Anti-aliasing tail. Which passes exist at all depends on the mode, so
    //    every AA setting is a different graph -- and therefore a different
    //    specialization of everything downstream, including which resource the
    //    tonemapper reads.
    auto aa = [&] {
        if constexpr (Mode == AAMode::TAA) {
            return Vk::MakePassPack(Passes::TAAPass {.impl = self});
        } else if constexpr (Mode == AAMode::FXAA) {
            return Vk::MakePassPack(Passes::FXAAPass {.impl = self});
        } else if constexpr (Mode == AAMode::MLAA) {
            return Vk::MakePassPack(Passes::MLAAPass {.impl = self});
        } else if constexpr (Mode == AAMode::SMAA) {
            return Vk::MakePassPack(Passes::SmaaEdgePass {.impl = self}, Passes::SmaaWeightPass {.impl = self}, Passes::SmaaBlendPass {.impl = self});
        } else {
            return Vk::MakePassPack();
        }
    }();

    // 3. Tone-mapping and the blit onto this frame's presentation image.
    auto blit = Vk::MakePassPack(
        Passes::TonemapBlitPass<Mode, std::decay_t<GetSwapchainImageT>> {.impl = self, .getSwapchainImage = std::forward<GetSwapchainImageT>(getSwapchain)}
    );

    return (std::move(core) + std::move(aa) + std::move(blit)).BuildGraph();
}

template <AAMode Mode, typename GetSwapchainImageT>
void ExecuteSceneGraph(
    RenderContext::Impl&    self,
    VkCommandBuffer         cmd,
    const SceneView&        view,
    const GraphicsSettings& settings,
    GetSwapchainImageT&&    getSwapchain
) {
    auto graph = BuildFrameGraph<Mode>(self, AssembleScenePushConstants(self, view, settings), std::forward<GetSwapchainImageT>(getSwapchain));

    // Every resource a pass named is resolved once: explicit resolvers cover
    // non-bundle resources (swapchain and history) and the transmission
    // attachment's mip-0 view; the rest use reflected target metadata.
    typename decltype(graph)::Binder binder;
    binder.AutoBind(self);

    auto* diagnostics = self.gpuDiagnostics.IsActive() ? &self.gpuDiagnostics : nullptr;
    graph.Execute(cmd, binder, self.presenter.frameIndex, &self.gpuProfiler, diagnostics, self.ForkExecutor());
}

template <typename Self, typename GetSwapchainImageT>
void DispatchAAMode(Self& self, VkCommandBuffer cmd, AAMode mode, const SceneView& view, const GraphicsSettings& settings, GetSwapchainImageT&& getSwapchain) {
    Reflect::DispatchEnum(mode, [&]<AAMode Val>() { ExecuteSceneGraph<Val>(self, cmd, view, settings, std::forward<GetSwapchainImageT>(getSwapchain)); });
}

// The graph type for one anti-aliasing configuration. The visualization is a
// compile-time walk of the state table, so it needs the type and nothing else:
// no pass is constructed and no command buffer is involved, which is why
// `decltype` on an unevaluated call is enough to ask the engine what it worked
// out.
template <AAMode Mode>
using SceneGraphFor =
    decltype(BuildFrameGraph<Mode>(std::declval<RenderContext::Impl&>(), std::declval<const ScenePushConstants&>(), std::declval<SwapchainImage (*)()>()));

} // namespace

// The five dumps above are five separate compile-time walks of five state tables,
// and they are built by every build whether or not anything ever asks for one --
// a measurable slice of this translation unit's front-end time and of its peak
// memory. Configure with -DZHLN_GRAPH_VISUALIZE=ON to build them again; the strings
// are byte-for-byte the ones this used to produce, and with the switch off the
// `--print-graph` command still answers, it just says so.
#ifndef ZHLN_GRAPH_VISUALIZE
#define ZHLN_GRAPH_VISUALIZE 0
#endif

std::string_view GetRenderGraphDump(AAMode currentMode) noexcept {
#if ZHLN_GRAPH_VISUALIZE
    using enum AAMode;
    using Vk::Debug::GraphVisualizer;

    static constexpr auto vis_taa  = GraphVisualizer<SceneGraphFor<TAA>>::Visualize();
    static constexpr auto vis_smaa = GraphVisualizer<SceneGraphFor<SMAA>>::Visualize();
    static constexpr auto vis_fxaa = GraphVisualizer<SceneGraphFor<FXAA>>::Visualize();
    static constexpr auto vis_mlaa = GraphVisualizer<SceneGraphFor<MLAA>>::Visualize();
    static constexpr auto vis_none = GraphVisualizer<SceneGraphFor<None>>::Visualize();

    switch (currentMode) {
        case TAA:
            return vis_taa.string_view();
        case SMAA:
            return vis_smaa.string_view();
        case FXAA:
            return vis_fxaa.string_view();
        case MLAA:
            return vis_mlaa.string_view();
        case None:
            return vis_none.string_view();
    }
    return "Not implemented.";
#else
    (void) currentMode;
    return "Render graph visualization is compiled out: build with -DZHLN_GRAPH_VISUALIZE=ON to print the graph.";
#endif
}

void RenderContext::Impl::RecordSceneFrame(Vk::CommandBuffer<Vk::QueueType::Graphics> cmd, const SceneView& view, const GraphicsSettings& sceneSettings) {
    auto getSwapchainImage = [&]() -> SwapchainImage { return sceneTarget->Assume<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(); };

    DispatchAAMode(*this, cmd, sceneSettings.antiAliasing.mode, view, sceneSettings, getSwapchainImage);
}

} // namespace ZHLN
