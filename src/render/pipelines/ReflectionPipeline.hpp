// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "Rendering.hpp"
#include "graph/RenderPassDecl.hpp"
#include "pipeline/FullscreenPass.hpp"
#include <GeneratedGpuTypes.hpp>

namespace ZHLN::Passes {
struct GBufferSurface;
}

namespace ZHLN {

struct RenderFrame;

// A renderer-owned pass bundle with no per-frame state of its own: the frame it
// dispatches for is an argument, so this class cannot hold a stale view of one.
class ReflectionPipeline {
public:
  ReflectionPipeline() noexcept = default;

  Vk::FullscreenPass<Vk::ReflectedLayout> pass;

  void Dispatch(RenderFrame& frame, Vk::RasterPassContextBase& ctx, const Passes::GBufferSurface& surface, const Slang::ScenePassPushConstants& pc)
      const noexcept;
};

}
