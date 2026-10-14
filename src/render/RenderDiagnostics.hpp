// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/RenderDiagnostics.hpp
//
// The profiler, the Vulkan diagnostic layer, the shader hot-reload registry and
// the two counters that throttle one-shot warnings. All of it was parked on
// RenderContext::Impl, and nothing outside the renderer's own .cpp files ever
// reached for it -- no pass sees any of this, which is what makes it a safe
// first cut of step 4 in rendercontext-impl-decomposition.md.
//
// One thing here is not self-contained: rebuilding a shader has to idle the
// device first, so HandleShaderFileEvent needs Vk::Context. It is handed one at
// construction -- RenderFrameContext::ctx, which is a base of Impl and so is
// alive before any Impl member is built.
//
// Definitions live in RenderResources.cpp, next to the two functions they were
// extracted from. The source list in src/render/CMakeLists.txt is explicit, so
// this is deliberately header-only rather than a new translation unit.

#pragma once

#include "RenderConstants.hpp" // FrameProfiler
#include "ShaderReloadRegistry.hpp"

#include "Rendering.hpp" // Vk::Context
#include "diagnostics/GPUDiagnostics.hpp"

#include <Zahlen/Config.hpp>              // isDev
#include <Zahlen/Core/Optional.hpp>
#include <Zahlen/FileSystem/FileWatcher.hpp>
#include <Zahlen/Render/PipelineStats.hpp> // GpuPipelineCounters

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

    // Throttles for two warnings that would otherwise repeat per draw or per
    // frame: a draw dropped for an invalid instance (RenderDrawCommands.cpp) and
    // a target the graph left unwritten (RenderPresentation.cpp).
    std::atomic<uint32_t> invalidDrawWarningCount {0};
    bool                  warnedUnwrittenTarget = false;

    // Watch resources/shaders and rebuild on change. Both bodies are no-ops
    // outside a dev build.
    void BeginShaderObservation();
    void HandleShaderFileEvent(const FS::FileWatchEvent& event);

    // Undoes BeginShaderObservation. ~Impl() calls this before the members above
    // are destroyed.
    void StopShaderObservation() noexcept;
};

} // namespace ZHLN
