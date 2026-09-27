// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <cstdint>

namespace ZHLN {

// NOLINTBEGIN(performance-enum-size)
enum class TextureHandle : uint64_t { Invalid = 0 };
enum class BufferHandle : uint64_t { Invalid = 0 };
enum class PipelineHandle : uint64_t { Invalid = 0 };
enum class ResourceGroupHandle : uint64_t { Invalid = 0 };
// NOLINTEND(performance-enum-size)

static_assert(sizeof(BufferHandle) == 8);
static_assert(sizeof(PipelineHandle) == 8);
static_assert(sizeof(ResourceGroupHandle) == 8);
static_assert(sizeof(TextureHandle) == 8);

namespace SystemTextures {
inline constexpr TextureHandle Invalid    = TextureHandle(0);
inline constexpr TextureHandle Black      = TextureHandle(1);
inline constexpr TextureHandle White      = TextureHandle(2);
inline constexpr TextureHandle FlatNormal = TextureHandle(3);
}

struct RenderAttachment {
    TextureHandle texture    = TextureHandle::Invalid;
    uint16_t      mipLevel   = 0;
    uint16_t      arrayLayer = 0;

    [[nodiscard]] constexpr bool Valid() const noexcept {
        return texture != TextureHandle::Invalid;
    }

    explicit constexpr operator bool() const noexcept {
        return Valid();
    }
};

static_assert(sizeof(RenderAttachment) == 16);

}
