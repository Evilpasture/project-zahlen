// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "GpuLayout.hpp"
#include "Rendering.hpp"
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Render/GpuEnums.hpp>
#include <cstdint>
#include <span>

namespace ZHLN {

// The shadow atlas: one 2D depth image that every shadow-casting spot
// and point light sub-allocates square tiles out of.
//
// This replaced a `TextureCubeArray` of 24 layers reserved for four lights. The
// array was a hard budget -- a fifth shadowed light simply had no shadows -- and
// the slot assignment that decided which four got them thrashed frame to frame,
// which is what the score hysteresis in `LightingSystem` was patched to hide.
// An atlas has neither problem: the lights that ask are served in ranked order
// until the atlas is full, and a light that falls behind loses tile
// *resolution* rather than its shadows.
//
// A spot light is one tile. A point light is six -- its cube faces, in Vulkan
// layer order (+X,-X,+Y,-Y,+Z,-Z) -- laid out consecutively, so a light's tile
// base plus a face index addresses any of them. A spot with no authored cone is
// lit as a full sphere by `lighting.slang`, so it gets six tiles too: one would
// only cover the front of it.
//
// Tiles are cached per light across frames. A light whose tiles land in the
// same place with the same matrices did not move, so its depth is still valid
// and the pass can skip re-rasterizing it -- provided nothing that casts into
// it moved either, which only the pass can know.
class ShadowAtlas {
  public:
    static constexpr uint32_t kSize       = 4096; // texels, square: 64 MB of D32
    static constexpr uint32_t kMaxTiles   = 512;
    // Lights served per frame. The pass gives each one a slot in the indirect
    // command buffer, so this is the shape of that buffer too; the cap that
    // matters is kMaxLights * kPointFaces <= kMaxTiles, which 64 * 6 = 384
    // satisfies with room for the spots' single tiles.
    static constexpr uint32_t kMaxLights  = 64;
    static constexpr uint32_t kMinTile    = 128;
    static constexpr uint32_t kMaxTile    = 1024;
    static constexpr uint32_t kPointFaces = 6;

    // One light's ask. `faces` holds `faceCount` world-to-clip matrices in the
    // order the tiles are laid out; `MakeFaceMatrices` builds them.
    struct Request {
        uint64_t   key        = 0; // stable identity, see MakeLightKey
        int32_t    lightIndex = -1; // the light's index in this frame's list
        uint32_t   faceCount  = 1;
        uint32_t   tile       = kMinTile;
        JPH::Mat44 faces[kPointFaces] {};
    };

    struct Grant {
        int32_t tileBase = -1; // index of the first tile, or -1 when refused
        // True when this light held exactly these tiles, with exactly these
        // matrices, last frame: its depth is still valid and re-rasterizing it
        // would write the same texels.
        bool reuse = false;
    };

    // What the pass records per light: which tiles to draw into and whether it
    // can skip doing so.
    struct Placement {
        int32_t  lightIndex = -1;
        int32_t  tileBase   = -1;
        uint32_t faceCount  = 0;
        bool     cached     = false;
    };

    void BeginFrame() noexcept;

    // Hands out `req.faceCount` tiles, or nothing, and records the placement
    // for the pass either way. Refusal is not an error: it means the atlas is
    // full, and the light renders without shadows.
    [[nodiscard]] auto Acquire(const Request& req) noexcept -> Grant;

    void EndFrame() noexcept;

    // The packed tiles for this frame's upload, and the pass's draw list.
    [[nodiscard]] auto Tiles() noexcept -> std::span<ShadowTile> { return std::span {_tiles.data(), _tiles.size()}; }
    [[nodiscard]] auto TileCount() const noexcept -> uint32_t { return static_cast<uint32_t>(_tiles.size()); }
    [[nodiscard]] auto Placements() const noexcept -> std::span<const Placement> { return _placements; }
    // A tile's rectangle in texels, which is the viewport and scissor the pass
    // rasterizes it with. `Tiles()` stores the same rectangle as atlas UV; both
    // directions are exact because the atlas edge is a power of two and every
    // tile is aligned to its own size.
    struct TileRect {
        uint32_t x    = 0;
        uint32_t y    = 0;
        uint32_t size = 0;
    };
    [[nodiscard]] static auto TileRectOf(const ShadowTile& tile) noexcept -> TileRect;

    // The light-space transforms for a point light's six cube faces, or a spot
    // light's one cone. `dir` and the cone angles are the light's own; a spot
    // with no authored cone gets the six-face treatment its lighting needs.
    [[nodiscard]] static auto FaceCount(bool isPoint, float outerConeRadians) noexcept -> uint32_t;
    [[nodiscard]] static auto MakeFaceMatrices(
        const JPH::Vec3& position, const JPH::Vec3& direction, float range, bool isPoint, float outerConeRadians, JPH::Mat44* out
    ) noexcept -> uint32_t;
    // Stable identity for the cache: the light's own parameters, so a light
    // keeps its tiles across a reordering of the scene's light list.
    [[nodiscard]] static auto MakeLightKey(LightType type, const JPH::Vec3& position, float intensity, float range) noexcept -> uint64_t;
    // Distance-adaptive tile edge. Near lights get more texels per metre; the
    // step-down is what makes falling behind the ranking a soft landing.
    [[nodiscard]] static auto TileForDistance(float distance) noexcept -> uint32_t;

  private:
    // A row of same-sized tiles packed left to right. One chain per (size,
    // row), opened on demand, so a frame that only asked for 256s never pays
    // for 1024-sized fragmentation.
    struct Shelf {
        uint32_t tile   = 0;
        uint32_t y      = 0;
        uint32_t cursor = 0;
    };
    struct CacheEntry {
        uint64_t   key       = 0;
        uint32_t   faceCount = 0;
        uint32_t   tile      = 0;
        uint32_t   x         = 0;
        uint32_t   y         = 0;
        JPH::Mat44 faces[kPointFaces] {};
        bool       seen = false;
    };

    [[nodiscard]] auto Place(uint32_t faceCount, uint32_t tile, uint32_t& outX, uint32_t& outY) noexcept -> bool;

    ZHLN::Array<Shelf>       _shelves;
    ZHLN::Array<CacheEntry>  _cache;
    ZHLN::Array<ShadowTile>  _tiles;
    ZHLN::Array<Placement>   _placements;
};

}
