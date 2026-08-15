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
#include <Zahlen/Core/Ranges.hpp>
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
#include <cstdint>
#include <numbers>
#include <random>
#include <unordered_map>
#include <vector>

export module ZHLN.CombatFX;

export namespace ZHLN::CombatFX {

// ============================================================================
// Data-Driven Surface & Impact Definitions
// ============================================================================

/**
 * @brief Configurable response when a projectile or beam impacts a surface.
 */
struct SurfaceResponse {
    TextureHandle decalTexture   = TextureHandle::Invalid;
    TextureHandle normalTexture  = TextureHandle::Invalid;
    float         decalSize      = 0.28f;
    float         decalLifetime  = 15.0f;
    float         decalRoughness = 0.90f;
    bool          spawnDecal     = true;

    JPH::Vec4 sparkColor    = JPH::Vec4(1.0f, 0.82f, 0.55f, 1.0f);
    uint32_t  sparkCount    = 8;
    float     sparkSpeed    = 4.5f;
    float     sparkLifetime = 0.35f;
    float     sparkSize     = 0.035f;
    float     sparkGravity  = -14.0f;
    float     sparkDrag     = 1.5f;

    // Optional audio parameters (Hz, Q, Duration)
    bool  hasAudio      = true;
    float soundFreq     = 280.0f;
    float soundDuration = 0.12f;
};

// ============================================================================
// Transient Visual Effects (Tracers, Rings, Particles)
// ============================================================================

struct BulletTracer {
    JPH::Vec3 start         = JPH::Vec3::sZero();
    JPH::Vec3 direction     = JPH::Vec3::sAxisZ();
    float     speed         = 350.0f;
    float     length        = 2.5f;
    float     totalDistance = 0.0f;
    float     traveled      = 0.0f;
    JPH::Vec4 colorStart    = JPH::Vec4(1.0f, 0.92f, 0.65f, 1.0f);
    JPH::Vec4 colorEnd      = JPH::Vec4(1.0f, 0.45f, 0.10f, 0.0f);
};

struct EnergyRing {
    JPH::Vec3 position  = JPH::Vec3::sZero();
    JPH::Vec3 normal    = JPH::Vec3::sAxisY();
    float     radius    = 0.0f;
    float     maxRadius = 2.5f;
    float     age       = 0.0f;
    float     duration  = 0.25f;
    JPH::Vec4 color     = JPH::Vec4(0.65f, 0.90f, 1.00f, 0.8f);
};

struct ImpactParticle {
    JPH::Vec3 position = JPH::Vec3::sZero();
    JPH::Vec3 velocity = JPH::Vec3::sZero();
    JPH::Vec4 color    = JPH::Vec4::sReplicate(1.0f);
    float     size     = 0.04f;
    float     age      = 0.0f;
    float     maxLife  = 0.35f;
    float     drag     = 1.5f;
    float     gravity  = -12.0f;
};

// ============================================================================
// High-Level CombatFX Subsystem Manager
// ============================================================================

class System {
  public:
    System() = default;

    /**
     * @brief Registers default procedural textures (bullet hole, spark shapes).
     */
    void Init(Engine& engine) {
        if (m_initialized) {
            return;
        }
        m_initialized = true;

        auto& rc = engine.GetRenderContext();

        // 1. Build default bullet hole texture
        m_defaultHoleTex = rc.CreateProceduralTexture("vfx_combat_bullethole", 128, 128, true, GenerateBulletHoleTexture(128).data());

        // 2. Setup standard default surface presets
        // Preset 0: Generic Solid / Concrete
        RegisterSurface(
            0, SurfaceResponse {
                   .decalTexture = m_defaultHoleTex,
                   .decalSize    = 0.28f,
                   .sparkColor   = {0.95f, 0.90f, 0.80f, 1.0f},
                   .sparkCount   = 8,
                   .sparkSpeed   = 4.5f,
                   .soundFreq    = 240.0f
               }
        );

        // Preset 1: Flesh / Organic (Red splatter, no decal)
        RegisterSurface(
            1, SurfaceResponse {
                   .spawnDecal    = false,
                   .sparkColor    = {0.60f, 0.08f, 0.08f, 0.95f},
                   .sparkCount    = 14,
                   .sparkSpeed    = 3.0f,
                   .sparkLifetime = 0.50f,
                   .sparkSize     = 0.05f,
                   .soundFreq     = 160.0f
               }
        );

        // Preset 2: Metal (Bright hot sparks + ping)
        RegisterSurface(
            2, SurfaceResponse {
                   .decalTexture = m_defaultHoleTex,
                   .decalSize    = 0.20f,
                   .sparkColor   = {1.0f, 0.75f, 0.30f, 1.0f},
                   .sparkCount   = 16,
                   .sparkSpeed   = 6.5f,
                   .soundFreq    = 850.0f
               }
        );

        // Preset 3: Energy Shield (Blue/Cyan flare)
        RegisterSurface(
            3, SurfaceResponse {
                   .spawnDecal   = false,
                   .sparkColor   = {0.35f, 0.85f, 1.0f, 0.95f},
                   .sparkCount   = 12,
                   .sparkSpeed   = 5.0f,
                   .sparkGravity = 0.0f, // Defies gravity
                   .soundFreq    = 600.0f
               }
        );
    }

