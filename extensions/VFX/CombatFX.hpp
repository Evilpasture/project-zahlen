// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Zahlen/COM/IDebugDraw.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/Core/COM/Ref.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Math/Transform.hpp>
#include <algorithm>
#include <cmath>
#include <expected>
#include <numbers>
#include <random>
#include <unordered_map>
#include <vector>
namespace ZHLN::CombatFX {
// ============================================================================
// Data-Driven Surface & Impact Definitions
// ============================================================================

/**
 * @brief Configurable response when a projectile or beam impacts a surface.
 */
struct SurfaceResponse {
    TextureID decalTexture   = TextureID::Invalid;
    TextureID normalTexture  = TextureID::Invalid;
    float     decalSize      = 0.28f;
    float     decalLifetime  = 15.0f;
    float     decalRoughness = 0.90f;
    bool      spawnDecal     = true;

    ZHLN::FVec4 sparkColor    = {1.0f, 0.82f, 0.55f, 1.0f};
    uint32_t    sparkCount    = 8;
    float       sparkSpeed    = 4.5f;
    float       sparkLifetime = 0.35f;
    float       sparkSize     = 0.035f;
    float       sparkGravity  = -14.0f;
    float       sparkDrag     = 1.5f;

    // Optional audio parameters (Hz, Q, Duration)
    bool  hasAudio      = true;
    float soundFreq     = 280.0f;
    float soundDuration = 0.12f;
};

// ============================================================================
// Transient Visual Effects (Tracers, Rings, Particles)
// ============================================================================

struct BulletTracer {
    ZHLN::FVec3 start         = {};
    ZHLN::FVec3 direction     = {0.0f, 0.0f, 1.0f};
    float       speed         = 350.0f;
    float       length        = 2.5f;
    float       totalDistance = 0.0f;
    float       traveled      = 0.0f;
    ZHLN::FVec4 colorStart    = {1.0f, 0.92f, 0.65f, 1.0f};
    ZHLN::FVec4 colorEnd      = {1.0f, 0.45f, 0.10f, 0.0f};
};

struct EnergyRing {
    ZHLN::FVec3 position  = {};
    ZHLN::FVec3 normal    = {0.0f, 1.0f, 0.0f};
    float       radius    = 0.0f;
    float       maxRadius = 2.5f;
    float       age       = 0.0f;
    float       duration  = 0.25f;
    ZHLN::FVec4 color     = {0.65f, 0.90f, 1.00f, 0.8f};
};

struct ImpactParticle {
    ZHLN::FVec3 position = {};
    ZHLN::FVec3 velocity = {};
    ZHLN::FVec4 color    = {1.0f, 1.0f, 1.0f, 1.0f};
    float       size     = 0.04f;
    float       age      = 0.0f;
    float       maxLife  = 0.35f;
    float       drag     = 1.5f;
    float       gravity  = -12.0f;
};

// ============================================================================
// High-Level CombatFX Subsystem Manager
// ============================================================================

class System {
  public:
    System() = default;
    ~System() {
        Release();
    }
    System(const System&)                    = delete;
    auto operator=(const System&) -> System& = delete;
    System(System&&) noexcept                = default;
    auto operator=(System&&) -> System&      = delete;
    auto Start(COM::IEngine& engine) -> std::expected<void, ErrorCode> {
        return Init(engine);
    }
    void Stop(COM::IEngine&) {
        Release();
    }
    auto BeforeRender(COM::IEngine& engine, float dt) -> std::expected<void, ErrorCode> {
        return Update(engine, dt);
    }
    [[nodiscard]] auto TracerCount() const noexcept -> size_t {
        return m_tracers.size();
    }
    [[nodiscard]] auto RingCount() const noexcept -> size_t {
        return m_rings.size();
    }
    [[nodiscard]] auto ParticleCount() const noexcept -> size_t {
        return m_particles.size();
    }
    [[nodiscard]] auto DecalCount() const noexcept -> size_t {
        return m_decals.size();
    }

