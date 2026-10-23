// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "passes/gbuffer/GBufferBasePass.hpp"

namespace ZHLN {

using GBufferPassAttachments = Vk::PassAttachmentFormats<Passes::GBufferBasePass>;

// Pipeline formats are derived from the renderer's pass contract, rather than
// maintained as a second renderer-specific list in the Vulkan target layer.
struct ActiveGBuffer {
    static constexpr size_t count = GBufferPassAttachments::color_count;
    static constexpr auto   array = GBufferPassAttachments::color_formats;
};

} // namespace ZHLN
