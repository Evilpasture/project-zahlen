// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "PassPayloads.hpp"
#include "Rendering.hpp"
#include "pipeline/ComputePass.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN {

// The compute pipelines that drive ambient occlusion and Radiance Cascades
// Global Illumination (RCGI). Each field bundles the pipeline and its
// reflected descriptor-heap bindings together.
struct LightingResources {
    Vk::BoundComputePass<Vk::TypedComputePass<Shaders::Modules::GtaoCS, GtaoPushConstants>> gtao;
    Vk::BoundComputePass<Vk::DynamicComputePass>                                            rcgiProbeSetup;
    Vk::BoundComputePass<Vk::DynamicComputePass>                                            rcgiCascadeTraceMerge;
    Vk::BoundComputePass<Vk::DynamicComputePass>                                            rcgiResolve;
};

} // namespace ZHLN
