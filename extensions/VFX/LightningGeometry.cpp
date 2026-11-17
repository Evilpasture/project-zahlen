// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#include "LightningGeometry.hpp"
#include <Zahlen/Math/Math3D.hpp>
#include <algorithm>
#include <cmath>
#include <span>
namespace ZHLN::LightningHost {
namespace {
struct LightningSegment {
    FVec3 start;
    FVec3 end;
    float width;
    float branchLevel;
};

auto GenerateFractalSegments(FVec3 start, FVec3 end, float startWidth, const ConfigInfo& config, std::mt19937& rng) -> std::vector<LightningSegment> {
    std::vector<LightningSegment> queue;
    queue.push_back({.start = start, .end = end, .width = startWidth, .branchLevel = 0.0f});

    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::uniform_real_distribution<float> prob(0.0f, 1.0f);

    for (int step = 0; step < config.subdivisions; ++step) {
        std::vector<LightningSegment> nextQueue;
        const float                   scale = std::pow(0.55f, static_cast<float>(step)) * (end - start).Length() * 0.18f;

        for (const auto& seg: queue) {
            const FVec3 mid = (seg.start + seg.end) * 0.5f;
            const FVec3 dir = (seg.end - seg.start);
            const float len = dir.Length();

            if (len < 0.1f) {
                nextQueue.push_back(seg);
                continue;
            }
            const FVec3 dirNorm = dir / len;

            FVec3 side     = Cross(dirNorm, FVec3::UnitY());
            side           = (side.LengthSquared() < 1e-4f) ? FVec3::UnitX() : side.Normalized();
            const FVec3 up = Cross(dirNorm, side).Normalized();

            const FVec3 offset      = (side * dist(rng) + up * dist(rng)) * scale;
            const FVec3 jitteredMid = mid + offset;

            nextQueue.push_back({.start = seg.start, .end = jitteredMid, .width = seg.width, .branchLevel = seg.branchLevel});
            nextQueue.push_back({.start = jitteredMid, .end = seg.end, .width = seg.width, .branchLevel = seg.branchLevel});

            const float branchProb = (0.35f * config.eta / 2.0f) / (1.0f + seg.branchLevel * 0.8f);
            if (prob(rng) < branchProb) {
                const FVec3 branchDir = (dirNorm + (side * dist(rng) + up * dist(rng)) * 0.7f).Normalized();
                const float branchLen = len * (0.4f + prob(rng) * 0.3f);
                const FVec3 branchEnd = jitteredMid + branchDir * branchLen;

                nextQueue.push_back({.start = jitteredMid, .end = branchEnd, .width = seg.width * 0.5f, .branchLevel = seg.branchLevel + 1.0f});
            }
        }
        queue = std::move(nextQueue);
    }
    return queue;
}

auto BuildCameraFacingRibbon(std::span<const LightningSegment> segments, FVec3 cameraPos) -> GeneratedRibbon {
    GeneratedRibbon ribbon;
    ribbon.positions.reserve(segments.size() * 6);
    ribbon.frames.reserve(segments.size() * 6);
    ribbon.surfaces.reserve(segments.size() * 6);

    const auto n    = Math::PackNormal(0, 1, 0);
    const auto t    = Math::PackNormal(1, 0, 0, 1);
    const auto c    = Math::PackColor(1.0f, 1.0f, 1.0f, 1.0f);
    const auto uv00 = Math::PackUV(0.0f, 0.0f);
    const auto uv10 = Math::PackUV(1.0f, 0.0f);
    const auto uv01 = Math::PackUV(0.0f, 1.0f);
    const auto uv11 = Math::PackUV(1.0f, 1.0f);

    for (const auto& seg: segments) {
        const FVec3 p0 = seg.start;
        const FVec3 p1 = seg.end;

        const FVec3 dir = (p1 - p0);
        const float len = dir.Length();
        if (len < 1e-4f) {
            continue;
        }
        const FVec3 dirNorm = dir / len;

        const FVec3 toCamDiff = cameraPos - p0;
        const FVec3 toCam     = (toCamDiff.LengthSquared() < 1e-4f) ? FVec3::UnitZ() : toCamDiff.Normalized();

        FVec3 side = Cross(dirNorm, toCam);
        side       = (side.LengthSquared() < 1e-4f) ? FVec3::UnitX() : side.Normalized();

        const float width = seg.width;
        const FVec3 v0    = p0 - side * width;
        const FVec3 v1    = p0 + side * width;
        const FVec3 v2    = p1 - side * width;
        const FVec3 v3    = p1 + side * width;

        ribbon.positions.push_back({{v0.x, v0.y, v0.z}});
        ribbon.frames.push_back({.normal = n, .tangent = t});
        ribbon.surfaces.push_back({.uv = uv00, .color = c});
        ribbon.positions.push_back({{v1.x, v1.y, v1.z}});
        ribbon.frames.push_back({.normal = n, .tangent = t});
        ribbon.surfaces.push_back({.uv = uv10, .color = c});
        ribbon.positions.push_back({{v2.x, v2.y, v2.z}});
        ribbon.frames.push_back({.normal = n, .tangent = t});
        ribbon.surfaces.push_back({.uv = uv01, .color = c});

        ribbon.positions.push_back({{v2.x, v2.y, v2.z}});
        ribbon.frames.push_back({.normal = n, .tangent = t});
        ribbon.surfaces.push_back({.uv = uv01, .color = c});
        ribbon.positions.push_back({{v1.x, v1.y, v1.z}});
        ribbon.frames.push_back({.normal = n, .tangent = t});
        ribbon.surfaces.push_back({.uv = uv10, .color = c});
        ribbon.positions.push_back({{v3.x, v3.y, v3.z}});
        ribbon.frames.push_back({.normal = n, .tangent = t});
        ribbon.surfaces.push_back({.uv = uv11, .color = c});
    }

    ribbon.maxVertices = static_cast<uint32_t>(ribbon.positions.size());
    return ribbon;
}

} // namespace
auto GenerateRibbon(FVec3 start, FVec3 end, FVec3 camera, const ConfigInfo& cfg, std::mt19937& rng) -> GeneratedRibbon {
    const auto finite = [](FVec3 p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); };
    if (!finite(start) || !finite(end) || !finite(camera) || !std::isfinite((end - start).LengthSquared()) || !std::isfinite(cfg.eta) || cfg.eta < 0 ||
        cfg.eta > 8 || !std::isfinite(cfg.ribbonWidth) || cfg.ribbonWidth <= 0 || cfg.ribbonWidth > 100 || cfg.subdivisions < 0 || cfg.subdivisions > 8)
        return {};
    return BuildCameraFacingRibbon(GenerateFractalSegments(start, end, cfg.ribbonWidth, cfg, rng), camera);
}
} // namespace ZHLN::LightningHost
