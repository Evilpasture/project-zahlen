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

class ShadowAtlas {
  public:
    static constexpr uint32_t kSize       = 4096;
    static constexpr uint32_t kMaxTiles   = 512;

    static constexpr uint32_t kMaxLights  = 64;
    static constexpr uint32_t kMinTile    = 128;
    static constexpr uint32_t kMaxTile    = 1024;
    static constexpr uint32_t kPointFaces = 6;

    struct Request {
        uint64_t   key        = 0;
        int32_t    lightIndex = -1;
        uint32_t   faceCount  = 1;
        uint32_t   tile       = kMinTile;
        JPH::Mat44 faces[kPointFaces] {};
    };

    struct Grant {
        int32_t tileBase = -1;

        bool reuse = false;
    };

    struct Placement {
        int32_t  lightIndex = -1;
        int32_t  tileBase   = -1;
        uint32_t faceCount  = 0;
        bool     cached     = false;
    };

    void BeginFrame() noexcept;

    [[nodiscard]] auto Acquire(const Request& req) noexcept -> Grant;

    void EndFrame() noexcept;

    [[nodiscard]] auto Tiles() noexcept -> std::span<ShadowTile> { return std::span {_tiles.data(), _tiles.size()}; }
    [[nodiscard]] auto TileCount() const noexcept -> uint32_t { return static_cast<uint32_t>(_tiles.size()); }
    [[nodiscard]] auto Placements() const noexcept -> std::span<const Placement> { return _placements; }

    struct TileRect {
        uint32_t x    = 0;
        uint32_t y    = 0;
        uint32_t size = 0;
    };
    [[nodiscard]] static auto TileRectOf(const ShadowTile& tile) noexcept -> TileRect;

    [[nodiscard]] static auto FaceCount(bool isPoint, float outerConeRadians) noexcept -> uint32_t;
    [[nodiscard]] static auto MakeFaceMatrices(
        const JPH::Vec3& position, const JPH::Vec3& direction, float range, bool isPoint, float outerConeRadians, JPH::Mat44* out
    ) noexcept -> uint32_t;

    [[nodiscard]] static auto MakeLightKey(LightType type, const JPH::Vec3& position, float intensity, float range) noexcept -> uint64_t;

    [[nodiscard]] static auto TileForDistance(float distance) noexcept -> uint32_t;

  private:
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
