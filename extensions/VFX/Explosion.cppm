// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module;

// Public Engine & Jolt Headers
// clang-format off
#include <Zahlen/Physics/Jolt.hpp>
#include <Zahlen/Engine/Capabilities/AudioEngine.hpp>
#include <Zahlen/Engine/Capabilities/DebugDraw.hpp>
#include <Zahlen/Engine/Capabilities/GraphicsDevice.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
// clang-format on
#include <Zahlen/Audio/Audio.hpp>
#include <Zahlen/Audio/AudioTypes.hpp>
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/ECS/EntityCommandBuffer.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Engine/PrefabFactory.hpp>
#include <Zahlen/Math/Math3D.hpp>
#include <Zahlen/Math/Transform.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Math/Angles.hpp>
#include <Zahlen/Render/GpuEnums.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Render/Types.hpp>

// Standard Library Headers
#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <span>
#include <utility>
#include <vector>

export module ZHLN.Explosions;

namespace ZHLN {

// ============================================================================
// Ordnance Types
// ============================================================================

export enum class OrdnanceType : uint8_t {
    StandardFireball, // Standard omnidirectional fireball
    ArtilleryMortar   // Anisotropic cone, shockwave rings, heavy soil ejecta, crater decal & blast impulse
};

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
    const auto theta = RadF{2.0f * std::numbers::pi_v<float> * u};
    float                                 phi   = std::acos(2.0f * v - 1.0f);
    float                                 r     = std::cbrt(dis(gen));
    return {r * std::sin(phi) * Cos(theta), r * std::sin(phi) * Sin(theta), r * std::cos(phi)};
}

[[nodiscard]] static FVec3 SampleConeDirection(std::mt19937& gen, float maxAngleDeg) noexcept {
    std::uniform_real_distribution<float> dis(0.0f, 1.0f);
    const auto theta = DegF{dis(gen) * maxAngleDeg}.ToRadians();
    float                                 phi   = dis(gen) * 2.0f * std::numbers::pi_v<float>;
    float                                 sinT  = Sin(theta);
    float                                 cosT  = Cos(theta);

    return FVec3{sinT * std::cos(phi), std::abs(cosT) + 0.06f, sinT * std::sin(phi)}.Normalized();
}

[[nodiscard]] static FVec3 SampleRingDirection(std::mt19937& gen, float minAngleDeg, float maxAngleDeg) noexcept {
    std::uniform_real_distribution<float> dis(0.0f, 1.0f);
    const auto theta = DegF{minAngleDeg + dis(gen) * (maxAngleDeg - minAngleDeg)}.ToRadians();
    float                                 phi   = dis(gen) * 2.0f * std::numbers::pi_v<float>;
    float                                 sinT  = Sin(theta);
    float                                 cosT  = Cos(theta);

    return FVec3{sinT * std::cos(phi), std::abs(cosT) * 0.2f + 0.02f, sinT * std::sin(phi)}.Normalized();
}

// ============================================================================
// Component Definitions
// ============================================================================

struct ShockwaveParticle {
    float     delay     = 0.0f;
    float     maxLife   = 0.5f;
    float     maxRadius = 18.0f;
    float     speed     = 1.0f;
    FVec3 colorStart {};
    FVec3 colorEnd {};
    bool      isGround = false;
};

struct ExplosionParticle {
    FVec3 position {};
    FVec3 velocity {};
    float     life      = 0.0f;
    float     maxLife   = 1.0f;
    float     startSize = 1.0f;
    float     drag      = 1.0f;
    float     gravity   = 0.0f;
    FVec3 colorStart {};
    FVec3 colorMid {};
    FVec3 colorEnd {};
};

export struct ExplosionComponent {
    OrdnanceType type     = OrdnanceType::ArtilleryMortar;
    FVec3    origin   = FVec3::Zero();
    float        scale    = 1.0f;
    float        age      = 0.0f;
    float        duration = 3.5f;

    std::vector<ExplosionParticle> fireball;
    std::vector<ExplosionParticle> soilSmoke;
    std::vector<ShockwaveParticle> shockwaves;

    Entity debrisEntity  = Entity::Null();
    bool   craterSpawned = false;

    // No GPU storage: the particles above are the simulation's own state, and
    // rendering them is RenderBatchGPU describing world-space quads for
    // SceneData (ApplyDrawData flushes them after BeginFrame).
};

