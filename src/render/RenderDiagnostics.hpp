// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "RenderConstants.hpp"
#include "Rendering.hpp"
#include "ShaderReloadRegistry.hpp"
#include "diagnostics/GPUDiagnostics.hpp"
#include <Zahlen/Config.hpp>
#include <Zahlen/Core/Optional.hpp>
#include <Zahlen/FileSystem/FileWatcher.hpp>
#include <Zahlen/Render/PipelineStats.hpp>
#include <atomic>
#include <cstdint>
#include <string_view>

namespace ZHLN {

struct RenderDiagnostics {
    RenderDiagnostics(Vk::Context& device, Optional<FS::FileSystemWatcher&> watcher) noexcept: ctx(device), fileSystemWatcher(watcher) {
    }

    Vk::Context& ctx;

    FrameProfiler      gpuProfiler;
    Vk::GPUDiagnostics gpuDiagnostics;

    GpuPipelineCounters pendingPipelineCounters {};

    Optional<FS::FileSystemWatcher&> fileSystemWatcher;
    FS::FileWatchHandle              shaderDirectoryWatch = 0;
    ShaderReloadRegistry             shaderReloads;

    std::atomic<uint32_t> invalidDrawWarningCount {0};
    bool                  warnedUnwrittenTarget = false;

    void BeginShaderObservation();
    void HandleShaderFileEvent(const FS::FileWatchEvent& event);

    void StopShaderObservation() noexcept;
};

} // namespace ZHLN
