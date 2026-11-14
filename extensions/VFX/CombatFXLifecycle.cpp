// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module;
// Public Engine & Jolt Headers
// clang-format off
#include <Zahlen/Engine/Capabilities/AudioEngine.hpp>
#include <Zahlen/Engine/Capabilities/DebugDraw.hpp>
#include <Zahlen/Engine/Capabilities/GraphicsDevice.hpp>
// clang-format on
#include <Zahlen/Audio/Audio.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/Ranges.hpp>
#include <Zahlen/Engine/PrefabFactory.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Math/Math3D.hpp>
#include <Zahlen/Math/Transform.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/ECS/EntityCommandBuffer.hpp>
#include <Zahlen/Audio/AudioTypes.hpp>
#include <Zahlen/Render/RenderIDs.hpp>

// Standard Library Headers
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <random>
#include <unordered_map>
#include <vector>

#include <Zahlen/Engine/Capabilities/Entities.hpp>

module ZHLN.CombatFX;

namespace ZHLN::CombatFX {
void System::SpawnImpact(Engine& engine, const FVec3& point, const FVec3& normal, uint32_t surfaceId) {
        auto                   it  = m_surfaces.find(surfaceId);
        const SurfaceResponse& cfg = (it != m_surfaces.end()) ? it->second : m_surfaces[0];

        std::mt19937                          gen(m_randomDevice());
        std::uniform_real_distribution<float> dis(-1.0f, 1.0f);

        // 1. Spawn Decal via ECS DecalComponent (Heightmap & Geometry Agnostic)
        if (cfg.spawnDecal && cfg.decalTexture != TextureID::Invalid) {
            auto& ecb = engine.Commands();

            FVec3 zAxis = -normal.Normalized();
            FVec3 up    = (std::abs(zAxis.y) > 0.95f) ? FVec3::UnitX() : FVec3::UnitY();
            FVec3 xAxis = Cross(up, zAxis).Normalized();
            FVec3 yAxis = Cross(zAxis, xAxis).Normalized();

            const auto rotMat = Mat4x4::FromCols(FVec4{xAxis, 0.0f}, FVec4{yAxis, 0.0f}, FVec4{zAxis, 0.0f}, FVec4{0.0f, 0.0f, 0.0f, 1.0f});
            const auto decalRot = DecomposeTransform(rotMat).rotation;

            FVec3  decalScale(cfg.decalSize, cfg.decalSize, 0.40f);
            const auto worldMat = ComposeTransform(point, decalRot, decalScale);

            Entity decalEnt = engine.Entities().Create();
            ecb.AddComponent(
                decalEnt, Components::NameComponent {.name = String64("ImpactDecal")},
                Components::TransformComponent {.position = point, .rotation = decalRot, .scale = decalScale},
                Components::WorldTransformComponent {.world = worldMat, .previous = worldMat},
                Components::DecalComponent {.albedoMap = cfg.decalTexture, .normalMap = cfg.normalTexture, .roughness = cfg.decalRoughness, .metallic = 0.0f}
            );
        }

        // 2. Spawn Particle Spray
        for (uint32_t i = 0; i < cfg.sparkCount; ++i) {
            FVec3 randSpread(dis(gen), (dis(gen) + 1.0f) * 0.5f, dis(gen));
            FVec3 vel = (normal * (cfg.sparkSpeed * 0.6f)) + (randSpread.Normalized() * (cfg.sparkSpeed * 0.8f));

            m_particles.push_back(
                ImpactParticle {
                    .position = point,
                    .velocity = vel,
                    .color    = cfg.sparkColor,
                    .size     = cfg.sparkSize * (0.8f + (dis(gen) + 1.0f) * 0.3f),
                    .age      = 0.0f,
                    .maxLife  = cfg.sparkLifetime * (0.7f + (dis(gen) + 1.0f) * 0.4f),
                    .drag     = cfg.sparkDrag,
                    .gravity  = cfg.sparkGravity
                }
            );
        }

        // 3. Acoustic Audio Feedback via Unified Fire-and-Forget Event Queue
        if (cfg.hasAudio) {
            engine.Audio().PostEvent(
                {.type       = AudioEventType::NoiseBurst3D,
                 .position   = point,
                 .volume     = 0.65f,
                 .param1     = cfg.soundFreq,
                 .param2     = 2.5f,
                 .duration   = cfg.soundDuration,
                 .filterType = AudioFilterType::BandPass,
                 .noiseType  = AudioNoiseType::Pink}
            );
        }
    }
}
