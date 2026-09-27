// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <Zahlen/Geometry2D.hpp>
#include <Zahlen/Render/Handles.hpp>
#include <Zahlen/Vertex.hpp>
#include <cstdint>
#include <span>

namespace ZHLN {

struct UIBatch {
    TextureHandle texture              = TextureHandle::Invalid;
    uint32_t      bindlessTextureIndex = 0;
    uint32_t      vertexStart          = 0;
    uint32_t      vertexCount          = 0;
    bool          useScissor           = false;
    bool          isSDF                = false;
    bool          useTextureColor      = false;
    ScissorRect   scissorRect          = {};
};

struct UIDrawData {
    std::span<const UIBatch>          batches;
    std::span<const VertexPosition>   positions;
    std::span<const VertexAttributes> attributes;

    [[nodiscard]] constexpr bool Empty() const noexcept {
        return batches.empty() || positions.empty() || attributes.empty();
    }
};

}
