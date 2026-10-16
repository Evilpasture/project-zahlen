// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "FrameContext.hpp"
#include "graph/RenderGraph.hpp"
#include "passes/FullscreenPassRecorder.hpp"
#include "passes/GraphPassUtils.hpp"

namespace ZHLN::Passes {

// One fullscreen raster pass: bind a FullscreenPassRecorder pipeline, build
// the push payload and slot writes through the prepare callable, record. The
// AA wrappers each differed from this only in their push struct, their one
// texture slot and their usage list, so they are instantiations of this now.
// PrepareFn is spelled explicitly (the graph pack cannot name a closure type
// at the call site), which is why the call sites pass a named function
// pointer and decltype(...).
template <
    Vk::ResourceName PassName,
    typename ShaderDecl,
    typename ShaderModule,
    typename OutRes,
    typename PushT,
    typename PrepareFn,
    typename... InRes>
struct FullscreenPass: Vk::RenderPass<PassName, Vk::ShaderRead<InRes>..., Vk::ColorWrite<OutRes>> {
    static_assert(TemplatedDetail::kNameProfilesAsStage<PassName>, "the pass name has no Stage enumerator, so the GPU profiler would never time it");

    RenderFrameContext&                     frame;
    Vk::FullscreenPass<Vk::ReflectedLayout>& pipeline;
    PrepareFn                               prepare;

    void operator()(Vk::RasterPassContextBase& ctx) const noexcept {
        FullscreenPassRecorder<ShaderDecl, ShaderModule>::Record(frame, ctx.Cmd(), pipeline, [&]() { return prepare(frame); });
    }
};

} // namespace ZHLN::Passes
