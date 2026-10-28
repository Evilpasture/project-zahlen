// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "RenderConstants.hpp"
#include "Rendering.hpp"
#include "ShaderReloadRegistry.hpp"
#include "diagnostics/GPUDiagnostics.hpp"
#include <Zahlen/Config.hpp>
#include <Zahlen/Render/PipelineStats.hpp>
#include <atomic>
#include <cstdint>
#include <string_view>

namespace ZHLN {

struct RenderDiagnostics {
    explicit RenderDiagnostics(Vk::Context& device) noexcept: ctx(device) {
    }

    Vk::Context& ctx;

    FrameProfiler      gpuProfiler;
    Vk::GPUDiagnostics gpuDiagnostics;

    GpuPipelineCounters pendingPipelineCounters {};

    ShaderReloadRegistry shaderReloads;

    std::atomic<uint32_t> invalidDrawWarningCount {0};
    bool                  warnedUnwrittenTarget = false;

    void NotifyShaderChanged(std::string_view changedPath);
};

} // namespace ZHLN
