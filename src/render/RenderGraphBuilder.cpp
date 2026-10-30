// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RenderGraphBuild.hpp"
#include "Zahlen/Math/Math3D.hpp"
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <Zahlen/Core/Reflection/Structs.hpp>
#include <Zahlen/Render/RenderGraphDump.hpp>
#include <string_view>
#include <utility>

namespace ZHLN {

namespace Vk {

template <>
struct ResourceResolver<Res_Depth> {
    [[nodiscard]] static constexpr auto Resolve(RenderContext::Impl& impl) noexcept {
        return MakeRef<Res_Depth>(impl.ActiveDepthTarget());
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

        return ImageSlice {target.image.Handle(), target.mipAttachmentViews[0], target.extent, Res_TransLighting::format};
    }
};

template <>
struct ResourceResolver<Res_LightingGI> {
    [[nodiscard]] static auto Resolve(RenderContext::Impl& impl) noexcept -> ImageSlice {
        const auto& target = impl.graphResources.lightingGI;

        return ImageSlice {target.image.Handle(), target.mipAttachmentViews[0], target.extent, Res_LightingGI::format};
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

template <AAMode Mode>
void ExecuteSceneGraph(
    RenderContext::Impl&                self,
    VkCommandBuffer                     cmd,
    const SceneView&                    view,
    const GraphicsSettings&             settings,
    RenderGraphDetail::SwapchainImageFn getSwapchain
) {
    // The frame this graph records into: the slot it occupies in the
    // frames-in-flight storage, and the state it was settled with. It outlives
    // every pass below because it is a local of the recording, not a member of
    // the renderer.
    RenderFrame frame {self, self.presenter.frameIndex};

    auto graph = RenderGraphDetail::BuildFrameGraph<Mode>(self, frame, RenderGraphDetail::AssembleScenePushConstants(self, view, settings), getSwapchain);

    typename decltype(graph)::Binder binder;
    binder.AutoBind(self);

    auto* diagnostics = self.diagnostics.gpuDiagnostics.IsActive() ? &self.diagnostics.gpuDiagnostics : nullptr;
    graph.Execute(cmd, binder, self.presenter.frameIndex, &self.diagnostics.gpuProfiler, diagnostics, self.ForkExecutor());
}

void DispatchAAMode(
    RenderContext::Impl&                self,
    VkCommandBuffer                     cmd,
    AAMode                              mode,
    const SceneView&                    view,
    const GraphicsSettings&             settings,
    RenderGraphDetail::SwapchainImageFn getSwapchain
) {
    Reflect::DispatchEnum(mode, [&]<AAMode Val>() { ExecuteSceneGraph<Val>(self, cmd, view, settings, getSwapchain); });
}

} // namespace

void RenderContext::Impl::RecordSceneFrame(Vk::CommandBuffer<Vk::QueueType::Graphics> cmd, const SceneView& view, const GraphicsSettings& sceneSettings) {
    auto getSwapchainImage = [&]() -> RenderGraphDetail::SwapchainImage { return sceneTarget->Assume<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(); };

    DispatchAAMode(*this, cmd, sceneSettings.antiAliasing.mode, view, sceneSettings, getSwapchainImage);
}

auto GetRenderGraphDump(AAMode currentMode) noexcept -> std::string_view {
    using enum AAMode;
    using RenderGraphDetail::SceneGraphFor;
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
}

} // namespace ZHLN
