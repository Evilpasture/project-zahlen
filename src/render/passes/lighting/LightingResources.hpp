// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "PassPayloads.hpp"
#include "Rendering.hpp"
#include "pipeline/ComputePass.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN {

// The pipelines that light the scene through rays and screen-space taps: direct
// lighting shadows, reflections, diffuse GI, and the DDGI probe update, plus the
// à-trous chains that denoise them.
//
// These are lighting resources, not post-processing ones. They were stored in
// PostProcessFeature because that class was the only compute-pass bag on the
// frame, which meant the lighting passes had to ask a post-process object for
// their pipelines and then ask it again for the bindings that matched them.
// Each field here bundles both halves, so one question gets both answers.
struct LightingResources {
    Vk::BoundComputePass<Vk::TypedComputePass<Shaders::Modules::GtaoCS, GtaoPushConstants>>                       gtao;
    Vk::BoundComputePass<Vk::TypedComputePass<Shaders::Modules::RtShadowCS, RTShadowPushConstants>>               rtShadow;
    Vk::BoundComputePass<Vk::DynamicComputePass>                                                                  shadowDenoise;
    Vk::BoundComputePass<Vk::DynamicComputePass>                                                                  rcgiProbeSetup;
    Vk::BoundComputePass<Vk::DynamicComputePass>                                                                  rcgiCascadeTraceMerge;
    Vk::BoundComputePass<Vk::DynamicComputePass>                                                                  rcgiResolve;
    Vk::BoundComputePass<Vk::DynamicComputePass>                                                                  rtgiComposite;
    Vk::BoundComputePass<Vk::TypedComputePass<Shaders::Modules::DdgiTraceCS, DdgiTracePushConstants>>             ddgiTrace;
    Vk::BoundComputePass<Vk::TypedComputePass<Shaders::Modules::DdgiUpdateIrradianceCS, DdgiUpdatePushConstants>> ddgiUpdate;
};

} // namespace ZHLN
