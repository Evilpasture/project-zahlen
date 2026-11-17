// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Private host policy. CPU state stays outside Core and outside ECS components.
#include "ExplosionHost.hpp"
#include <Zahlen/COM/IDebugDraw.hpp>
#include <Zahlen/Math/Angles.hpp>
#include <Zahlen/Math/Math3D.hpp>
#include <Zahlen/Math/Transform.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <span>
#include <vector>
namespace ZHLN::ExplosionHost {
// ============================================================================
// Modernized Fast SIMD & Noise Utilities
// ============================================================================

[[nodiscard]] static float Hash31(FVec3 p) noexcept {
    float px = std::fmod(p.x * 0.1031f, 1.0f);
    float py = std::fmod(p.y * 0.1031f, 1.0f);
    float pz = std::fmod(p.z * 0.1031f, 1.0f);

    float dotVal = px * (pz + 31.32f) + py * (py + 31.32f) + pz * (px + 31.32f);
    return std::fmod((px + dotVal) * (py + dotVal) * (pz + dotVal), 1.0f);
}

[[nodiscard]] static FVec3 RandomInUnitSphere(std::mt19937& gen) noexcept {
    std::uniform_real_distribution<float> dis(0.0f, 1.0f);
    float                                 u     = dis(gen);
    float                                 v     = dis(gen);
    const auto                            theta = RadF {2.0f * std::numbers::pi_v<float> * u};
    float                                 phi   = std::acos(2.0f * v - 1.0f);
    float                                 r     = std::cbrt(dis(gen));
    return {r * std::sin(phi) * Cos(theta), r * std::sin(phi) * Sin(theta), r * std::cos(phi)};
}

[[nodiscard]] static FVec3 SampleConeDirection(std::mt19937& gen, float maxAngleDeg) noexcept {
    std::uniform_real_distribution<float> dis(0.0f, 1.0f);
    const auto                            theta = DegF {dis(gen) * maxAngleDeg}.ToRadians();
    float                                 phi   = dis(gen) * 2.0f * std::numbers::pi_v<float>;
    float                                 sinT  = Sin(theta);
    float                                 cosT  = Cos(theta);

    return FVec3 {sinT * std::cos(phi), std::abs(cosT) + 0.06f, sinT * std::sin(phi)}.Normalized();
}

[[nodiscard]] static FVec3 SampleRingDirection(std::mt19937& gen, float minAngleDeg, float maxAngleDeg) noexcept {
    std::uniform_real_distribution<float> dis(0.0f, 1.0f);
    const auto                            theta = DegF {minAngleDeg + dis(gen) * (maxAngleDeg - minAngleDeg)}.ToRadians();
    float                                 phi   = dis(gen) * 2.0f * std::numbers::pi_v<float>;
    float                                 sinT  = Sin(theta);
    float                                 cosT  = Cos(theta);

    return FVec3 {sinT * std::cos(phi), std::abs(cosT) * 0.2f + 0.02f, sinT * std::sin(phi)}.Normalized();
}

// ============================================================================
// Component Definitions
// ============================================================================

struct ShockwaveParticle {
    float delay     = 0.0f;
    float maxLife   = 0.5f;
    float maxRadius = 18.0f;
    float speed     = 1.0f;
    FVec3 colorStart {};
    FVec3 colorEnd {};
    bool  isGround = false;
};

struct ExplosionParticle {
    FVec3 position {};
    FVec3 velocity {};
    float life      = 0.0f;
    float maxLife   = 1.0f;
    float startSize = 1.0f;
    float drag      = 1.0f;
    float gravity   = 0.0f;
    FVec3 colorStart {};
    FVec3 colorMid {};
    FVec3 colorEnd {};
};

struct ExplosionComponent {
    Kind  type     = Kind::ArtilleryMortar;
    FVec3 origin   = FVec3::Zero();
    float scale    = 1.0f;
    float age      = 0.0f;
    float duration = 3.5f;

    std::vector<ExplosionParticle> fireball;
    std::vector<ExplosionParticle> soilSmoke;
    std::vector<ShockwaveParticle> shockwaves;

    Entity debrisEntity  = Entity::Null();
    bool   craterSpawned = false;

