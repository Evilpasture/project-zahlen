// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/system/TextureSystem.hpp"
#include <Zahlen/Engine.hpp>
#include <Zahlen/Render.hpp>

namespace ZHLN {

void TextureSystem::Update(Engine& /*engine*/, float /*dt*/) {
    // Space reserved for async texture streaming / mip-fading
}

uint32_t TextureSystem::ResolveIndex(Engine& engine, TextureHandle handle) noexcept {
    return engine.GetRenderContext().GetBindlessIndex(handle);
}

} // namespace ZHLN