export struct CraterDecalComponent {
    float     age          = 0.0f;
    float     lifetime     = 28.0f; // Remains on terrain for 28 seconds
    float     fadeDuration = 6.0f;  // Dissolves smoothly over the final 6 seconds
    float     baseRadius   = 3.4f;
    float     baseDepth    = 5.0f;
    FVec3 origin       = FVec3::Zero();
    Quat rotation = Quat::Identity();
};

// ============================================================================
// Explosion System Subsystem
// ============================================================================

export class ExplosionSystem {
  public:
    // No Release and no cleanup pass: the component holds nothing on the GPU, and
    // the renderer releases what it allocated for described quads on its own
    // schedule. Detaching is an ECS mutation and nothing else.
    static void Detach(Engine& engine, Entity entity);

    static void Init(Engine& engine);

    static void Attach(Engine& engine, Entity entity, ExplosionComponent component);

    static Entity Spawn(Engine& engine, ZHLN::FVec3 origin, float scale = 1.0f, OrdnanceType type = OrdnanceType::ArtilleryMortar);

    // The typed ECS update lives outside this module interface so its query
    // definitions do not enter the module's global fragment.
    static void AdvanceAndRender(ExplosionComponent& exp, SceneData& scene, float dt) {
        UpdateGroup(exp.fireball, dt, false);
        UpdateGroup(exp.soilSmoke, dt, true);
        RenderBatchGPU(scene, exp);
    }
    static float EffectHash(FVec3 position) noexcept { return Hash31(position); }
    static auto CraterTextures() noexcept -> std::pair<TextureID, TextureID> { return {s_CraterTexID, s_CraterNormalTexID}; }