    // No GPU storage: the particles above are the simulation's own state, and
    // rendering them is RenderBatchGPU describing world-space quads for
    // SceneData (ApplyDrawData flushes them after BeginFrame).
};

struct CraterDecalComponent {
    float age          = 0.0f;
    float lifetime     = 28.0f; // Remains on terrain for 28 seconds
    float fadeDuration = 6.0f;  // Dissolves smoothly over the final 6 seconds
    float baseRadius   = 3.4f;
    float baseDepth    = 5.0f;
    FVec3 origin       = FVec3::Zero();
    Quat  rotation     = Quat::Identity();
};

[[nodiscard]] static constexpr float Hash2D(float x, float y) noexcept {
    float v = std::sin(x * 127.1f + y * 311.7f + 13.37f) * 43758.5453f;
    return v - std::floor(v);
}

[[nodiscard]] static constexpr float ValueNoise2D(float x, float y) noexcept {
    float ix = std::floor(x);
    float iy = std::floor(y);
    float fx = x - ix;
    float fy = y - iy;

    float a = Hash2D(ix, iy);
    float b = Hash2D(ix + 1.0f, iy);
    float c = Hash2D(ix, iy + 1.0f);
    float d = Hash2D(ix + 1.0f, iy + 1.0f);

    float u = fx * fx * (3.0f - 2.0f * fx);
    float v = fy * fy * (3.0f - 2.0f * fy);

    return std::lerp(std::lerp(a, b, u), std::lerp(c, d, u), v);
}

[[nodiscard]] static float FBM2D(float x, float y, int octaves = 4) noexcept {
    float v    = 0.0f;
    float amp  = 0.5f;
    float freq = 1.0f;
    float norm = 0.0f;

    for (int i = 0; i < octaves; ++i) {
        v += ValueNoise2D(x * freq, y * freq) * amp;
        norm += amp;
        amp *= 0.5f;
        freq *= 2.1f;
    }
    return v / norm;
}

// ============================================================================
// Procedural Texture Generators
// ============================================================================

static std::vector<uint32_t> GenerateFireTexture(uint32_t size) {
    std::vector<uint32_t> pixels(static_cast<size_t>(size * size));
    const float           center = size * 0.5f;

    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            float dx   = (static_cast<float>(x) - center) / center;
            float dy   = (static_cast<float>(y) - center) / center;
            float d    = std::sqrt(dx * dx + dy * dy);
            float ang  = std::atan2(dy, dx);
            float turb = FBM2D(x * 0.028f, y * 0.028f, 3) * 0.45f + FBM2D(x * 0.11f, y * 0.11f, 2) * 0.18f;

            float r = std::clamp(1.0f - d * 1.15f + turb * 0.9f + std::sin(ang * 6.0f + d * 8.0f) * 0.07f, 0.0f, 1.0f);

            float R = 0.0f, G = 0.0f, B = 0.0f, A = 0.0f;
            if (r > 0.01f) {
                A = std::pow(r, 0.9f);
                if (r < 0.25f) {
                    float t = r / 0.25f;
                    R       = 120.0f + t * 80.0f;
                    G       = 10.0f + t * 15.0f;
                } else if (r < 0.48f) {
                    float t = (r - 0.25f) / 0.23f;
                    R       = 200.0f + t * 45.0f;
                    G       = 25.0f + t * 80.0f;
                } else if (r < 0.72f) {
                    float t = (r - 0.48f) / 0.24f;
                    R       = 245.0f + t * 10.0f;
                    G       = 105.0f + t * 95.0f;
                    B       = t * 15.0f;
                } else {
                    float t = (r - 0.72f) / 0.28f;
                    R       = 255.0f;
                    G       = 200.0f + t * 55.0f;
                    B       = 120.0f + t * 135.0f;
                }
                float speck = Hash2D(x * 1.7f, y * 1.7f) * 0.08f;
                R           = std::min(255.0f, R + speck * 255.0f);
                G           = std::min(255.0f, G + speck * 120.0f);
            }

            pixels[y * size + x] = Math::PackColor(R / 255.0f, G / 255.0f, B / 255.0f, A).data;
        }
    }
    return pixels;
}

static std::vector<uint32_t> GenerateSoilTexture(uint32_t size) {
    std::vector<uint32_t> pixels(static_cast<size_t>(size * size));
    const float           center = size * 0.5f;

    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            float dx    = (static_cast<float>(x) - center) / center;
            float dy    = (static_cast<float>(y) - center) / center;
            float d     = std::sqrt(dx * dx + dy * dy);
            float n     = FBM2D(x * 0.025f, y * 0.025f, 3);
            float n2    = FBM2D(x * 0.06f + 10.0f, y * 0.06f, 2);
            float ang   = std::atan2(dy, dx);
            float alpha = std::clamp(
                std::pow(std::max(0.0f, 1.0f - d * 1.08f), 0.9f) * (0.65f + n * 0.55f) * (0.85f + n2 * 0.3f) * (1.0f + std::sin(ang * 3.0f + n * 6.0f) * 0.08f),
                0.0f, 1.0f
            );

            pixels[y * size + x] = Math::PackColor(138.0f / 255.0f, 122.0f / 255.0f, 98.0f / 255.0f, alpha).data;
        }
    }
    return pixels;
}

