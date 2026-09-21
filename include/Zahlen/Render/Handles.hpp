// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Render/Handles.hpp
//
// The renderer's opaque vocabulary, and the whole of it that anything outside
// src/render needs to name: four 64-bit handles, the subresource reference that
// addresses a drawable target, and the bindless slot constants behind the
// engine's built-in textures.
//
// Deliberately its own header, and deliberately empty of everything else. These
// types are spelled by the GUI, the meshlet cooker, the asset pipeline and the
// ECS, and none of those should pull Jolt in to name a texture: Render/Types.hpp
// is the header that knows about materials and transforms, this one is only a
// vocabulary. Keep it dependency-free -- if it ever needs an include beyond
// <cstdint>, the type it needs belongs in Render/Types.hpp instead.
#pragma once
#include <cstdint>

namespace ZHLN {

// NOLINTBEGIN(performance-enum-size)
// Each handle is opaque: its value is minted and interpreted inside src/render,
// and no caller may index, compare for order, or arithmetic on one. The
// underlying uint64_t is what lets a handle be stored in a GPU constant buffer
// and hashed as an identity key without a translation table.
enum class TextureHandle : uint64_t { Invalid = 0 };
enum class BufferHandle : uint64_t { Invalid = 0 };
enum class PipelineHandle : uint64_t { Invalid = 0 };
enum class ResourceGroupHandle : uint64_t { Invalid = 0 };
// NOLINTEND(performance-enum-size)

static_assert(sizeof(BufferHandle) == 8);
static_assert(sizeof(PipelineHandle) == 8);
static_assert(sizeof(ResourceGroupHandle) == 8);
static_assert(sizeof(TextureHandle) == 8);

// NOTE: these are BINDLESS SLOT indices conceptually, but they are NOT valid
// TextureHandles. TextureHandle keys are hashed asset ids
// (TextureManager::RegisterUploaded -> HashAssetID), and the fallback slots are
// registered separately (RenderInternal.hpp: black 0, white 1, flat normal 2).
// Passing SystemTextures::White therefore logs "[Warning] TextureHandle 0x2 was
// not found in registry" and falls back to white anyway. Use
// TextureHandle::Invalid when you want the white fallback texture.
namespace SystemTextures {
inline constexpr TextureHandle Invalid    = TextureHandle(0);
inline constexpr TextureHandle Black      = TextureHandle(1);
inline constexpr TextureHandle White      = TextureHandle(2);
inline constexpr TextureHandle FlatNormal = TextureHandle(3);
} // namespace SystemTextures

// Universal subresource reference to any renderable GPU target. Fully
// identifies a swapchain backbuffer, an offscreen texture, a cubemap face or a
// mip level, so a caller never has to say *what kind* of target it is asking
// for: it addresses a subresource and the renderer resolves it.
//
// The handle stays opaque. What the texture physically *is* -- extent, format,
// layer count -- is a property of its allocation inside src/vulkan, never a
// mirrored public enum, so adding a new destination (OpenXR eye, cubemap probe
// face, portal) needs no enumeration of view kinds here.
struct RenderAttachment {
    TextureHandle texture    = TextureHandle::Invalid;
    uint16_t      mipLevel   = 0;
    uint16_t      arrayLayer = 0; // Cubemap face (0..5) or texture array slice

    [[nodiscard]] constexpr bool Valid() const noexcept {
        return texture != TextureHandle::Invalid;
    }

    explicit constexpr operator bool() const noexcept {
        return Valid();
    }
};

static_assert(sizeof(RenderAttachment) == 16);

} // namespace ZHLN