  private:
    static void InitArtilleryParticles(ExplosionComponent& exp, std::mt19937& gen) {
        exp.fireball.resize(78);
        for (size_t i = 0; i < exp.fireball.size(); ++i) {
            bool      isRing = (i > 52);
            FVec3 dir    = isRing ? SampleRingDirection(gen, 82.0f, 94.0f) : SampleConeDirection(gen, 34.0f);
            float     speed  = (isRing ? (10.0f + Hash31(dir * 1.5f) * 6.0f) : (16.0f + Hash31(dir * 1.5f) * 11.0f)) * exp.scale;

            exp.fireball[i] = {
                .position   = dir * (0.1f * exp.scale),
                .velocity   = dir * speed,
                .life       = 0.0f,
                .maxLife    = 0.62f + Hash31(dir * 2.5f) * 0.44f,
                .startSize  = (0.85f + Hash31(dir * 3.5f) * 0.95f) * exp.scale,
                .drag       = 3.0f,
                .gravity    = -4.2f,
                .colorStart = FVec3{4.0f, 2.8f, 1.2f},
                .colorMid   = FVec3{2.5f, 1.0f, 0.2f},
                .colorEnd   = FVec3{0.23f, 0.10f, 0.04f}
            };
        }

        exp.soilSmoke.resize(84);
        for (size_t i = 0; i < exp.soilSmoke.size(); ++i) {
            bool      isRing = (i < 24);
            FVec3 dir    = isRing ? SampleRingDirection(gen, 84.0f, 92.0f) : SampleConeDirection(gen, 40.0f);
            float     speed  = (isRing ? (7.0f + Hash31(dir * 1.5f) * 4.0f) : (8.0f + Hash31(dir * 1.5f) * 6.0f)) * exp.scale;
            FVec3 sc     = (Hash31(dir * 6.5f) < 0.3f) ? FVec3{0.42f, 0.35f, 0.26f} : FVec3{0.54f, 0.48f, 0.38f};

            exp.soilSmoke[i] = {
                .position   = dir * (0.2f * exp.scale),
                .velocity   = dir * speed,
                .life       = 0.0f,
                .maxLife    = 2.3f + Hash31(dir * 2.5f) * 1.1f,
                .startSize  = (1.55f + Hash31(dir * 3.5f) * 1.35f) * exp.scale,
                .drag       = 1.8f,
                .gravity    = -1.1f,
                .colorStart = FVec3{1.2f, 0.7f, 0.25f},
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
            dir.y = std::abs(dir.y) * 0.6f + dir.y * 0.4f;

            p = {
                .position   = dir * (0.15f * exp.scale),
                .velocity   = dir * ((3.0f + Hash31(dir * 1.5f) * 6.0f) * exp.scale),
                .life       = 0.0f,
                .maxLife    = 0.7f + Hash31(dir * 2.5f) * 0.7f,
                .startSize  = (0.5f + Hash31(dir * 3.5f) * 0.8f) * exp.scale,
                .drag       = 2.5f,
                .gravity    = -1.0f,
                .colorStart = FVec3{4.0f, 3.0f, 1.4f},
                .colorMid   = FVec3{2.2f, 0.9f, 0.25f},
                .colorEnd   = FVec3{0.4f, 0.06f, 0.02f}
            };
        }

        exp.soilSmoke.resize(80);
        for (auto& p: exp.soilSmoke) {
            FVec3 dir = RandomInUnitSphere(gen);
            dir.y = std::abs(dir.y) * 0.8f + dir.y * 0.2f;
            float gray = 0.05f + Hash31(dir * 6.5f) * 0.15f;

            p = {
                .position   = dir * (0.3f * exp.scale),
                .velocity   = dir * ((1.5f + Hash31(dir * 1.5f) * 3.5f) * exp.scale) + FVec3{0.0f, 0.8f * exp.scale, 0.0f},
                .life       = 0.0f,
                .maxLife    = 2.5f + Hash31(dir * 2.5f) * 2.0f,
                .startSize  = (1.0f + Hash31(dir * 4.5f) * 1.0f) * exp.scale,
                .drag       = 1.2f,
                .gravity    = 0.6f,
                .colorStart = FVec3{1.4f, 0.6f, 0.15f},
                .colorMid   = FVec3{0.35f, 0.25f, 0.2f},
                .colorEnd   = FVec3{gray, gray, gray * 0.95f}
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

    static void RenderBatchGPU(SceneData& scene, ExplosionComponent& exp) {
        // The simulation above is the CPU's; this describes what it produced in
        // world-space quads and hands them over. SceneData holds the batches until
        // ApplyDrawData; the renderer owns GPU storage after BeginFrame.
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
                     .color    = FVec4{color * (opacity * 3.5f), opacity},
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
                float     opacity = 0.0f;
                float     size    = p.startSize;
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
                     .color    = FVec4{color, opacity},
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
                    FVec3   color     = sw.colorStart;

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
                         .color    = FVec4{color * (opacity * 3.5f), opacity},
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
                FVec3   color     = sw.colorStart;

                if (localTime > 0.0f && localTime < sw.maxLife) {
                    float t = localTime / sw.maxLife;
                    radius  = (1.0f - std::pow(1.0f - t, 2.2f)) * sw.maxRadius * exp.scale;
                    color   = sw.colorStart + (sw.colorEnd - sw.colorStart) * t;
                    opacity = std::pow(1.0f - t, 1.5f) * 0.35f;
                }

                const BillboardQuad quad {
                    .position = exp.origin + FVec3{0.0f, 0.05f, 0.0f},
                    .size     = radius * 2.0f,
                    .rotation = 0.0f,
                    .color    = FVec4{color, opacity},
                    .facing   = ParticleAlignment::GroundFlat,
                };
                scene.AddBillboards(s_GroundRingID, std::span {&quad, 1}, true);
            }
        }
    }

    // Declared here, defined once below in this module's own purview. `inline
    // static` is how a header defines a static data member in-class, and in a
    // module interface unit it is a trap rather than a convenience: no symbol is
    // emitted for the definition, so an importer that inlines one of the member
    // bodies above is left holding an undefined reference. A single definition
    // in the module's translation unit needs no help from `inline`.
    static Engine* s_LastEngine;

    // Type-safe registered Texture Handles
    static TextureID s_FireTexID;
    static TextureID s_SoilTexID;
    static TextureID s_ShockwaveTexID;
    static TextureID s_GroundRingID;
    static TextureID s_CraterTexID;
    static TextureID s_CraterNormalTexID;

    static AssetID    s_DebrisMeshAsset;
    static MaterialID s_DebrisMatAsset;
};

Engine* ExplosionSystem::s_LastEngine = nullptr;

TextureID ExplosionSystem::s_FireTexID         = TextureID::Invalid;
TextureID ExplosionSystem::s_SoilTexID         = TextureID::Invalid;
TextureID ExplosionSystem::s_ShockwaveTexID    = TextureID::Invalid;
TextureID ExplosionSystem::s_GroundRingID      = TextureID::Invalid;
TextureID ExplosionSystem::s_CraterTexID       = TextureID::Invalid;
TextureID ExplosionSystem::s_CraterNormalTexID = TextureID::Invalid;

AssetID    ExplosionSystem::s_DebrisMeshAsset = InvalidAssetID;
MaterialID ExplosionSystem::s_DebrisMatAsset  = InvalidMaterialID;

} // namespace ZHLN
