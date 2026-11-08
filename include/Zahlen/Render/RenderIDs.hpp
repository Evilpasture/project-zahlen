// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <cstdint>
#include <type_traits>

namespace ZHLN {

// Naming taxonomy for the four concepts that used to share the word "Handle":
//
//   1. inert numeric key         -> *ID   (the enums/structs in this header)
//   2. non-owning domain facade  -> a noun (GraphicsDevice, Simulation, DebugDraw)
//   3. scoped begin/end token    -> *Scope (FrameScope, SessionScope, FrameTarget)
//   4. owning allocation         -> std::unique_ptr<T> / *Instance
//
// Nothing below owns GPU memory or has methods that act on a resource: these are
// opaque keys you hand *to* an API. The renderer resolves them to real objects.
//
// NOLINTBEGIN(performance-enum-size)
enum class TextureID : uint64_t { Invalid = 0 };
enum class BufferID : uint64_t { Invalid = 0 };
enum class PipelineID : uint64_t { Invalid = 0 };
// NOLINTEND(performance-enum-size)

static_assert(sizeof(BufferID) == 8);
static_assert(sizeof(PipelineID) == 8);
static_assert(sizeof(TextureID) == 8);

namespace SystemTextures {
inline constexpr TextureID Invalid    = TextureID(0);
inline constexpr TextureID Black      = TextureID(1);
inline constexpr TextureID White      = TextureID(2);
inline constexpr TextureID FlatNormal = TextureID(3);
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
    // The texture must be a render texture (CreateRenderTexture): it is the
    // same TextureID used for bindless sampling, with the attachment
    // side looked up in the render-texture registry at resolve time.
    [[nodiscard]] constexpr auto ForTexture(TextureID texture) const noexcept -> FrameTarget {
        if (!Valid() || texture == TextureID::Invalid) {
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
    TextureID          _texture     = TextureID::Invalid;
};

static_assert(std::is_trivially_copyable_v<FrameTarget>);

}