static std::vector<uint32_t> GenerateShockwaveRingTexture(uint32_t size) {
    std::vector<uint32_t> pixels(static_cast<size_t>(size * size));
    const float           center = size * 0.5f;

    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            float dx    = (static_cast<float>(x) - center) / center;
            float dy    = (static_cast<float>(y) - center) / center;
            float d     = std::sqrt(dx * dx + dy * dy);
            float ring  = std::pow(std::max(0.0f, 1.0f - std::abs(d - 0.82f) / 0.08f), 2.5f);
            float angle = std::atan2(dy, dx);
            float n     = FBM2D(dx * 4.0f, dy * 4.0f, 2);
            float strk  = std::pow(std::sin(angle * 12.0f + n * 3.0f) * 0.5f + 0.5f, 4.0f) * 0.35f;
            float alpha = (d > 1.0f) ? 0.0f : std::clamp((ring * 0.85f) + (ring * strk), 0.0f, 1.0f);

            pixels[y * size + x] = Math::PackColor(255.0f / 255.0f, 240.0f / 255.0f, 210.0f / 255.0f, alpha).data;
        }
    }
    return pixels;
}

static std::vector<uint32_t> GenerateGroundRingTexture(uint32_t size) {
    std::vector<uint32_t> pixels(static_cast<size_t>(size * size));
    const float           center = size * 0.5f;

    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            float dx        = (static_cast<float>(x) - center) / center;
            float dy        = (static_cast<float>(y) - center) / center;
            float d         = std::sqrt(dx * dx + dy * dy);
            float ring      = std::pow(std::max(0.0f, 1.0f - std::abs(d - 0.85f) / 0.10f), 2.0f) * (0.75f + FBM2D(x * 0.05f, y * 0.05f, 2) * 0.35f);
            float innerFill = std::max(0.0f, 1.0f - d / 0.85f) * 0.06f;
            float alpha     = (d > 1.0f) ? 0.0f : std::clamp((ring * 0.28f) + innerFill, 0.0f, 0.35f);

            pixels[y * size + x] = Math::PackColor(210.0f / 255.0f, 180.0f / 255.0f, 130.0f / 255.0f, alpha).data;
        }
    }
    return pixels;
}

static std::vector<uint32_t> GenerateCraterTexture(uint32_t size) {
    std::vector<uint32_t> pixels(static_cast<size_t>(size * size));
    const float           center = size * 0.5f;

    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            float dx    = (static_cast<float>(x) - center) / center;
            float dy    = (static_cast<float>(y) - center) / center;
            float d     = std::sqrt(dx * dx + dy * dy);
            float n     = FBM2D(x * 0.04f, y * 0.04f, 3);
            float alpha = (d <= 0.40f) ? 0.95f : ((d <= 0.85f) ? (0.95f * (1.0f - std::pow((d - 0.40f) / 0.45f, 2.0f)) * (0.8f + n * 0.4f)) : 0.0f);

            pixels[y * size + x] = Math::PackColor(28.0f / 255.0f, 22.0f / 255.0f, 18.0f / 255.0f, std::clamp(alpha, 0.0f, 1.0f)).data;
        }
    }
    return pixels;
}

