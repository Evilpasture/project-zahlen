// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Types.hpp>
#include <Zahlen/gui/GUI.hpp>
#include <Jolt/Math/Vec4.h>
#include <cstdint>
#include <string>

namespace ZHLN::GUI {

auto AppendTextVertices(
    VertexPosition*    outPos,
    VertexAttributes*  outAttr,
    const FontAtlas&   font,
    const std::string& text,
    float              x,
    float              y,
    float              scale,
    const JPH::Vec4&   color
) -> uint32_t;

} // namespace ZHLN::GUI
