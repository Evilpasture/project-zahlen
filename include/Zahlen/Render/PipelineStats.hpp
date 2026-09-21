// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Render/PipelineStats.hpp
//
// The counter block a scoped capture accumulates. On its own because it is plain
// data with no dependencies, so a caller that only reads counters does not pull
// the renderer's dispatch surface in to do it.
#pragma once
#include <cstdint>

namespace ZHLN {

// GPU pipeline counters summed over every profiled pass of the captured frames
// (hardware VK_QUERY_TYPE_PIPELINE_STATISTICS; see CapturePipelineStats). Counters the
// device does not support stay 0.
//
// The ratios this exists to measure -- clipping: 1 - clipperPrimitivesOut /
// clipperInvocations; meshlet culling: meshInvocations against the meshlets the scene
// issued.
struct GpuPipelineCounters {
    uint64_t iaPrimitives         = 0;
    uint64_t vsInvocations        = 0;
    uint64_t clipperInvocations   = 0; // primitives fed to the clipper
    uint64_t clipperPrimitivesOut = 0; // primitives that survived clipping
    uint64_t gsInvocations        = 0;
    uint64_t gsPrimitives         = 0;
    uint64_t fsInvocations        = 0;
    uint64_t csInvocations        = 0;
    uint64_t taskInvocations      = 0; // task workgroups launched (needs mesh shading)
    uint64_t meshInvocations      = 0; // mesh workgroups executed post-culling
};

} // namespace ZHLN
