// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Precompiled header for zahlen_render (opt-in via ZHLN_ENABLE_PCH).
// Intentionally excludes generated ShaderBindings.hpp and GeneratedGpuTypes.hpp
// so shader edits never invalidate the precompiled header.
#include "vulkan/Rendering.hpp"
#include "render/RenderTargetTags.hpp"
#include "vulkan/graph/RenderPassDecl.hpp"
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Render/GraphicsSettings.hpp>
#include <Zahlen/Log.hpp>
