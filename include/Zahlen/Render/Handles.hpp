// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <cstdint>
#include <type_traits>

namespace ZHLN {

// NOLINTBEGIN(performance-enum-size)
enum class TextureHandle : uint64_t { Invalid = 0 };
enum class RenderTextureHandle : uint64_t { Invalid = 0 };
enum class BufferHandle : uint64_t { Invalid = 0 };
enum class PipelineHandle : uint64_t { Invalid = 0 };
enum class ResourceGroupHandle : uint64_t { Invalid = 0 };
// NOLINTEND(performance-enum-size)

static_assert(sizeof(BufferHandle) == 8);
static_assert(sizeof(PipelineHandle) == 8);
static_assert(sizeof(ResourceGroupHandle) == 8);
static_assert(sizeof(TextureHandle) == 8);
static_assert(sizeof(RenderTextureHandle) == 8);

namespace SystemTextures {
inline constexpr TextureHandle Invalid    = TextureHandle(0);
inline constexpr TextureHandle Black      = TextureHandle(1);
inline constexpr TextureHandle White      = TextureHandle(2);
inline constexpr TextureHandle FlatNormal = TextureHandle(3);
}

class RenderContext;

// A non-owning capability for exactly one acquired presentation image and its
// command stream. Copy it within that frame; EndFrame, ReleaseTarget, a rebuild,
// or renderer destruction invalidates it. Rendering checks the identity rather
// than trying to turn a stale texture-shaped integer into a different target.
class FrameTarget {
  public:
    constexpr FrameTarget() noexcept = default;

    [[nodiscard]] constexpr auto Valid() const noexcept -> bool {
        return _renderer != 0 && _frame != 0 && _window != 0 && _acquisition != 0;
    }
    explicit constexpr operator bool() const noexcept { return Valid(); }

    // An offscreen image still records into this frame's acquired command
    // stream. It cannot be passed as a frame target without that association.
    [[nodiscard]] constexpr auto ForTexture(RenderTextureHandle texture) const noexcept -> FrameTarget {
        if (!Valid() || texture == RenderTextureHandle::Invalid) {
            return {};
        }
        FrameTarget result = *this;
        result._texture = texture;
        return result;
    }

    friend constexpr auto operator==(const FrameTarget&, const FrameTarget&) noexcept -> bool = default;

  private:
    friend class RenderContext;

    constexpr FrameTarget(uint64_t renderer, uint64_t frame, uint64_t window, uint64_t acquisition) noexcept:
        _renderer(renderer), _frame(frame), _window(window), _acquisition(acquisition) {}

    uint64_t            _renderer    = 0;
    uint64_t            _frame       = 0;
    uint64_t            _window      = 0;
    uint64_t            _acquisition = 0;
    RenderTextureHandle _texture     = RenderTextureHandle::Invalid;
};

static_assert(std::is_trivially_copyable_v<FrameTarget>);

}
