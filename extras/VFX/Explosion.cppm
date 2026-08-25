// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module;

// Public Engine & Jolt Headers
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
#include <Zahlen/Audio.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/ecs/EntityCommandBuffer.hpp>

// Standard Library Headers
#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <span>
#include <vector>

export module ZHLN.Explosions;

namespace ZHLN {

// ============================================================================
// Ordnance Types
// ============================================================================

export enum class OrdnanceType: uint8_t {
    StandardFireball, // Standard omnidirectional fireball
    ArtilleryMortar   // Anisotropic cone, shockwave rings, heavy soil ejecta, crater decal & blast impulse
};

// ============================================================================
// Modernized Fast SIMD & Noise Utilities
// ============================================================================

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

[[nodiscard]] static inline float FBM2D(float x, float y, int octaves = 4) noexcept {
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

[[nodiscard]] static inline float Hash31(JPH::Vec3Arg p) noexcept {
    float px = std::fmod(p.GetX() * 0.1031f, 1.0f);
    float py = std::fmod(p.GetY() * 0.1031f, 1.0f);
    float pz = std::fmod(p.GetZ() * 0.1031f, 1.0f);

    float dotVal = px * (pz + 31.32f) + py * (py + 31.32f) + pz * (px + 31.32f);
    return std::fmod((px + dotVal) * (py + dotVal) * (pz + dotVal), 1.0f);
}

[[nodiscard]] static inline JPH::Vec3 RandomInUnitSphere(std::mt19937& gen) noexcept {
    std::uniform_real_distribution<float> dis(0.0f, 1.0f);
    float                                 u     = dis(gen);
    float                                 v     = dis(gen);
    float                                 theta = 2.0f * std::numbers::pi_v<float> * u;
    float                                 phi   = std::acos(2.0f * v - 1.0f);
    float                                 r     = std::cbrt(dis(gen));
    return {r * std::sin(phi) * std::cos(theta), r * std::sin(phi) * std::sin(theta), r * std::cos(phi)};
}

[[nodiscard]] static inline JPH::Vec3 SampleConeDirection(std::mt19937& gen, float maxAngleDeg) noexcept {
    std::uniform_real_distribution<float> dis(0.0f, 1.0f);
    float                                 theta = JPH::DegreesToRadians(dis(gen) * maxAngleDeg);
    float                                 phi   = dis(gen) * 2.0f * std::numbers::pi_v<float>;
    float                                 sinT  = std::sin(theta);
    float                                 cosT  = std::cos(theta);

    return JPH::Vec3(sinT * std::cos(phi), std::abs(cosT) + 0.06f, sinT * std::sin(phi)).Normalized();
}

[[nodiscard]] static inline JPH::Vec3 SampleRingDirection(std::mt19937& gen, float minAngleDeg, float maxAngleDeg) noexcept {
    std::uniform_real_distribution<float> dis(0.0f, 1.0f);
    float                                 theta = JPH::DegreesToRadians(minAngleDeg + dis(gen) * (maxAngleDeg - minAngleDeg));
    float                                 phi   = dis(gen) * 2.0f * std::numbers::pi_v<float>;
    float                                 sinT  = std::sin(theta);
    float                                 cosT  = std::cos(theta);

    return JPH::Vec3(sinT * std::cos(phi), std::abs(cosT) * 0.2f + 0.02f, sinT * std::sin(phi)).Normalized();
}

// ============================================================================
// Procedural Texture Generators
// ============================================================================

static inline std::vector<uint32_t> GenerateFireTexture(uint32_t size) {
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

static inline std::vector<uint32_t> GenerateSoilTexture(uint32_t size) {
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

static inline std::vector<uint32_t> GenerateShockwaveRingTexture(uint32_t size) {
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

static inline std::vector<uint32_t> GenerateGroundRingTexture(uint32_t size) {
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

static inline std::vector<uint32_t> GenerateCraterTexture(uint32_t size) {
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

static inline std::vector<uint32_t> GenerateCraterNormalTexture(uint32_t size) {
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

// ============================================================================
// Component Definitions
// ============================================================================

struct ShockwaveParticle {
    float     delay     = 0.0f;
    float     maxLife   = 0.5f;
    float     maxRadius = 18.0f;
    float     speed     = 1.0f;
    JPH::Vec3 colorStart {};
    JPH::Vec3 colorEnd {};
    bool      isGround = false;
};

struct ExplosionParticle {
    JPH::Vec3 position {};
    JPH::Vec3 velocity {};
    float     life      = 0.0f;
    float     maxLife   = 1.0f;
    float     startSize = 1.0f;
    float     drag      = 1.0f;
    float     gravity   = 0.0f;
    JPH::Vec3 colorStart {};
    JPH::Vec3 colorMid {};
    JPH::Vec3 colorEnd {};
};

export struct ExplosionComponent {
    OrdnanceType type     = OrdnanceType::ArtilleryMortar;
    JPH::Vec3    origin   = JPH::Vec3::sZero();
    float        scale    = 1.0f;
    float        age      = 0.0f;
    float        duration = 3.5f;

    std::vector<ExplosionParticle> fireball;
    std::vector<ExplosionParticle> soilSmoke;
    std::vector<ShockwaveParticle> shockwaves;

    Entity debrisEntity  = Entity::Null();
    bool   craterSpawned = false;
};

export struct CraterDecalComponent {
    float     age          = 0.0f;
    float     lifetime     = 28.0f; // Remains on terrain for 28 seconds
    float     fadeDuration = 6.0f;  // Dissolves smoothly over the final 6 seconds
    float     baseRadius   = 3.4f;
    float     baseDepth    = 5.0f;
    JPH::Vec3 origin       = JPH::Vec3::sZero();
    JPH::Quat rotation     = JPH::Quat::sIdentity();
};

// ============================================================================
// Explosion System Subsystem
// ============================================================================

export class ExplosionSystem {
  public:
    static void Init(Engine& engine) {
        auto& rc  = engine.GetRenderContext();
        auto& reg = engine.GetRegistry();

        // Register ECS components for the current registry instance
        reg.RegisterComponent<ExplosionComponent>("ExplosionComponent");
        reg.RegisterComponent<CraterDecalComponent>("CraterDecalComponent");

        if (s_LastRenderContext == &rc) {
            return;
        }
        s_LastRenderContext = &rc;

        // Register procedural textures into TextureManager
        s_FireTexHandle         = rc.CreateProceduralTexture("vfx_artillery_fire", 256, 256, true, GenerateFireTexture(256).data());
        s_SoilTexHandle         = rc.CreateProceduralTexture("vfx_artillery_soil", 256, 256, true, GenerateSoilTexture(256).data());
        s_ShockwaveTexHandle    = rc.CreateProceduralTexture("vfx_artillery_shockwave", 512, 512, true, GenerateShockwaveRingTexture(512).data());
        s_GroundRingHandle      = rc.CreateProceduralTexture("vfx_artillery_ground_ring", 512, 512, true, GenerateGroundRingTexture(512).data());
        s_CraterTexHandle       = rc.CreateProceduralTexture("vfx_artillery_crater", 256, 256, true, GenerateCraterTexture(256).data());
        s_CraterNormalTexHandle = rc.CreateProceduralTexture("vfx_artillery_crater_norm", 256, 256, false, GenerateCraterNormalTexture(256).data());

        // Debris box mesh for physical ejecta chunks
        Mesh boxMesh = CreativeWorksFactory::CreateBoxMesh(rc, JPH::Vec3(0.5f, 0.5f, 0.5f), {0.28f, 0.22f, 0.16f, 1.0f});

        Material debrisMat = CreativeWorksFactory::CreateMaterial(
                                 rc,
                                 {
                                     .roughness = 0.94f,
                                     .baseColor = {0.28f, 0.22f, 0.16f, 1.0f},
                                 }
        )
                                 .value_or(Material {});

        s_DebrisMeshAsset = HashAssetID("artillery_debris_mesh");
        s_DebrisMatAsset  = HashAssetID("artillery_debris_mat");

        rc.RegisterGPUMesh(s_DebrisMeshAsset, boxMesh);
        rc.RegisterGPUMaterial(s_DebrisMatAsset, debrisMat);
    }

    static Entity Spawn(Engine& engine, const JPH::Vec3& origin, float scale = 1.0f, OrdnanceType type = OrdnanceType::ArtilleryMortar) {
        auto& reg = engine.GetRegistry();
        Init(engine);

        ExplosionComponent initialExp {.type = type, .origin = origin, .scale = scale, .duration = (type == OrdnanceType::ArtilleryMortar) ? 3.5f : 2.5f};

        std::mt19937 gen(std::random_device {}());
        if (type == OrdnanceType::ArtilleryMortar) {
            InitArtilleryParticles(initialExp, gen);
        } else {
            InitStandardFireballParticles(initialExp, gen);
        }

        Entity root         = reg.Create();
        Entity debrisEntity = (type == OrdnanceType::ArtilleryMortar) ? reg.Create() : Entity::Null();

        initialExp.debrisEntity = debrisEntity;

        // Position light directly at origin with slight vertical offset
        JPH::Vec3  lightWorldPos = origin + JPH::Vec3(0.0f, 1.8f * scale, 0.0f);
        JPH::Mat44 rootTransform = Math::CreateTransform(lightWorldPos, JPH::Quat::sIdentity());

        // Attach synchronously to root entity
        reg.Add(
            root, Components::NameComponent {.name = String64("ArtilleryExplosionRoot")}, Components::TransformComponent {.position = lightWorldPos},
            Components::WorldTransformComponent {.world = rootTransform, .previous = rootTransform},
            Components::LightComponent {
                .type        = LightType::Point,
                .color       = JPH::Vec3(1.0f, 0.48f, 0.09f),
                .intensity   = 2800.0f * scale * scale,
                .radius      = 1.5f,
                .range       = 60.0f * scale,
                .shadowLayer = -1
            },
            std::move(initialExp)
        );

        if (debrisEntity != Entity::Null()) {
            reg.Add(
                debrisEntity, Components::NameComponent {.name = String64("Artillery3DDebris")}, Components::TransformComponent {.position = origin},
                Components::MeshParticleEmitterComponent {
                    .meshAsset     = s_DebrisMeshAsset,
                    .materialAsset = s_DebrisMatAsset,
                    .maxParticles  = 48,
                    .active        = true,
                    .params =
                        {.gravity     = {0.0f, -18.0f * scale, 0.0f},
                         .drag        = 0.32f,
                         .spawnOrigin = {origin.GetX(), origin.GetY(), origin.GetZ()},
                         .initVelMin  = {-12.0f * scale, 6.0f * scale, -12.0f * scale},
                         .lifetimeMin = 2.0f,
                         .initVelMax  = {12.0f * scale, 20.0f * scale, 12.0f * scale},
                         .lifetimeMax = 3.0f,
                         .rotVelMin   = {-8.0f, -8.0f, -8.0f},
                         .scaleMin    = 0.09f * scale,
                         .rotVelMax   = {8.0f, 8.0f, 8.0f},
                         .scaleMax    = 0.27f * scale,
                         .startColor  = {0.28f, 0.22f, 0.16f, 1.0f},
                         .endColor    = {0.28f, 0.22f, 0.16f, 1.0f}}
                },
                Components::HierarchyComponent {.parent = root}
            );
        }

        // Procedural Audio DSP
        auto& audio = engine.GetAudioContext();
        audio.PostEvent(
            {.type       = AudioEventType::NoiseBurst3D,
             .position   = origin,
             .volume     = 1.0f,
             .param1     = 60.0f, // Low pass cutoff frequency
             .param2     = 3.5f,  // Q factor
             .duration   = 0.85f,
             .filterType = AudioFilterType::LowPass,
             .noiseType  = AudioNoiseType::Brownian}
        );

        audio.PostEvent(
            {.type     = AudioEventType::ToneSweep3D,
             .position = origin,
             .volume   = 0.85f,
             .param1   = 150.0f, // Start frequency
             .param2   = 30.0f,  // End frequency
             .duration = 0.45f,
             .waveType = AudioWaveformType::Sawtooth}
        );

        audio.PostEvent(
            {.type       = AudioEventType::NoiseBurst3D,
             .position   = origin,
             .volume     = 0.65f,
             .param1     = 320.0f, // Band pass center frequency
             .param2     = 1.4f,   // Q factor
             .duration   = 0.30f,
             .filterType = AudioFilterType::BandPass,
             .noiseType  = AudioNoiseType::Pink}
        );

        return root;
    }

    static void Update(Engine& engine, float dt) {
        Init(engine);

        auto& reg = engine.GetRegistry();
        auto& ecb = engine.GetMainECB();
        auto& rc  = engine.GetRenderContext();

        // --------------------------------------------------------------------
        // 1. UPDATE ACTIVE EXPLOSIONS (Fireball, Flash, Shockwaves)
        // --------------------------------------------------------------------
        auto expEntities = reg.GetEntitiesWith<ExplosionComponent>();
        if (!expEntities.empty()) {
            auto explosions = reg.GetRawArray<ExplosionComponent>();

            for (size_t i = 0; i < expEntities.size(); ++i) {
                Entity              e   = expEntities[i];
                ExplosionComponent& exp = explosions[i];
                exp.age += dt;

                // Flash Decay on Root Entity (Fades sharply to true 0 within ~0.6s)
                reg.Patch<Components::LightComponent>(e, [&](auto& light) {
                    float flashT = std::exp(-exp.age * 5.5f);
                    if (flashT < 0.002f) {
                        light.intensity = 0.0f; // Drop to pitch black so zero light lingers in fog
                    } else {
                        float flicker   = (0.92f + Hash31(exp.origin * exp.age) * 0.16f);
                        light.intensity = flashT * flicker * 2800.0f * exp.scale * exp.scale;
                        light.color     = JPH::Vec3(1.0f, 0.48f * flashT + 0.1f, 0.09f * flashT);
                    }
                });

                // Spawn autonomous crater decal
                if (!exp.craterSpawned && exp.age >= 0.20f && exp.type == OrdnanceType::ArtilleryMortar) {
                    exp.craterSpawned = true;
                    Entity crater     = reg.Create();

                    float     randomYaw = Hash31(exp.origin) * 6.2831853f;
                    JPH::Quat yawRot    = JPH::Quat::sRotation(JPH::Vec3::sAxisY(), randomYaw);
                    JPH::Quat baseRot   = JPH::Quat::sRotation(JPH::Vec3::sAxisX(), JPH::DegreesToRadians(-90.0f));
                    JPH::Quat decalRot  = (yawRot * baseRot).Normalized();

                    float     radius    = 3.4f * exp.scale;
                    float     projDepth = 5.0f * exp.scale;
                    JPH::Vec3 decalScale(radius * 2.0f, radius * 2.0f, projDepth);

                    JPH::Mat44 localMat = Math::CreateTransform(exp.origin, decalRot, decalScale);

                    // Autonomous entity with CraterDecalComponent
                    ecb.AddComponent(
                        crater, Components::NameComponent {.name = String64("ArtilleryCraterDecal")},
                        Components::TransformComponent {.position = exp.origin, .rotation = decalRot, .scale = decalScale},
                        Components::WorldTransformComponent {.world = localMat, .previous = localMat},
                        Components::DecalComponent {.albedoMap = s_CraterTexHandle, .normalMap = s_CraterNormalTexHandle, .roughness = 0.96f, .metallic = 0.0f},
                        CraterDecalComponent {
                            .age          = 0.0f,
                            .lifetime     = 28.0f,
                            .fadeDuration = 6.0f,
                            .baseRadius   = radius,
                            .baseDepth    = projDepth,
                            .origin       = exp.origin,
                            .rotation     = decalRot
                        }
                    );
                }

                // Update Particle Emitters
                UpdateGroup(exp.fireball, dt, false);
                UpdateGroup(exp.soilSmoke, dt, true);
                RenderBatchGPU(rc, e, exp);

                // Clean up explosion particle root and children when particles finish
                if (exp.age > exp.duration) {
                    if (exp.debrisEntity != Entity::Null()) {
                        ecb.DestroyEntity(exp.debrisEntity);
                    }
                    ecb.DestroyEntity(e); // Atomically destroys root and its LightComponent
                }
            }
        }

        // --------------------------------------------------------------------
        // 2. UPDATE PERSISTENT CRATER DECALS (Long-lived & Smooth Fade)
        // --------------------------------------------------------------------
        auto craterEntities = reg.GetEntitiesWith<CraterDecalComponent>();
        if (!craterEntities.empty()) {
            auto craters = reg.GetRawArray<CraterDecalComponent>();

            for (size_t i = 0; i < craterEntities.size(); ++i) {
                Entity                craterEnt = craterEntities[i];
                CraterDecalComponent& crater    = craters[i];
                crater.age += dt;

                // Handle smooth dissolution over the final fadeDuration seconds
                if (crater.age >= (crater.lifetime - crater.fadeDuration)) {
                    float remaining = std::max(0.0f, crater.lifetime - crater.age);
                    float fadeRatio = remaining / crater.fadeDuration; // 1.0 -> 0.0

                    float ease          = std::pow(fadeRatio, 0.75f);
                    float currentRadius = crater.baseRadius * ease;
                    float currentDepth  = crater.baseDepth * ease;

                    JPH::Vec3 newScale(currentRadius * 2.0f, currentRadius * 2.0f, currentDepth);

                    reg.Patch<Components::TransformComponent>(craterEnt, [&](auto& trans) { trans.scale = newScale; });
                    reg.Patch<Components::WorldTransformComponent>(craterEnt, [&](auto& wt) {
                        wt.world = Math::CreateTransform(crater.origin, crater.rotation, newScale);
                    });
                }

                if (crater.age >= crater.lifetime) {
                    ecb.DestroyEntity(craterEnt);
                }
            }
        }
    }

  private:
    static void InitArtilleryParticles(ExplosionComponent& exp, std::mt19937& gen) {
        exp.fireball.resize(78);
        for (size_t i = 0; i < exp.fireball.size(); ++i) {
            bool      isRing = (i > 52);
            JPH::Vec3 dir    = isRing ? SampleRingDirection(gen, 82.0f, 94.0f) : SampleConeDirection(gen, 34.0f);
            float     speed  = (isRing ? (10.0f + Hash31(dir * 1.5f) * 6.0f) : (16.0f + Hash31(dir * 1.5f) * 11.0f)) * exp.scale;

            exp.fireball[i] = {
                .position   = dir * (0.1f * exp.scale),
                .velocity   = dir * speed,
                .life       = 0.0f,
                .maxLife    = 0.62f + Hash31(dir * 2.5f) * 0.44f,
                .startSize  = (0.85f + Hash31(dir * 3.5f) * 0.95f) * exp.scale,
                .drag       = 3.0f,
                .gravity    = -4.2f,
                .colorStart = JPH::Vec3(4.0f, 2.8f, 1.2f),
                .colorMid   = JPH::Vec3(2.5f, 1.0f, 0.2f),
                .colorEnd   = JPH::Vec3(0.23f, 0.10f, 0.04f)
            };
        }

        exp.soilSmoke.resize(84);
        for (size_t i = 0; i < exp.soilSmoke.size(); ++i) {
            bool      isRing = (i < 24);
            JPH::Vec3 dir    = isRing ? SampleRingDirection(gen, 84.0f, 92.0f) : SampleConeDirection(gen, 40.0f);
            float     speed  = (isRing ? (7.0f + Hash31(dir * 1.5f) * 4.0f) : (8.0f + Hash31(dir * 1.5f) * 6.0f)) * exp.scale;
            JPH::Vec3 sc     = (Hash31(dir * 6.5f) < 0.3f) ? JPH::Vec3(0.42f, 0.35f, 0.26f) : JPH::Vec3(0.54f, 0.48f, 0.38f);

            exp.soilSmoke[i] = {
                .position   = dir * (0.2f * exp.scale),
                .velocity   = dir * speed,
                .life       = 0.0f,
                .maxLife    = 2.3f + Hash31(dir * 2.5f) * 1.1f,
                .startSize  = (1.55f + Hash31(dir * 3.5f) * 1.35f) * exp.scale,
                .drag       = 1.8f,
                .gravity    = -1.1f,
                .colorStart = JPH::Vec3(1.2f, 0.7f, 0.25f),
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
            JPH::Vec3 dir = RandomInUnitSphere(gen);
            dir.SetY(std::abs(dir.GetY()) * 0.6f + dir.GetY() * 0.4f);

            p = {
                .position   = dir * (0.15f * exp.scale),
                .velocity   = dir * ((3.0f + Hash31(dir * 1.5f) * 6.0f) * exp.scale),
                .life       = 0.0f,
                .maxLife    = 0.7f + Hash31(dir * 2.5f) * 0.7f,
                .startSize  = (0.5f + Hash31(dir * 3.5f) * 0.8f) * exp.scale,
                .drag       = 2.5f,
                .gravity    = -1.0f,
                .colorStart = JPH::Vec3(4.0f, 3.0f, 1.4f),
                .colorMid   = JPH::Vec3(2.2f, 0.9f, 0.25f),
                .colorEnd   = JPH::Vec3(0.4f, 0.06f, 0.02f)
            };
        }

        exp.soilSmoke.resize(80);
        for (auto& p: exp.soilSmoke) {
            JPH::Vec3 dir = RandomInUnitSphere(gen);
            dir.SetY(std::abs(dir.GetY()) * 0.8f + dir.GetY() * 0.2f);
            float gray = 0.05f + Hash31(dir * 6.5f) * 0.15f;

            p = {
                .position   = dir * (0.3f * exp.scale),
                .velocity   = dir * ((1.5f + Hash31(dir * 1.5f) * 3.5f) * exp.scale) + JPH::Vec3(0.0f, 0.8f * exp.scale, 0.0f),
                .life       = 0.0f,
                .maxLife    = 2.5f + Hash31(dir * 2.5f) * 2.0f,
                .startSize  = (1.0f + Hash31(dir * 4.5f) * 1.0f) * exp.scale,
                .drag       = 1.2f,
                .gravity    = 0.6f,
                .colorStart = JPH::Vec3(1.4f, 0.6f, 0.15f),
                .colorMid   = JPH::Vec3(0.35f, 0.25f, 0.2f),
                .colorEnd   = JPH::Vec3(gray, gray, gray * 0.95f)
            };
        }
    }

    static void UpdateGroup(std::vector<ExplosionParticle>& group, float dt, bool isSmoke) noexcept {
        for (auto& p: group) {
            p.life += dt;
            if (p.life > p.maxLife) {
                continue;
            }

            if (isSmoke) {
                p.velocity.SetY(p.velocity.GetY() + 0.58f * dt);
            }

            p.velocity *= std::exp(-p.drag * dt);
            p.velocity.SetY(p.velocity.GetY() + p.gravity * dt);
            p.position += p.velocity * dt;
        }
    }

    static void RenderBatchGPU(RenderContext& rc, Entity e, const ExplosionComponent& exp) {
        thread_local std::vector<Particle> t_gpuScratch;

        // 1. FIREBALL
        if (!exp.fireball.empty()) {
            t_gpuScratch.resize(exp.fireball.size());
            for (size_t i = 0; i < exp.fireball.size(); ++i) {
                const auto& p = exp.fireball[i];
                float       t = std::min(1.0f, p.life / p.maxLife);

                JPH::Vec3 color = (t < 0.52f) ? p.colorStart + (p.colorMid - p.colorStart) * (t / 0.52f) :
                                                p.colorMid + (p.colorEnd - p.colorMid) * ((t - 0.52f) / 0.48f);

                float opacity = (1.0f - t) * 0.98f;
                float size    = p.startSize * (1.0f + t * 1.15f);

                t_gpuScratch[i] = {
                    .position = JPH::Vec4(exp.origin + p.position, 1.0f),
                    .velocity = JPH::Vec4::sZero(),
                    .color    = JPH::Vec4(color * (opacity * 3.5f), opacity),
                    .params   = JPH::Vec4(p.life, p.maxLife, size, 0.0f)
                };
            }

            BufferHandle buf = rc.GetOrCreateParticleBuffer(e.Pack() ^ 0x1111, static_cast<uint32_t>(exp.fireball.size()));
            rc.UpdateBuffer(buf, t_gpuScratch.data(), t_gpuScratch.size() * sizeof(Particle));
            rc.SubmitParticleEmitter(
                buf, static_cast<uint32_t>(exp.fireball.size()),
                {.textureIndex = rc.GetBindlessIndex(s_FireTexHandle), .alignment = ParticleAlignment::CameraBillboard, .blendMode = 1}
            );
        }

        // 2. SOIL SMOKE
        if (!exp.soilSmoke.empty()) {
            t_gpuScratch.resize(exp.soilSmoke.size());
            for (size_t i = 0; i < exp.soilSmoke.size(); ++i) {
                const auto& p       = exp.soilSmoke[i];
                float       opacity = 0.0f;
                float       size    = p.startSize;
                JPH::Vec3   color   = p.colorEnd;

                if (p.life < p.maxLife) {
                    float t = p.life / p.maxLife;
                    color = (t < 0.5f) ? p.colorStart + (p.colorMid - p.colorStart) * (t / 0.5f) : p.colorMid + (p.colorEnd - p.colorMid) * ((t - 0.5f) / 0.5f);
                    opacity = 0.46f * std::pow(1.0f - t, 1.5f);
                    size    = p.startSize * (1.0f + t * 1.95f);
                }

                t_gpuScratch[i] = {
                    .position = JPH::Vec4(exp.origin + p.position, 1.0f),
                    .velocity = JPH::Vec4::sZero(),
                    .color    = JPH::Vec4(color, opacity),
                    .params   = JPH::Vec4(p.life, p.maxLife, size, 0.0f)
                };
            }

            BufferHandle buf = rc.GetOrCreateParticleBuffer(e.Pack() ^ 0x2222, static_cast<uint32_t>(exp.soilSmoke.size()));
            rc.UpdateBuffer(buf, t_gpuScratch.data(), t_gpuScratch.size() * sizeof(Particle));
            rc.SubmitParticleEmitter(
                buf, static_cast<uint32_t>(exp.soilSmoke.size()),
                {.textureIndex = rc.GetBindlessIndex(s_SoilTexHandle), .alignment = ParticleAlignment::CameraBillboard, .blendMode = 0}
            );
        }

        // 3. SHOCKWAVE AIR & GROUND FRONTS
        if (!exp.shockwaves.empty()) {
            // Air Shockwave Billboards (4 Fronts)
            {
                t_gpuScratch.resize(4);
                for (size_t i = 0; i < 4; ++i) {
                    const auto& sw        = exp.shockwaves[i];
                    float       localTime = exp.age - sw.delay;
                    float       opacity   = 0.0f;
                    float       radius    = 0.1f;
                    JPH::Vec3   color     = sw.colorStart;

                    if (localTime > 0.0f && localTime < sw.maxLife) {
                        float t = localTime / sw.maxLife;
                        radius  = (1.0f - std::pow(1.0f - t, 3.0f)) * sw.maxRadius * exp.scale;
                        color   = sw.colorStart + (sw.colorEnd - sw.colorStart) * t;
                        opacity = std::pow(1.0f - t, 1.2f) * 0.45f;
                    }

                    t_gpuScratch[i] = {
                        .position = JPH::Vec4(exp.origin, 1.0f),
                        .velocity = JPH::Vec4::sZero(),
                        .color    = JPH::Vec4(color * (opacity * 3.5f), opacity),
                        .params   = JPH::Vec4(localTime, sw.maxLife, radius * 2.0f, 0.0f)
                    };
                }

                BufferHandle buf = rc.GetOrCreateParticleBuffer(e.Pack() ^ 0x3333, 4);
                rc.UpdateBuffer(buf, t_gpuScratch.data(), 4 * sizeof(Particle));
                rc.SubmitParticleEmitter(
                    buf, 4, {.textureIndex = rc.GetBindlessIndex(s_ShockwaveTexHandle), .alignment = ParticleAlignment::CameraBillboard, .blendMode = 1}
                );
            }

            // Ground Dust Flat Front (1 Plane)
            {
                t_gpuScratch.resize(1);
                const auto& sw        = exp.shockwaves[4];
                float       localTime = exp.age - sw.delay;
                float       opacity   = 0.0f;
                float       radius    = 0.1f;
                JPH::Vec3   color     = sw.colorStart;

                if (localTime > 0.0f && localTime < sw.maxLife) {
                    float t = localTime / sw.maxLife;
                    radius  = (1.0f - std::pow(1.0f - t, 2.2f)) * sw.maxRadius * exp.scale;
                    color   = sw.colorStart + (sw.colorEnd - sw.colorStart) * t;
                    opacity = std::pow(1.0f - t, 1.5f) * 0.35f;
                }

                t_gpuScratch[0] = {
                    .position = JPH::Vec4(exp.origin + JPH::Vec3(0.0f, 0.05f, 0.0f), 1.0f),
                    .velocity = JPH::Vec4::sZero(),
                    .color    = JPH::Vec4(color, opacity),
                    .params   = JPH::Vec4(localTime, sw.maxLife, radius * 2.0f, 0.0f)
                };

                BufferHandle buf = rc.GetOrCreateParticleBuffer(e.Pack() ^ 0x4444, 1);
                rc.UpdateBuffer(buf, t_gpuScratch.data(), 1 * sizeof(Particle));
                rc.SubmitParticleEmitter(
                    buf, 1, {.textureIndex = rc.GetBindlessIndex(s_GroundRingHandle), .alignment = ParticleAlignment::GroundFlat, .blendMode = 1}
                );
            }
        }
    }

    inline static RenderContext* s_LastRenderContext = nullptr;

    // Type-safe registered Texture Handles
    inline static TextureHandle s_FireTexHandle         = TextureHandle::Invalid;
    inline static TextureHandle s_SoilTexHandle         = TextureHandle::Invalid;
    inline static TextureHandle s_ShockwaveTexHandle    = TextureHandle::Invalid;
    inline static TextureHandle s_GroundRingHandle      = TextureHandle::Invalid;
    inline static TextureHandle s_CraterTexHandle       = TextureHandle::Invalid;
    inline static TextureHandle s_CraterNormalTexHandle = TextureHandle::Invalid;

    inline static AssetID    s_DebrisMeshAsset = InvalidAssetID;
    inline static MaterialID s_DebrisMatAsset  = InvalidMaterialID;
};

} // namespace ZHLN
