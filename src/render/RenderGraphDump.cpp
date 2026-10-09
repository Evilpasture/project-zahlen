// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Compile-time walk of the scene graph for `--print-graph`. Isolated from
// RenderGraphBuilder.cpp so GraphVisualizer's five specializations do not
// rebuild whenever execute/record changes, and vice versa.

#include "RenderGraphBuild.hpp"
#include <Zahlen/GraphicsSettings.hpp>
#include <string_view>

namespace ZHLN {

auto GetRenderGraphDump(AAMode currentMode) noexcept -> std::string_view {
    using enum AAMode;
    using Vk::Debug::GraphVisualizer;
    using RenderGraphDetail::SceneGraphFor;

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