    /**
     * @brief Registers default procedural textures (bullet hole, spark shapes).
     */
    [[nodiscard]] auto Init(COM::IEngine& engine) -> std::expected<void, ErrorCode> {
        if (m_engine.Get() != &engine) {
            Release();
            m_engine = COM::Ref<COM::IEngine>(&engine);
        }
        if (m_initialized && m_generation == engine.DeviceGeneration())
            return {};
        const auto pixels  = GenerateBulletHoleTexture(128);
        const auto texture = engine.Graphics()->CreateTexture(
            COM::InChain(COM::TextureCreateInfo {.pixels = std::as_bytes(std::span(pixels)), .extent = {128, 128}, .isSRGB = true})
        );
        if (!texture.HasValue())
            return std::unexpected(texture.error);
        const auto oldTexture = m_defaultHoleTex;
        m_defaultHoleTex      = texture.value;
        m_generation          = engine.DeviceGeneration();
        if (m_initialized) {
            for (auto& [id, surface]: m_surfaces)
                if (surface.decalTexture == oldTexture)
                    surface.decalTexture = m_defaultHoleTex;
            for (const auto& decal: m_decals)
                if (decal.usesDefault) {
                    if (auto* component = engine.Entities()->Get<Components::DecalComponent>(decal.entity))
                        component->albedoMap = m_defaultHoleTex;
                }
            return {};
        }
        m_initialized = true;

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
        return {};
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
    [[nodiscard]] auto SpawnImpact(COM::IEngine& engine, const FVec3& point, const FVec3& normal, uint32_t surfaceId = 0) -> std::expected<void, ErrorCode>;

    /**
     * @brief Spawns a high-speed ballistic tracer beam.
     */
    void SpawnTracer(
        const FVec3& from,
        const FVec3& to,
        float        speed      = 320.0f,
        float        length     = 2.2f,
        const FVec4& colorStart = {1.0f, 0.95f, 0.65f, 1.0f},
        const FVec4& colorEnd   = {1.0f, 0.45f, 0.10f, 0.0f}
    ) {
        if (!std::isfinite(speed) || speed <= 0 || !std::isfinite(length) || length <= 0)
            return;
        FVec3 diff = to - from;
        float dist = diff.Length();
        if (!std::isfinite(dist) || dist <= 0.01f) {
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
        const FVec3& position,
        const FVec3& normal,
        float        maxRadius = 2.5f,
        float        duration  = 0.25f,
        const FVec4& color     = {0.65f, 0.90f, 1.00f, 0.8f}
    ) {
        if (!std::isfinite(duration) || duration <= 0 || !std::isfinite(maxRadius) || maxRadius <= 0 || !std::isfinite(normal.LengthSquared()) ||
            normal.LengthSquared() < 1e-10f)
            return;
        m_rings.push_back(
            EnergyRing {
                .position = position, .normal = normal.Normalized(), .radius = 0.05f, .maxRadius = maxRadius, .age = 0.0f, .duration = duration, .color = color
            }
        );
    }

    /**
     * @brief Simulates active effects and submits GPU batches.
     */
    [[nodiscard]] auto Update(COM::IEngine& engine, float dt) -> std::expected<void, ErrorCode> {
        if (!std::isfinite(dt) || dt < 0)
            return std::unexpected(COM::Error::InvalidArgument);
        if (auto ready = Init(engine); !ready)
            return ready;
        auto& scene = *engine.Debug();
        std::erase_if(m_decals, [&](auto& decal) {
            if (!engine.Entities()->IsAlive(decal.entity) || engine.Entities()->Get<Components::PendingDestroy>(decal.entity))
                return true;
            decal.remaining -= dt;
            if (decal.remaining > 0)
                return false;
            engine.Entities()->Despawn(decal.entity);
            return true;
        });

        // 1. Advance and Render Tracers via Zero-Allocation Line Pipeline (rc.DrawLine)
        for (auto& tracer: m_tracers) {
            tracer.traveled += tracer.speed * dt;
            float head = std::min(tracer.traveled, tracer.totalDistance);
            float tail = std::max(0.0f, tracer.traveled - tracer.length);

            if (head > tail) {
                // Load once, use twice.
                const FVec3 tracerStart = tracer.start;
                const FVec3 tracerDir   = tracer.direction;
                FVec3       pTail       = tracerStart + (tracerDir * tail);
                FVec3       pHead       = tracerStart + (tracerDir * head);

                // Draws line segment with HDR emissive boost into line queue
                scene.AddLine(pTail, pHead, tracer.colorEnd, tracer.colorStart);
            }
        }
        std::erase_if(m_tracers, [](const auto& t) -> auto { return (t.traveled - t.length) >= t.totalDistance; });

        // 2. Advance and Render Expanding Shockwave Rings
        for (auto& ring: m_rings) {
            ring.age += dt;
            float t             = std::min(1.0f, ring.age / ring.duration);
            float currentRadius = ring.maxRadius * std::sin(t * (std::numbers::pi_v<float> * 0.5f));

            // Approximate ring with 16-segment line loop
            constexpr int kSegments  = 16;
            const FVec3   ringNormal = ring.normal;
            const FVec4   ringColor  = ring.color;
            const FVec3   ringOrigin = ring.position;
            FVec3         u          = (std::abs(ringNormal.y) > 0.95f) ? FVec3::UnitX() : FVec3::UnitY();
            FVec3         v          = Cross(ringNormal, u).Normalized();
            u                        = Cross(v, ringNormal).Normalized();

            float alpha = (1.0f - t) * ringColor.w;
            FVec4 col(ringColor.x, ringColor.y, ringColor.z, alpha);

            FVec3 prevP = ringOrigin + u * currentRadius;
            for (int i = 1; i <= kSegments; ++i) {
                float theta = (static_cast<float>(i) / kSegments) * 2.0f * std::numbers::pi_v<float>;
                FVec3 p     = ringOrigin + (u * std::cos(theta) + v * std::sin(theta)) * currentRadius;
                scene.AddLine(prevP, p, col, col);
                prevP = p;
            }
        }
        std::erase_if(m_rings, [](const auto& r) -> auto { return r.age >= r.duration; });

        // 3. Advance Impact Particles (Generalized drag + gravity without planar floor assumption)
        for (auto& p: m_particles) {
            p.age += dt;
            // Integrate in native vector space and write the updated state back.
            FVec3 velocity = p.velocity;
            FVec3 position = p.position;
            velocity.y += p.gravity * dt;
            velocity *= std::exp(-p.drag * dt);
            position += velocity * dt;
            p.velocity = velocity;
            p.position = position;

            // Render spark as short directional streak along velocity vector
            float speed = velocity.Length();
            if (speed > 0.05f) {
                FVec3 streakTail = position - (velocity / speed) * (p.size * 1.5f);
                float alpha      = std::max(0.0f, 1.0f - (p.age / p.maxLife)) * p.color.w;
                FVec4 col(p.color.x, p.color.y, p.color.z, alpha);

                scene.AddLine(streakTail, position, col, col);
            }
        }
        std::erase_if(m_particles, [](const auto& p) -> auto { return p.age >= p.maxLife; });
        return {};
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

    struct Decal {
        Entity entity;
        float  remaining;
        bool   usesDefault;
    };
    std::vector<Decal>     m_decals;
    COM::Ref<COM::IEngine> m_engine;
    uint64_t               m_generation = 0;
    void                   Release() {
        if (m_engine) {
            for (const auto& decal: m_decals)
                m_engine->Entities()->Despawn(decal.entity);
            if (m_generation == m_engine->DeviceGeneration() && m_defaultHoleTex != TextureID::Invalid)
                m_engine->Graphics()->UnloadTexture(m_defaultHoleTex);
        }
        Clear();
        m_decals.clear();
        m_surfaces.clear();
        m_defaultHoleTex = TextureID::Invalid;
        m_initialized    = false;
        m_generation     = 0;
        m_engine         = {};
    }
    bool                                          m_initialized    = false;
    TextureID                                     m_defaultHoleTex = TextureID::Invalid;
    std::mt19937                                  m_random {std::random_device {}()};
    std::unordered_map<uint32_t, SurfaceResponse> m_surfaces;

    std::vector<BulletTracer>   m_tracers;
    std::vector<EnergyRing>     m_rings;
    std::vector<ImpactParticle> m_particles;
};

} // namespace ZHLN::CombatFX
