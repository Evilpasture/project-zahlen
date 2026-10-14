// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "features/ShadowAtlas.hpp"
#include <Jolt/Math/Float2.h>
#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/Render/GpuEnums.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

namespace ZHLN {

namespace {

constexpr float kAtlasNearPlane = 0.05f;

struct FaceBasis {
    JPH::Vec3 x;
    JPH::Vec3 y;
    JPH::Vec3 z;
};

constexpr float kFaceAxes[ShadowAtlas::kPointFaces][9] = {
    {0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f, -1.0f, 0.0f, 0.0f},
    {0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f},
    {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, -1.0f, 0.0f},
    {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f},
    {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, -1.0f},
    {-1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f},
};

[[nodiscard]] auto FaceBasisAt(uint32_t face) noexcept -> FaceBasis {
    const float* a = kFaceAxes[face];
    return FaceBasis {
        JPH::Vec3 {a[0], a[1], a[2]},
        JPH::Vec3 {a[3], a[4], a[5]},
        JPH::Vec3 {a[6], a[7], a[8]},
    };
}

[[nodiscard]] auto BasisToView(const FaceBasis& basis, const JPH::Vec3& origin) noexcept -> JPH::Mat44 {
    const JPH::Mat44 rotation {
        JPH::Vec4 {basis.x.GetX(), basis.y.GetX(), basis.z.GetX(), 0.0f},
        JPH::Vec4 {basis.x.GetY(), basis.y.GetY(), basis.z.GetY(), 0.0f},
        JPH::Vec4 {basis.x.GetZ(), basis.y.GetZ(), basis.z.GetZ(), 0.0f},
        JPH::Vec4 {0.0f, 0.0f, 0.0f, 1.0f},
    };
    const JPH::Vec3 shift = rotation * (-origin);
    JPH::Mat44      view  = rotation;
    view.SetColumn4(3, JPH::Vec4 {shift.GetX(), shift.GetY(), shift.GetZ(), 1.0f});
    return view;
}

[[nodiscard]] auto ConeToView(const JPH::Vec3& direction, const JPH::Vec3& origin) noexcept -> JPH::Mat44 {
    const JPH::Vec3 forward = direction.LengthSq() > 1e-8f ? direction.Normalized() : JPH::Vec3 {0.0f, 0.0f, -1.0f};
    const JPH::Vec3 hint    = (std::abs(forward.GetY()) > 0.99f) ? JPH::Vec3::sAxisZ() : JPH::Vec3::sAxisY();
    const JPH::Vec3 right   = hint.Cross(forward).NormalizedOr(JPH::Vec3::sAxisX());
    const FaceBasis basis {right, forward.Cross(right), -forward};
    return BasisToView(basis, origin);
}

} // namespace

auto ShadowAtlas::FaceCount(bool isPoint, float outerConeRadians) noexcept -> uint32_t {
    return (isPoint || outerConeRadians <= 0.0f) ? kPointFaces : 1;
}

auto ShadowAtlas::MakeFaceMatrices(
    const JPH::Vec3& position, const JPH::Vec3& direction, float range, bool isPoint, float outerConeRadians, JPH::Mat44* out
) noexcept -> uint32_t {
    const float    farPlane  = std::max(range, kAtlasNearPlane * 2.0f);
    const uint32_t faceCount = FaceCount(isPoint, outerConeRadians);

    if (faceCount == 1) {
        const float outer   = std::clamp(outerConeRadians, 0.01f, JPH::JPH_PI * 0.45f);
        const JPH::Mat44 projection = Math::CreatePerspective(outer * 2.0f, 1.0f, kAtlasNearPlane, farPlane);
        out[0]                        = projection * ConeToView(direction, position);
        return 1;
    }

    const JPH::Mat44 projection = Math::CreatePerspective(JPH::JPH_PI * 0.5f, 1.0f, kAtlasNearPlane, farPlane);
    for (uint32_t face = 0; face < kPointFaces; ++face) {
        out[face] = projection * BasisToView(FaceBasisAt(face), position);
    }
    return kPointFaces;
}

auto ShadowAtlas::MakeLightKey(LightType type, const JPH::Vec3& position, float intensity, float range) noexcept -> uint64_t {
    const float    parts[] = {position.GetX(), position.GetY(), position.GetZ(), intensity, range};
    uint64_t       hash    = Hash64(reinterpret_cast<const char*>(parts), sizeof(parts));
    const uint32_t kind    = static_cast<uint32_t>(type);
    hash ^= Hash64(reinterpret_cast<const char*>(&kind), sizeof(kind)) + kGolden64 + (hash << 6) + (hash >> 2);
    return hash == 0 ? 1 : hash;
}

auto ShadowAtlas::TileForDistance(float distance) noexcept -> uint32_t {
    if (distance < 8.0f) return kMaxTile;
    if (distance < 20.0f) return 512;
    if (distance < 60.0f) return 256;
    return kMinTile;
}

auto ShadowAtlas::TileRectOf(const ShadowTile& tile) noexcept -> TileRect {
    const float   atlas = static_cast<float>(kSize);
    const uint32_t size = static_cast<uint32_t>(std::lround(static_cast<float>(tile.uvScale) * atlas));
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

    uint32_t x = 0;
    uint32_t y = 0;
    if (!Place(faceCount, req.tile, x, y)) {
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
    bool reuse = cached != nullptr && cached->seen == false && cached->x == x && cached->y == y && cached->tile == req.tile &&
                 cached->faceCount == faceCount;
    if (reuse) {
        for (uint32_t face = 0; face < faceCount && reuse; ++face) {
            reuse = std::memcmp(&cached->faces[face], &req.faces[face], sizeof(JPH::Mat44)) == 0;
        }
    }

    const int32_t tileBase = static_cast<int32_t>(_tiles.size());
    const float   invAtlas = 1.0f / static_cast<float>(kSize);
    for (uint32_t face = 0; face < faceCount; ++face) {
        ShadowTile tile {};
        tile.viewProj = req.faces[face];
        tile.uvOrigin = JPH::Float2 {static_cast<float>(x + face * req.tile) * invAtlas, static_cast<float>(y) * invAtlas};
        tile.uvScale  = static_cast<float>(req.tile) * invAtlas;
        _tiles.push_back(tile);
    }

    if (cached == nullptr) {
        _cache.push_back(CacheEntry {});
        cached = &_cache.back();
    }
    cached->key       = req.key;
    cached->faceCount = faceCount;
    cached->tile      = req.tile;
    cached->x         = x;
    cached->y         = y;
    cached->seen      = true;
    for (uint32_t face = 0; face < faceCount; ++face) {
        cached->faces[face] = req.faces[face];
    }

    _placements.push_back(
        Placement {.lightIndex = req.lightIndex, .tileBase = tileBase, .faceCount = faceCount, .cached = reuse}
    );
    return Grant {.tileBase = tileBase, .reuse = reuse};
}

void ShadowAtlas::EndFrame() noexcept {
    for (auto it = _cache.begin(); it != _cache.end();) {
        it = it->seen ? std::next(it) : _cache.erase(it);
    }
}

} // namespace ZHLN
