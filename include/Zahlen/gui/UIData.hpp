// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/gui/UIData.hpp
//
// The 2D geometry payload the GUI hands to the renderer: batches of quads plus
// the vertex streams they index. It lives here, on the producing side, for the
// reason the payload exists at all -- the GUI describes quads and the renderer
// draws them, so the GUI must be able to spell its own output without including
// the renderer's machinery. Nothing in this header is renderer-specific: a
// batch names a texture handle and a vertex range, and that is the whole
// contract between the two subsystems.
//
// Dependency-light on purpose: the handles come from Render/Handles.hpp (a
// vocabulary header, no Jolt), the rect from Geometry2D.hpp and the vertices
// from Vertex.hpp.
#pragma once
#include <Zahlen/Geometry2D.hpp>    // ScissorRect
#include <Zahlen/Render/Handles.hpp> // TextureHandle
#include <Zahlen/Vertex.hpp>         // VertexPosition, VertexAttributes
#include <cstdint>
#include <span>

namespace ZHLN {

// One draw: a contiguous vertex range, the texture it samples, and the state
// that cannot be expressed by the vertex data itself.
struct UIBatch {
    TextureHandle texture              = TextureHandle::Invalid;
    uint32_t      bindlessTextureIndex = 0; // Non-zero bypasses TextureManager lookup (user bindless IDs)
    uint32_t      vertexStart          = 0;
    uint32_t      vertexCount          = 0;
    bool          useScissor           = false;
    bool          isSDF                = false;
    bool          useTextureColor      = false;
    ScissorRect   scissorRect          = {};
};

// Immutable 2D UI geometry payload extracted from Clay by
// `GUI::Context::EndFrame`. Plain data: the GUI subsystem neither knows nor
// inherits from the renderer, it just describes quads.
//
// The spans point into storage the producing GUI context owns until its next
// frame, so the payload must be handed to `RenderContext::RenderUI` in the
// same frame it was built.
struct UIDrawData {
    std::span<const UIBatch>          batches;
    std::span<const VertexPosition>   positions;
    std::span<const VertexAttributes> attributes;

    [[nodiscard]] constexpr bool Empty() const noexcept {
        return batches.empty() || positions.empty() || attributes.empty();
    }
};

} // namespace ZHLN
