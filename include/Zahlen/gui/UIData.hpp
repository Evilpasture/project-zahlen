// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <Zahlen/Geometry2D.hpp>
#include <Zahlen/Render/Handles.hpp>
#include <Zahlen/Vertex.hpp>
#include <array>
#include <cstddef>
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
    std::span<const UIBatch>        batches;
    std::span<const VertexPosition> positions;
    std::span<const VertexSurface>  surfaces;
    bool                            wantsCaptureMouse    = false;
    bool                            wantsCaptureKeyboard = false;

    [[nodiscard]] constexpr bool Empty() const noexcept {
        return batches.empty() || positions.empty() || surfaces.empty();
    }
};

// Stack-resident software cursor dot (dark outline disc + bright white fill)
// for environments without an OS compositor cursor plane (Linux KMS/DRM TTY).
class SoftwareCursorDot {
  public:
    static constexpr size_t kSegments    = 16;
    static constexpr size_t kVertexCount = kSegments * 6;

    constexpr SoftwareCursorDot(float x, float y, float innerRadius = 3.5f, float outlineRadius = 5.5f) noexcept {
        constexpr std::array<std::array<float, 2>, kSegments> kUnitCircle {{
            {1.0000000f, 0.0000000f},
            {0.9238795f, 0.3826834f},
            {0.7071068f, 0.7071068f},
            {0.3826834f, 0.9238795f},
            {0.0000000f, 1.0000000f},
            {-0.3826834f, 0.9238795f},
            {-0.7071068f, 0.7071068f},
            {-0.9238795f, 0.3826834f},
            {-1.0000000f, 0.0000000f},
            {-0.9238795f, -0.3826834f},
            {-0.7071068f, -0.7071068f},
            {-0.3826834f, -0.9238795f},
            {0.0000000f, -1.0000000f},
            {0.3826834f, -0.9238795f},
            {0.7071068f, -0.7071068f},
            {0.9238795f, -0.3826834f},
        }};

        constexpr PackedRGBA8 kOutlineColor {.data = 0xE6140E0Eu};
        constexpr PackedRGBA8 kInnerColor {.data = 0xFAFFFFFFu};

        size_t cursor = 0;
        auto   emitDisc = [&](float radius, PackedRGBA8 color) noexcept {
            const VertexSurface surface {.uv = {.data = 0u}, .color = color, .uv1 = {.data = 0u}};
            for (size_t i = 0; i < kSegments; ++i) {
                const size_t next = (i + 1) % kSegments;
                _positions[cursor]     = {{x, y, 0.0f}};
                _positions[cursor + 1] = {{x + kUnitCircle[i][0] * radius, y + kUnitCircle[i][1] * radius, 0.0f}};
                _positions[cursor + 2] = {{x + kUnitCircle[next][0] * radius, y + kUnitCircle[next][1] * radius, 0.0f}};
                _surfaces[cursor]      = surface;
                _surfaces[cursor + 1]  = surface;
                _surfaces[cursor + 2]  = surface;
                cursor += 3;
            }
        };

        emitDisc(outlineRadius, kOutlineColor);
        emitDisc(innerRadius, kInnerColor);

        _batches[0] = UIBatch {
            .texture     = TextureHandle::Invalid,
            .vertexStart = 0,
            .vertexCount = static_cast<uint32_t>(kVertexCount),
            .useScissor  = false,
            .isSDF       = false,
        };
    }

    SoftwareCursorDot(const SoftwareCursorDot&)                    = delete;
    auto operator=(const SoftwareCursorDot&) -> SoftwareCursorDot& = delete;
    SoftwareCursorDot(SoftwareCursorDot&&)                         = delete;
    auto operator=(SoftwareCursorDot&&) -> SoftwareCursorDot&      = delete;
    ~SoftwareCursorDot() noexcept                                  = default;

    [[nodiscard]] constexpr auto DrawData() const noexcept -> UIDrawData {
        return UIDrawData {
            .batches   = _batches,
            .positions = _positions,
            .surfaces  = _surfaces,
        };
    }

  private:
    std::array<UIBatch, 1>                     _batches {};
    std::array<VertexPosition, kVertexCount>   _positions {};
    std::array<VertexSurface, kVertexCount>    _surfaces {};
};

}