    /**
     * @brief Configures or overrides an impact surface preset.
     */
    void RegisterSurface(uint32_t surfaceId, const SurfaceResponse& config) {
        m_surfaces[surfaceId] = config;
    }

    /**
     * @brief Spawns visual and acoustic impact effects given a hit position and normal.
     */
    void SpawnImpact(Engine& engine, const JPH::Vec3& point, const JPH::Vec3& normal, uint32_t surfaceId = 0) {
        auto                   it  = m_surfaces.find(surfaceId);
        const SurfaceResponse& cfg = (it != m_surfaces.end()) ? it->second : m_surfaces[0];

        std::mt19937                          gen(m_randomDevice());
        std::uniform_real_distribution<float> dis(-1.0f, 1.0f);

        // 1. Spawn Decal via ECS DecalComponent (Heightmap & Geometry Agnostic)
        if (cfg.spawnDecal && cfg.decalTexture != TextureHandle::Invalid) {
            auto& reg = engine.GetRegistry();
            auto& ecb = engine.GetMainECB();

            JPH::Vec3 zAxis = -normal.Normalized();
            JPH::Vec3 up    = (std::abs(zAxis.GetY()) > 0.95f) ? JPH::Vec3::sAxisX() : JPH::Vec3::sAxisY();
            JPH::Vec3 xAxis = up.Cross(zAxis).Normalized();
            JPH::Vec3 yAxis = zAxis.Cross(xAxis).Normalized();

            JPH::Mat44 rotMat(JPH::Vec4(xAxis, 0.0f), JPH::Vec4(yAxis, 0.0f), JPH::Vec4(zAxis, 0.0f), JPH::Vec4(0.0f, 0.0f, 0.0f, 1.0f));
            JPH::Quat  decalRot = rotMat.GetQuaternion().Normalized();

            JPH::Vec3  decalScale(cfg.decalSize, cfg.decalSize, 0.40f);
            JPH::Mat44 worldMat = Math::CreateTransform(point, decalRot, decalScale);

            Entity decalEnt = reg.Create();
            ecb.AddComponent(
                decalEnt, Components::NameComponent {.name = String64("ImpactDecal")},
                Components::TransformComponent {.position = point, .rotation = decalRot, .scale = decalScale},
                Components::WorldTransformComponent {.world = worldMat, .previous = worldMat},
                Components::DecalComponent {.albedoMap = cfg.decalTexture, .normalMap = cfg.normalTexture, .roughness = cfg.decalRoughness, .metallic = 0.0f}
            );
        }

        // 2. Spawn Particle Spray
        for (uint32_t i = 0; i < cfg.sparkCount; ++i) {
            JPH::Vec3 randSpread(dis(gen), (dis(gen) + 1.0f) * 0.5f, dis(gen));
            JPH::Vec3 vel = (normal * (cfg.sparkSpeed * 0.6f)) + (randSpread.Normalized() * (cfg.sparkSpeed * 0.8f));

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

        // 3. Acoustic Audio Feedback
        if (cfg.hasAudio) {
            engine.GetAudioContext().PlayNoiseBurst3D(AudioFilterType::BandPass, cfg.soundFreq, 2.5f, 0.65f, cfg.soundDuration, point, AudioNoiseType::Pink);
        }
    }

    /**
     * @brief Spawns a high-speed ballistic tracer beam.
     */
    void SpawnTracer(
        const JPH::Vec3& from,
        const JPH::Vec3& to,
        float            speed      = 320.0f,
        float            length     = 2.2f,
        const JPH::Vec4& colorStart = {1.0f, 0.95f, 0.65f, 1.0f},
        const JPH::Vec4& colorEnd   = {1.0f, 0.45f, 0.10f, 0.0f}
    ) {
        JPH::Vec3 diff = to - from;
        float     dist = diff.Length();
        if (dist <= 0.01f) {
            return;
        }

        m_tracers.push_back(
            BulletTracer {
                .start         = from,
                .direction     = diff / dist,
                .speed         = speed,
                .length        = length,
                .totalDistance = dist,
                .traveled      = 0.0f,
                .colorStart    = colorStart,
                .colorEnd      = colorEnd
            }
        );
    }

    /**
     * @brief Spawns an expanding energy/kinetic shockwave ring.
     */
    void SpawnShockwaveRing(
        const JPH::Vec3& position,
        const JPH::Vec3& normal,
        float            maxRadius = 2.5f,
        float            duration  = 0.25f,
        const JPH::Vec4& color     = {0.65f, 0.90f, 1.00f, 0.8f}
    ) {
        m_rings.push_back(
            EnergyRing {
                .position = position, .normal = normal.Normalized(), .radius = 0.05f, .maxRadius = maxRadius, .age = 0.0f, .duration = duration, .color = color
            }
        );
    }

    /**
     * @brief Simulates active effects and submits GPU batches.
     */
    void Update(Engine& engine, float dt) {
        auto& rc = engine.GetRenderContext();

        // 1. Advance and Render Tracers via Zero-Allocation Line Pipeline (rc.DrawLine)
        for (auto& tracer: m_tracers) {
            tracer.traveled += tracer.speed * dt;
            float head = std::min(tracer.traveled, tracer.totalDistance);
            float tail = std::max(0.0f, tracer.traveled - tracer.length);

            if (head > tail) {
                JPH::Vec3 pTail = tracer.start + (tracer.direction * tail);
                JPH::Vec3 pHead = tracer.start + (tracer.direction * head);

                // Draws line segment with HDR emissive boost into line queue
                rc.DrawLine(pTail, pHead, tracer.colorEnd, tracer.colorStart);
            }
        }
        ZHLN::Ranges::EraseIf(m_tracers, [](const auto& t) -> auto { return (t.traveled - t.length) >= t.totalDistance; });

        // 2. Advance and Render Expanding Shockwave Rings
        for (auto& ring: m_rings) {
            ring.age += dt;
            float t             = std::min(1.0f, ring.age / ring.duration);
            float currentRadius = ring.maxRadius * std::sin(t * (std::numbers::pi_v<float> * 0.5f));

            // Approximate ring with 16-segment line loop
            constexpr int kSegments = 16;
            JPH::Vec3     u         = (std::abs(ring.normal.GetY()) > 0.95f) ? JPH::Vec3::sAxisX() : JPH::Vec3::sAxisY();
            JPH::Vec3     v         = ring.normal.Cross(u).Normalized();
            u                       = v.Cross(ring.normal).Normalized();

            float     alpha = (1.0f - t) * ring.color.GetW();
            JPH::Vec4 col(ring.color.GetX(), ring.color.GetY(), ring.color.GetZ(), alpha);

            JPH::Vec3 prevP = ring.position + u * currentRadius;
            for (int i = 1; i <= kSegments; ++i) {
                float     theta = (static_cast<float>(i) / kSegments) * 2.0f * std::numbers::pi_v<float>;
                JPH::Vec3 p     = ring.position + (u * std::cos(theta) + v * std::sin(theta)) * currentRadius;
                rc.DrawLine(prevP, p, col, col);
                prevP = p;
            }
        }
        ZHLN::Ranges::EraseIf(m_rings, [](const auto& r) -> auto { return r.age >= r.duration; });

        // 3. Advance Impact Particles (Generalized drag + gravity without planar floor assumption)
        for (auto& p: m_particles) {
            p.age += dt;
            p.velocity.SetY(p.velocity.GetY() + (p.gravity * dt));
            p.velocity *= std::exp(-p.drag * dt);
            p.position += p.velocity * dt;

            // Render spark as short directional streak along velocity vector
            float speed = p.velocity.Length();
            if (speed > 0.05f) {
                JPH::Vec3 streakTail = p.position - (p.velocity / speed) * (p.size * 1.5f);
                float     alpha      = std::max(0.0f, 1.0f - (p.age / p.maxLife)) * p.color.GetW();
                JPH::Vec4 col(p.color.GetX(), p.color.GetY(), p.color.GetZ(), alpha);

                rc.DrawLine(streakTail, p.position, col, col);
            }
        }
        ZHLN::Ranges::EraseIf(m_particles, [](const auto& p) -> auto { return p.age >= p.maxLife; });
    }

    void Clear() noexcept {
        m_tracers.clear();
        m_rings.clear();
        m_particles.clear();
    }

  private:
    static auto GenerateBulletHoleTexture(uint32_t size) -> std::vector<uint32_t> {
        std::vector<uint32_t> pixels(static_cast<size_t>(size * size));
        const float           center = size * 0.5f;

        for (uint32_t y = 0; y < size; ++y) {
            for (uint32_t x = 0; x < size; ++x) {
                float dx = (static_cast<float>(x) - center) / center;
                float dy = (static_cast<float>(y) - center) / center;
                float r  = std::sqrt(dx * dx + dy * dy);

                float   alpha = (r <= 0.25f) ? 0.95f : ((r <= 0.85f) ? (0.95f * (1.0f - (r - 0.25f) / 0.60f)) : 0.0f);
                uint8_t col   = (r <= 0.25f) ? 14 : 32;

                uint8_t a            = static_cast<uint8_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
                pixels[y * size + x] = (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(col) << 16) | (static_cast<uint32_t>(col) << 8) | col;
            }
        }
        return pixels;
    }

    bool                                          m_initialized    = false;
    TextureHandle                                 m_defaultHoleTex = TextureHandle::Invalid;
    std::random_device                            m_randomDevice;
    std::unordered_map<uint32_t, SurfaceResponse> m_surfaces;

    std::vector<BulletTracer>   m_tracers;
    std::vector<EnergyRing>     m_rings;
    std::vector<ImpactParticle> m_particles;
};

} // namespace ZHLN::CombatFX