static std::vector<uint32_t> GenerateCraterNormalTexture(uint32_t size) {
    std::vector<uint32_t> pixels(static_cast<size_t>(size * size));
    const float           center = size * 0.5f;

    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            float dx = (static_cast<float>(x) - center) / center;
            float dy = (static_cast<float>(y) - center) / center;
            float d  = std::sqrt(dx * dx + dy * dy);

            float nx = 0.0f;
            float ny = 0.0f;
            float nz = 1.0f;

            if (d > 0.001f && d < 0.95f) {
                float dirX = dx / d;
                float dirY = dy / d;

                float slope = 0.0f;
                if (d < 0.45f) {
                    slope = -std::sin(d / 0.45f * std::numbers::pi_v<float>) * 0.45f;
                } else if (d < 0.85f) {
                    float t = (d - 0.45f) / 0.40f;
                    slope   = std::sin(t * std::numbers::pi_v<float>) * 0.35f;
                }

                nx = dirX * slope;
                ny = dirY * slope;
                nz = std::sqrt(std::max(0.0f, 1.0f - (nx * nx + ny * ny)));
            }

            uint8_t r = static_cast<uint8_t>((nx * 0.5f + 0.5f) * 255.0f);
            uint8_t g = static_cast<uint8_t>((ny * 0.5f + 0.5f) * 255.0f);
            uint8_t b = static_cast<uint8_t>((nz * 0.5f + 0.5f) * 255.0f);

            pixels[y * size + x] = 0xFF000000u | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(g) << 8) | r;
        }
    }
    return pixels;
}

struct Sink {
    COM::IDebugDraw&               draw;
    std::expected<void, ErrorCode> result;
    void                           AddBillboards(TextureID texture, std::span<const BillboardQuad> quads, bool additive) {
        if (!result)
            return;
        auto submitted = draw.AddBillboards(texture, quads, additive);
        if (!submitted.HasValue())
            result = std::unexpected(submitted.error);
    }
};
struct Policy {
    static void InitArtilleryParticles(ExplosionComponent& exp, std::mt19937& gen) {
        exp.fireball.resize(78);
        for (size_t i = 0; i < exp.fireball.size(); ++i) {
            bool  isRing = (i > 52);
            FVec3 dir    = isRing ? SampleRingDirection(gen, 82.0f, 94.0f) : SampleConeDirection(gen, 34.0f);
            float speed  = (isRing ? (10.0f + Hash31(dir * 1.5f) * 6.0f) : (16.0f + Hash31(dir * 1.5f) * 11.0f)) * exp.scale;

            exp.fireball[i] = {
                .position   = dir * (0.1f * exp.scale),
                .velocity   = dir * speed,
                .life       = 0.0f,
                .maxLife    = 0.62f + Hash31(dir * 2.5f) * 0.44f,
                .startSize  = (0.85f + Hash31(dir * 3.5f) * 0.95f) * exp.scale,
                .drag       = 3.0f,
                .gravity    = -4.2f,
                .colorStart = FVec3 {4.0f, 2.8f, 1.2f},
                .colorMid   = FVec3 {2.5f, 1.0f, 0.2f},
                .colorEnd   = FVec3 {0.23f, 0.10f, 0.04f}
            };
        }

        exp.soilSmoke.resize(84);
        for (size_t i = 0; i < exp.soilSmoke.size(); ++i) {
            bool  isRing = (i < 24);
            FVec3 dir    = isRing ? SampleRingDirection(gen, 84.0f, 92.0f) : SampleConeDirection(gen, 40.0f);
            float speed  = (isRing ? (7.0f + Hash31(dir * 1.5f) * 4.0f) : (8.0f + Hash31(dir * 1.5f) * 6.0f)) * exp.scale;
            FVec3 sc     = (Hash31(dir * 6.5f) < 0.3f) ? FVec3 {0.42f, 0.35f, 0.26f} : FVec3 {0.54f, 0.48f, 0.38f};

            exp.soilSmoke[i] = {
                .position   = dir * (0.2f * exp.scale),
                .velocity   = dir * speed,
                .life       = 0.0f,
                .maxLife    = 2.3f + Hash31(dir * 2.5f) * 1.1f,
                .startSize  = (1.55f + Hash31(dir * 3.5f) * 1.35f) * exp.scale,
                .drag       = 1.8f,
                .gravity    = -1.1f,
                .colorStart = FVec3 {1.2f, 0.7f, 0.25f},
                .colorMid   = sc * 1.2f,
                .colorEnd   = sc * 0.7f
            };
        }

        exp.shockwaves = {
            {.delay      = 0.00f,
             .maxLife    = 0.45f,
             .maxRadius  = 16.0f,
             .speed      = 1.00f,
             .colorStart = {3.0f, 2.2f, 1.2f},
             .colorEnd   = {1.0f, 0.4f, 0.05f},
             .isGround   = false},
            {.delay      = 0.06f,
             .maxLife    = 0.55f,
             .maxRadius  = 22.0f,
             .speed      = 1.18f,
             .colorStart = {0.8f, 1.4f, 2.0f},
             .colorEnd   = {0.2f, 0.5f, 0.9f},
             .isGround   = false},
            {.delay      = 0.03f,
             .maxLife    = 0.50f,
             .maxRadius  = 12.0f,
             .speed      = 0.85f,
             .colorStart = {2.5f, 1.0f, 0.2f},
             .colorEnd   = {0.8f, 0.2f, 0.02f},
             .isGround   = false},
            {.delay      = 0.10f,
             .maxLife    = 0.65f,
             .maxRadius  = 26.0f,
             .speed      = 1.35f,
             .colorStart = {2.0f, 2.0f, 2.2f},
             .colorEnd   = {0.4f, 0.6f, 1.0f},
             .isGround   = false},
            {.delay      = 0.04f,
             .maxLife    = 0.80f,
             .maxRadius  = 20.0f,
             .speed      = 0.90f,
             .colorStart = {1.2f, 1.0f, 0.7f},
             .colorEnd   = {0.4f, 0.35f, 0.25f},
             .isGround   = true}
        };
    }

