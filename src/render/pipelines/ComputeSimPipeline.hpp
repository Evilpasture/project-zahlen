// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "render/RenderInternal.hpp"
#include <render/Render.hpp>
#include <Zahlen/Render/View.hpp>

namespace ZHLN::Pipelines {

struct ComputeSimPipeline {
    [[nodiscard]] static auto Submit(RenderContext::Impl& impl, float dt) noexcept -> RenderResult;
};

}
