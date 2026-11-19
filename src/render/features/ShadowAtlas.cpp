// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "render/features/ShadowAtlas.hpp"
#include <Zahlen/Math/Angles.hpp>
#include <Zahlen/Math/Vec2.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Render/GpuEnums.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <numbers>

namespace ZHLN {

namespace {

constexpr float kAtlasNearPlane = 0.05f;

struct FaceBasis {
    FVec3 x;
    FVec3 y;
    FVec3 z;
};

constexpr std::array<Mat3, ShadowAtlas::kPointFaces> kFaceAxes = {{
    Mat3::FromCols({0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}),
    Mat3::FromCols({0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}),
    Mat3::FromCols({1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, -1.0f, 0.0f}),
    Mat3::FromCols({1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}),
    Mat3::FromCols({1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, -1.0f}),
    Mat3::FromCols({-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}),
}};

[[nodiscard]] auto FaceBasisAt(uint32_t face) noexcept -> FaceBasis {
    auto m = kFaceAxes[face];
    return FaceBasis {
        .x = m.Column(0),
        .y = m.Column(1),
        .z = m.Column(2),
    };
}

[[nodiscard]] auto BasisToView(const FaceBasis& basis, const FVec3& origin) noexcept -> Mat4x4 {
    const Mat4x4 rotation = Mat4x4::FromCols(
        FVec4 {basis.x.x, basis.y.x, basis.z.x, 0.0f},
        FVec4 {basis.x.y, basis.y.y, basis.z.y, 0.0f},
        FVec4 {basis.x.z, basis.y.z, basis.z.z, 0.0f},
        FVec4 {0.0f, 0.0f, 0.0f, 1.0f}
    );
    const FVec3 shift = TransformVector(rotation, -origin);
    Mat4x4      view  = rotation;
    view[0, 3] = shift.x;
    view[1, 3] = shift.y;
    view[2, 3] = shift.z;
    return view;
}

[[nodiscard]] auto ConeToView(const FVec3& direction, const FVec3& origin) noexcept -> Mat4x4 {
    const FVec3 forward = direction.LengthSquared() > 1e-8f ? direction.Normalized() : FVec3 {0.0f, 0.0f, -1.0f};
    const FVec3 hint    = (std::abs(forward.y) > 0.99f) ? FVec3::UnitZ() : FVec3::UnitY();
    const FVec3 right   = Cross(hint, forward).Normalized();
    const FaceBasis basis {.x = right, .y = Cross(forward, right), .z = -forward};
    return BasisToView(basis, origin);
}

} // namespace

auto ShadowAtlas::FaceCount(bool isPoint, float outerConeRadians) noexcept -> uint32_t {
    return (isPoint || outerConeRadians <= 0.0f) ? kPointFaces : 1;
}

auto ShadowAtlas::MakeFaceMatrices(
    const FVec3& position,
    const FVec3& direction,
    float            range,
    bool             isPoint,
    float            outerConeRadians,
    Mat4x4*      out
) noexcept -> uint32_t {
    const float    farPlane  = std::max(range, kAtlasNearPlane * 2.0f);
    const uint32_t faceCount = FaceCount(isPoint, outerConeRadians);

    if (faceCount == 1) {
        const float      outer      = std::clamp(outerConeRadians, 0.01f, std::numbers::pi_v<float> * 0.45f);
        const Mat4x4 projection = CreatePerspective(RadF{outer * 2.0f}, 1.0f, kAtlasNearPlane, farPlane);
        out[0]                      = projection * ConeToView(direction, position);
        return 1;
    }

    const Mat4x4 projection = CreatePerspective(RadF{std::numbers::pi_v<float> * 0.5f}, 1.0f, kAtlasNearPlane, farPlane);
    for (uint32_t face = 0; face < kPointFaces; ++face) {
        out[face] = projection * BasisToView(FaceBasisAt(face), position);
    }
    return kPointFaces;
}

auto ShadowAtlas::MakeLightKey(LightType type, const FVec3& position, float intensity, float range) noexcept -> uint64_t {
    const float parts[] = {position.x, position.y, position.z, intensity, range};
    uint64_t    hash    = Hash64(reinterpret_cast<const char*>(parts), sizeof(parts));
    const auto  kind    = static_cast<uint32_t>(type);
    hash ^= Hash64(reinterpret_cast<const char*>(&kind), sizeof(kind)) + kGolden64 + (hash << 6) + (hash >> 2);
    return hash == 0 ? 1 : hash;
}

auto ShadowAtlas::TileForDistance(float distance) noexcept -> uint32_t {
    if (distance < 8.0f) {
        return kMaxTile;
    }
    if (distance < 20.0f) {
        return 512;
    }
    if (distance < 60.0f) {
        return 256;
    }
    return kMinTile;
}

auto ShadowAtlas::TileRectOf(const ShadowTile& tile) noexcept -> TileRect {
    const auto atlas = static_cast<float>(kSize);
    const auto size  = static_cast<uint32_t>(std::lround(static_cast<float>(tile.uvScale) * atlas));
    return TileRect {
        .x    = static_cast<uint32_t>(std::lround(tile.uvOrigin.x * atlas)),
        .y    = static_cast<uint32_t>(std::lround(tile.uvOrigin.y * atlas)),
        .size = size,
    };
}

void ShadowAtlas::BeginFrame() noexcept {
    _shelves.clear();
    _tiles.clear();
    _placements.clear();
    for (CacheEntry& entry: _cache) {
        entry.seen = false;
    }
}

auto ShadowAtlas::Place(uint32_t faceCount, uint32_t tile, uint32_t& outX, uint32_t& outY) noexcept -> bool {
    const uint32_t span = tile * faceCount;
    for (Shelf& shelf: _shelves) {
        if (shelf.tile == tile && shelf.cursor + span <= kSize) {
            outX         = shelf.cursor;
            outY         = shelf.y;
            shelf.cursor = shelf.cursor + span;
            return true;
        }
    }

    uint32_t y = 0;
    for (const Shelf& shelf: _shelves) {
        y = std::max(y, shelf.y + shelf.tile);
    }
    if (y + tile > kSize) {
        return false;
    }
    _shelves.push_back(Shelf {.tile = tile, .y = y, .cursor = span});
    outX = 0;
    outY = y;
    return true;
}

auto ShadowAtlas::Acquire(const Request& req) noexcept -> Grant {
    const uint32_t faceCount = std::min(req.faceCount, kPointFaces);
    if (faceCount == 0 || _placements.size() >= kMaxLights || _tiles.size() + faceCount > kMaxTiles) {
        _placements.push_back(Placement {.lightIndex = req.lightIndex, .tileBase = -1, .faceCount = faceCount, .cached = false});
        return Grant {};
    }

    uint32_t tile = req.tile;
    while (tile > kMinTile && tile * faceCount > kSize) {
        tile /= 2;
    }

    uint32_t x      = 0;
    uint32_t y      = 0;
    bool     placed = false;
    while (!(placed = Place(faceCount, tile, x, y))) {
        if (tile <= kMinTile) {
            break;
        }
        tile /= 2;
    }
    if (!placed) {
        _placements.push_back(Placement {.lightIndex = req.lightIndex, .tileBase = -1, .faceCount = faceCount, .cached = false});
        return Grant {};
    }

    CacheEntry* cached = nullptr;
    for (CacheEntry& entry: _cache) {
        if (entry.key == req.key) {
            cached = &entry;
            break;
        }
    }
    bool reuse = cached != nullptr && !cached->seen && cached->x == x && cached->y == y && cached->tile == tile && cached->faceCount == faceCount;
    if (reuse) {
        for (uint32_t face = 0; face < faceCount && reuse; ++face) {
            reuse = cached->faces[face] == req.faces[face];
        }
    }

    const auto  tileBase = static_cast<int32_t>(_tiles.size());
    const float invAtlas = 1.0f / static_cast<float>(kSize);
    for (uint32_t face = 0; face < faceCount; ++face) {
        ShadowTile tileEntry {};
        tileEntry.viewProj = req.faces[face];
        tileEntry.uvOrigin = {static_cast<float>(x + face * tile) * invAtlas, static_cast<float>(y) * invAtlas};
        tileEntry.uvScale  = static_cast<float>(tile) * invAtlas;
        _tiles.push_back(tileEntry);
    }

    if (cached == nullptr) {
        _cache.push_back(CacheEntry {});
        cached = &_cache.back();
    }
    cached->key       = req.key;
    cached->faceCount = faceCount;
    cached->tile      = tile;
    cached->x         = x;
    cached->y         = y;
    cached->seen      = true;
    for (uint32_t face = 0; face < faceCount; ++face) {
        cached->faces[face] = req.faces[face];
    }

    _placements.push_back(Placement {.lightIndex = req.lightIndex, .tileBase = tileBase, .faceCount = faceCount, .cached = reuse});
    return Grant {.tileBase = tileBase, .reuse = reuse};
}

void ShadowAtlas::EndFrame() noexcept {
    for (auto* it = _cache.begin(); it != _cache.end();) {
        it = it->seen ? std::next(it) : _cache.erase(it);
    }
}

} // namespace ZHLN