    static void InitStandardFireballParticles(ExplosionComponent& exp, std::mt19937& gen) {
        exp.fireball.resize(120);
        for (auto& p: exp.fireball) {
            FVec3 dir = RandomInUnitSphere(gen);
            dir.y     = std::abs(dir.y) * 0.6f + dir.y * 0.4f;

            p = {
                .position   = dir * (0.15f * exp.scale),
                .velocity   = dir * ((3.0f + Hash31(dir * 1.5f) * 6.0f) * exp.scale),
                .life       = 0.0f,
                .maxLife    = 0.7f + Hash31(dir * 2.5f) * 0.7f,
                .startSize  = (0.5f + Hash31(dir * 3.5f) * 0.8f) * exp.scale,
                .drag       = 2.5f,
                .gravity    = -1.0f,
                .colorStart = FVec3 {4.0f, 3.0f, 1.4f},
                .colorMid   = FVec3 {2.2f, 0.9f, 0.25f},
                .colorEnd   = FVec3 {0.4f, 0.06f, 0.02f}
            };
        }

        exp.soilSmoke.resize(80);
        for (auto& p: exp.soilSmoke) {
            FVec3 dir  = RandomInUnitSphere(gen);
            dir.y      = std::abs(dir.y) * 0.8f + dir.y * 0.2f;
            float gray = 0.05f + Hash31(dir * 6.5f) * 0.15f;

            p = {
                .position   = dir * (0.3f * exp.scale),
                .velocity   = dir * ((1.5f + Hash31(dir * 1.5f) * 3.5f) * exp.scale) + FVec3 {0.0f, 0.8f * exp.scale, 0.0f},
                .life       = 0.0f,
                .maxLife    = 2.5f + Hash31(dir * 2.5f) * 2.0f,
                .startSize  = (1.0f + Hash31(dir * 4.5f) * 1.0f) * exp.scale,
                .drag       = 1.2f,
                .gravity    = 0.6f,
                .colorStart = FVec3 {1.4f, 0.6f, 0.15f},
                .colorMid   = FVec3 {0.35f, 0.25f, 0.2f},
                .colorEnd   = FVec3 {gray, gray, gray * 0.95f}
            };
        }
    }

    static void UpdateGroup(std::span<ExplosionParticle> group, float dt, bool isSmoke) noexcept {
        for (auto& p: group) {
            p.life += dt;
            if (p.life > p.maxLife) {
                continue;
            }

            if (isSmoke) {
                p.velocity.y = p.velocity.y + 0.58f * dt;
            }

            p.velocity *= std::exp(-p.drag * dt);
            p.velocity.y = p.velocity.y + p.gravity * dt;
            p.position += p.velocity * dt;
        }
    }

