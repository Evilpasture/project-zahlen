// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once
#include "../RenderInternal.hpp"
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Render/View.hpp>

namespace ZHLN::Pipelines {

struct UIPipeline {
    [[nodiscard]] static auto Execute(RenderContext::Impl& impl, const UIView& view, const UIDrawData& uiData) noexcept -> FrameOutcome<FrameSkipped>;
};

}
