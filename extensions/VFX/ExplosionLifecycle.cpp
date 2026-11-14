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

#include <Zahlen/Engine/Capabilities/Entities.hpp>

module ZHLN.Explosions;

namespace ZHLN {

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


void ExplosionSystem::Detach(Engine& engine, Entity entity) {
        engine.Entities().Detach<ExplosionComponent>(entity);
    }

void ExplosionSystem::Init(Engine& engine) {
        auto  rc  = engine.Graphics();
        auto entities = engine.Entities();

        // Register ECS components for the current registry instance
        entities.Register<ExplosionComponent>("ExplosionComponent");
        entities.Register<CraterDecalComponent>("CraterDecalComponent");

        // No scene-cleanup pass: there is no GPU state of this system's to release.
        // The one thing that must not survive a device loss is the cached context
        // pointer, and the renderer is the one that lost the device.
        engine.AddDeviceLostCallback([](Engine& owner) { s_LastEngine = nullptr; });

        s_DebrisMeshAsset = HashAssetID("artillery_debris_mesh");
        s_DebrisMatAsset  = HashAssetID("artillery_debris_mat");

        // Debris is shared by every explosion in this scene, not owned by any
        // one emitter. Keep its buffers on a dedicated ECS resource entity.
        Entity resourceEntity = Entity::Null();
        for (const Entity e: entities.EntitiesWith<Components::OwnedMeshComponent>()) {
            if (const auto owned = entities.Get<Components::OwnedMeshComponent>(e); owned && owned->meshAsset == s_DebrisMeshAsset) {
                resourceEntity = e;
                break;
            }
        }
        if (s_LastEngine == &engine && resourceEntity != Entity::Null() && rc.Mesh(s_DebrisMeshAsset).has_value() &&
            rc.Material(s_DebrisMatAsset).has_value()) {
            return;
        }
        s_LastEngine = &engine;

        // Register procedural textures into TextureManager
        s_FireTexID         = rc.CreateProceduralTexture("vfx_artillery_fire", {256, 256}, GenerateFireTexture(256), true);
        s_SoilTexID         = rc.CreateProceduralTexture("vfx_artillery_soil", {256, 256}, GenerateSoilTexture(256), true);
        s_ShockwaveTexID    = rc.CreateProceduralTexture("vfx_artillery_shockwave", {512, 512}, GenerateShockwaveRingTexture(512), true);
        s_GroundRingID      = rc.CreateProceduralTexture("vfx_artillery_ground_ring", {512, 512}, GenerateGroundRingTexture(512), true);
        s_CraterTexID       = rc.CreateProceduralTexture("vfx_artillery_crater", {256, 256}, GenerateCraterTexture(256), true);
        s_CraterNormalTexID = rc.CreateProceduralTexture("vfx_artillery_crater_norm", {256, 256}, GenerateCraterNormalTexture(256), false);

        Mesh boxMesh;
        // Registry::Get answers nullopt when the entity has no such component, and
        // this used to be dereferenced on the strength of the entity merely being
        // non-null -- which is the null dereference GCC reports. A missing
        // component now takes the same path as a missing entity: the debris mesh
        // the asset is registered from gets created either way.
        auto owned = resourceEntity != Entity::Null() ? entities.Get<Components::OwnedMeshComponent>(resourceEntity) :
                                                        ZHLN::Optional<const Components::OwnedMeshComponent&> {};
        if (owned) {
            boxMesh = owned->mesh;
            if (boxMesh.posBuffer == BufferID::Invalid) {
                boxMesh = PrefabFactory::CreateBoxMesh(engine.Graphics(), {0.5f, 0.5f, 0.5f}, {0.28f, 0.22f, 0.16f, 1.0f});
                auto replacement = *owned;
                replacement.mesh = boxMesh;
                entities.Attach(resourceEntity, std::move(replacement));
            }
        } else {
            boxMesh = PrefabFactory::CreateBoxMesh(engine.Graphics(), {0.5f, 0.5f, 0.5f}, {0.28f, 0.22f, 0.16f, 1.0f});
            entities.Spawn(Components::OwnedMeshComponent {.meshAsset = s_DebrisMeshAsset, .mesh = boxMesh});
        }
        rc.RegisterMesh(s_DebrisMeshAsset, boxMesh);

        if (!rc.Material(s_DebrisMatAsset).has_value()) {
            Material debrisMat = rc.CreateMaterial({
                                                       .roughness = 0.94f,
                                                       .baseColor = {0.28f, 0.22f, 0.16f, 1.0f},
                                                   })
                                     .value_or(Material {});
            rc.RegisterMaterial(s_DebrisMatAsset, debrisMat);
        }
    }

void ExplosionSystem::Attach(Engine& engine, Entity entity, ExplosionComponent component) {
        Init(engine);
        Detach(engine, entity);
        engine.Entities().Attach(entity, std::move(component));
    }

Entity ExplosionSystem::Spawn(Engine& engine, ZHLN::FVec3 origin, float scale, OrdnanceType type) {
        auto entities = engine.Entities();
        Init(engine);

        ExplosionComponent initialExp {
            .type = type, .origin = origin, .scale = scale, .duration = (type == OrdnanceType::ArtilleryMortar) ? 3.5f : 2.5f
        };

        std::mt19937 gen(std::random_device {}());
        if (type == OrdnanceType::ArtilleryMortar) {
            InitArtilleryParticles(initialExp, gen);
        } else {
            InitStandardFireballParticles(initialExp, gen);
        }

        Entity root         = entities.Create();
        Entity debrisEntity = (type == OrdnanceType::ArtilleryMortar) ? entities.Create() : Entity::Null();

        initialExp.debrisEntity = debrisEntity;

        // Position light directly at origin with slight vertical offset
        FVec3  lightWorldPos = origin + FVec3{0.0f, 1.8f * scale, 0.0f};
        const auto rootTransform = CreateTranslation(lightWorldPos);

        // Attach synchronously to root entity
        entities.Attach(
            root, Components::NameComponent {.name = String64("ArtilleryExplosionRoot")}, Components::TransformComponent {.position = lightWorldPos},
            Components::WorldTransformComponent {.world = rootTransform, .previous = rootTransform},
            Components::LightComponent {
                .type = LightType::Point, .color = FVec3{1.0f, 0.48f, 0.09f}, .intensity = 2800.0f * scale * scale, .radius = 1.5f, .range = 60.0f * scale
            },
            std::move(initialExp)
        );

        if (debrisEntity != Entity::Null()) {
            entities.Attach(
                debrisEntity, Components::NameComponent {.name = String64("Artillery3DDebris")},
                Components::TransformComponent {.position = origin},
                Components::MeshParticleEmitterComponent {
                    .meshAsset     = s_DebrisMeshAsset,
                    .materialAsset = s_DebrisMatAsset,
                    .maxParticles  = 48,
                    .active        = true,
                    .params =
                        {.gravity     = {0.0f, -18.0f * scale, 0.0f},
                         .drag        = 0.32f,
                         .spawnOrigin = origin,
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
        auto audio = engine.Audio();
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



}