    void RenderBatchGPU(Sink& scene, ExplosionComponent& exp) {
        // The authored simulation produces world-space quads. Sink copies the
        // batches through IDebugDraw; the renderer owns all GPU storage.
        thread_local std::vector<BillboardQuad> t_quads;

        // 1. FIREBALL
        if (!exp.fireball.empty()) {
            t_quads.clear();
            t_quads.reserve(exp.fireball.size());
            for (const auto& p: exp.fireball) {
                const float t = std::min(1.0f, p.life / p.maxLife);

                const FVec3 color = (t < 0.52f) ? p.colorStart + (p.colorMid - p.colorStart) * (t / 0.52f) :
                                                  p.colorMid + (p.colorEnd - p.colorMid) * ((t - 0.52f) / 0.48f);

                const float opacity = (1.0f - t) * 0.98f;
                const float size    = p.startSize * (1.0f + t * 1.15f);

                t_quads.push_back(
                    {.position = exp.origin + p.position,
                     .size     = size,
                     .rotation = 0.0f,
                     .color    = FVec4 {color * (opacity * 3.5f), opacity},
                     .facing   = ParticleAlignment::CameraBillboard}
                );
            }

            scene.AddBillboards(s_FireTexID, std::span {t_quads}, true);
        }

        // 2. SOIL SMOKE
        if (!exp.soilSmoke.empty()) {
            t_quads.clear();
            t_quads.reserve(exp.soilSmoke.size());
            for (const auto& p: exp.soilSmoke) {
                float opacity = 0.0f;
                float size    = p.startSize;
                FVec3 color   = p.colorEnd;

                if (p.life < p.maxLife) {
                    float t = p.life / p.maxLife;
                    color = (t < 0.5f) ? p.colorStart + (p.colorMid - p.colorStart) * (t / 0.5f) : p.colorMid + (p.colorEnd - p.colorMid) * ((t - 0.5f) / 0.5f);
                    opacity = 0.46f * std::pow(1.0f - t, 1.5f);
                    size    = p.startSize * (1.0f + t * 1.95f);
                }

                t_quads.push_back(
                    {.position = exp.origin + p.position,
                     .size     = size,
                     .rotation = 0.0f,
                     .color    = FVec4 {color, opacity},
                     .facing   = ParticleAlignment::CameraBillboard}
                );
            }

            scene.AddBillboards(s_SoilTexID, std::span {t_quads}, false);
        }

        // 3. SHOCKWAVE AIR & GROUND FRONTS
        if (!exp.shockwaves.empty()) {
            // Air Shockwave Billboards (4 Fronts)
            {
                t_quads.clear();
                for (size_t i = 0; i < 4; ++i) {
                    const auto& sw        = exp.shockwaves[i];
                    float       localTime = exp.age - sw.delay;
                    float       opacity   = 0.0f;
                    float       radius    = 0.1f;
                    FVec3       color     = sw.colorStart;

                    if (localTime > 0.0f && localTime < sw.maxLife) {
                        float t = localTime / sw.maxLife;
                        radius  = (1.0f - std::pow(1.0f - t, 3.0f)) * sw.maxRadius * exp.scale;
                        color   = sw.colorStart + (sw.colorEnd - sw.colorStart) * t;
                        opacity = std::pow(1.0f - t, 1.2f) * 0.45f;
                    }

                    t_quads.push_back(
                        {.position = exp.origin,
                         .size     = radius * 2.0f,
                         .rotation = 0.0f,
                         .color    = FVec4 {color * (opacity * 3.5f), opacity},
                         .facing   = ParticleAlignment::CameraBillboard}
                    );
                }

                scene.AddBillboards(s_ShockwaveTexID, std::span {t_quads}, true);
            }

            // Ground Dust Flat Front (1 Plane)
            {
                const auto& sw        = exp.shockwaves[4];
                float       localTime = exp.age - sw.delay;
                float       opacity   = 0.0f;
                float       radius    = 0.1f;
                FVec3       color     = sw.colorStart;

                if (localTime > 0.0f && localTime < sw.maxLife) {
                    float t = localTime / sw.maxLife;
                    radius  = (1.0f - std::pow(1.0f - t, 2.2f)) * sw.maxRadius * exp.scale;
                    color   = sw.colorStart + (sw.colorEnd - sw.colorStart) * t;
                    opacity = std::pow(1.0f - t, 1.5f) * 0.35f;
                }

                const BillboardQuad quad {
                    .position = exp.origin + FVec3 {0.0f, 0.05f, 0.0f},
                    .size     = radius * 2.0f,
                    .rotation = 0.0f,
                    .color    = FVec4 {color, opacity},
                    .facing   = ParticleAlignment::GroundFlat,
                };
                scene.AddBillboards(s_GroundRingID, std::span {&quad, 1}, true);
            }
        }
    }

    TextureID s_FireTexID         = TextureID::Invalid;
    TextureID s_SoilTexID         = TextureID::Invalid;
    TextureID s_ShockwaveTexID    = TextureID::Invalid;
    TextureID s_GroundRingID      = TextureID::Invalid;
    TextureID s_CraterTexID       = TextureID::Invalid;
    TextureID s_CraterNormalTexID = TextureID::Invalid;
};
} // namespace ZHLN::ExplosionHost
