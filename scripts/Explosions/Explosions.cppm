// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module;

// Public Engine & Jolt Headers ONLY
#include <Jolt/Jolt.h>
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
#include <Zahlen/Audio.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/ecs/ECS.hpp>

export module ZHLN.Explosions;

import std;

namespace ZHLN {

// ============================================================================
// Math & Noise Utilities
// ============================================================================

inline float Hash31(JPH::Vec3 p) {
    p.SetX(std::fmod(p.GetX() * 0.1031f, 1.0f));
    p.SetY(std::fmod(p.GetY() * 0.1031f, 1.0f));
    p.SetZ(std::fmod(p.GetZ() * 0.1031f, 1.0f));

    float dot_val = p.GetX() * (p.GetZ() + 31.32f) + p.GetY() * (p.GetY() + 31.32f) + p.GetZ() * (p.GetX() + 31.32f);
    p += JPH::Vec3::sReplicate(dot_val);

    return std::fmod((p.GetX() + p.GetY()) * p.GetZ(), 1.0f);
}

inline JPH::Vec3 RandomInUnitSphere(std::mt19937& gen) {
    std::uniform_real_distribution<float> dis(0.0f, 1.0f);
    float                                 u     = dis(gen);
    float                                 v     = dis(gen);
    float                                 theta = 2.0f * 3.14159265f * u;
    float                                 phi   = std::acos(2.0f * v - 1.0f);
    float                                 r     = std::cbrt(dis(gen));
    return JPH::Vec3(r * std::sin(phi) * std::cos(theta), r * std::sin(phi) * std::sin(theta), r * std::cos(phi));
}

inline JPH::Vec3 RandomOnUnitSphere(std::mt19937& gen) {
    std::uniform_real_distribution<float> dis(0.0f, 1.0f);
    float                                 u     = dis(gen);
    float                                 v     = dis(gen);
    float                                 theta = 2.0f * 3.14159265f * u;
    float                                 phi   = std::acos(2.0f * v - 1.0f);
    return JPH::Vec3(std::sin(phi) * std::cos(theta), std::sin(phi) * std::sin(theta), std::cos(phi));
}

// ============================================================================
// Procedural Texture Generators
// ============================================================================

inline std::vector<uint32_t> GenerateFireTexture(uint32_t size) {
    std::vector<uint32_t> pixels(static_cast<size_t>(size) * size);
    float                 cx      = size / 2.0f;
    float                 cy      = size / 2.0f;
    float                 maxDist = size / 2.0f;
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            float dx = static_cast<float>(x) - cx;
            float dy = static_cast<float>(y) - cy;
            float d  = std::sqrt(dx * dx + dy * dy) / maxDist;

            float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
            if (d <= 0.15f) {
                float t = d / 0.15f;
                r       = 1.0f;
                g       = 1.0f - 0.06f * t;
                b       = 1.0f - 0.21f * t;
                a       = 1.0f - 0.05f * t;
            } else if (d <= 0.35f) {
                float t = (d - 0.15f) / 0.20f;
                r       = 1.0f;
                g       = 0.94f - 0.24f * t;
                b       = 0.79f - 0.44f * t;
                a       = 0.95f - 0.25f * t;
            } else if (d <= 0.6f) {
                float t = (d - 0.35f) / 0.25f;
                r       = 1.0f;
                g       = 0.70f - 0.35f * t;
                b       = 0.35f - 0.27f * t;
                a       = 0.70f - 0.40f * t;
            } else if (d <= 1.0f) {
                float t = (d - 0.6f) / 0.4f;
                r       = 1.0f - t;
                g       = 0.35f * (1.0f - t);
                b       = 0.08f * (1.0f - t);
                a       = 0.30f - 0.4f * t;
            }

            auto ru              = static_cast<uint8_t>(std::clamp(r, 0.0f, 1.0f) * 255.0f);
            auto gu              = static_cast<uint8_t>(std::clamp(g, 0.0f, 1.0f) * 255.0f);
            auto bu              = static_cast<uint8_t>(std::clamp(b, 0.0f, 1.0f) * 255.0f);
            auto au              = static_cast<uint8_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f);
            pixels[y * size + x] = (au << 24) | (bu << 16) | (gu << 8) | ru;
        }
    }
    return pixels;
}

inline std::vector<uint32_t> GenerateSmokeTexture(uint32_t size) {
    std::vector<uint32_t> pixels(static_cast<size_t>(size) * size);
    float                 cx      = size / 2.0f;
    float                 cy      = size / 2.0f;
    float                 maxDist = size / 2.0f;

    std::mt19937                          gen(1337);
    std::uniform_real_distribution<float> dis(-0.35f, 0.35f);
    std::uniform_real_distribution<float> radDis(0.08f, 0.16f);
    std::uniform_real_distribution<float> alphaDis(0.15f, 0.30f);

    struct Puff {
        float x, y, r, a;
    };
    std::vector<Puff> puffs;
    puffs.reserve(30);
    for (int i = 0; i < 30; ++i) {
        puffs.push_back({cx + dis(gen) * size, cy + dis(gen) * size, radDis(gen) * size, alphaDis(gen)});
    }

    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            float dx = static_cast<float>(x) - cx;
            float dy = static_cast<float>(y) - cy;
            float d  = std::sqrt(dx * dx + dy * dy) / maxDist;

            float baseAlpha = 0.0f;
            if (d <= 0.4f) {
                baseAlpha = 1.0f - (d / 0.4f) * 0.3f;
            } else if (d <= 0.75f) {
                float t   = (d - 0.4f) / 0.35f;
                baseAlpha = 0.7f - 0.55f * t;
            } else if (d <= 1.0f) {
                float t   = (d - 0.75f) / 0.25f;
                baseAlpha = 0.15f * (1.0f - t);
            }

            float puffAlphaSum = 0.0f;
            for (const auto& puff: puffs) {
                float pdx = static_cast<float>(x) - puff.x;
                float pdy = static_cast<float>(y) - puff.y;
                float pd  = std::sqrt(pdx * pdx + pdy * pdy) / puff.r;
                if (pd <= 1.0f) {
                    puffAlphaSum += puff.a * (1.0f - pd);
                }
            }

            float finalAlpha     = std::clamp(baseAlpha + puffAlphaSum, 0.0f, 1.0f);
            auto  au             = static_cast<uint8_t>(finalAlpha * 255.0f);
            pixels[y * size + x] = (au << 24) | (255 << 16) | (255 << 8) | 255;
        }
    }
    return pixels;
}

inline std::vector<uint32_t> GenerateSparkTexture(uint32_t size) {
    std::vector<uint32_t> pixels(static_cast<size_t>(size) * size);
    float                 cx      = size / 2.0f;
    float                 cy      = size / 2.0f;
    float                 maxDist = size / 2.0f;
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            float dx = static_cast<float>(x) - cx;
            float dy = static_cast<float>(y) - cy;
            float d  = std::sqrt(dx * dx + dy * dy) / maxDist;

            float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
            if (d <= 0.25f) {
                float t = d / 0.25f;
                r       = 1.0f;
                g       = 1.0f - 0.14f * t;
                b       = 1.0f - 0.41f * t;
                a       = 1.0f - 0.10f * t;
            } else if (d <= 0.5f) {
                float t = (d - 0.25f) / 0.25f;
                r       = 1.0f - 0.2f * t;
                g       = 0.86f - 0.27f * t;
                b       = 0.59f - 0.39f * t;
                a       = 0.90f - 0.55f * t;
            } else if (d <= 1.0f) {
                float t = (d - 0.5f) / 0.5f;
                r       = 0.8f * (1.0f - t);
                g       = 0.59f * (1.0f - t);
                b       = 0.20f * (1.0f - t);
                a       = 0.35f * (1.0f - t);
            }

            auto ru              = static_cast<uint8_t>(std::clamp(r, 0.0f, 1.0f) * 255.0f);
            auto gu              = static_cast<uint8_t>(std::clamp(g, 0.0f, 1.0f) * 255.0f);
            auto bu              = static_cast<uint8_t>(std::clamp(b, 0.0f, 1.0f) * 255.0f);
            auto au              = static_cast<uint8_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f);
            pixels[y * size + x] = (au << 24) | (bu << 16) | (gu << 8) | ru;
        }
    }
    return pixels;
}

inline std::vector<uint32_t> GenerateShockwaveTexture(uint32_t size) {
    std::vector<uint32_t> pixels(static_cast<size_t>(size) * size);
    float                 cx    = size / 2.0f;
    float                 cy    = size / 2.0f;
    float                 outer = size / 2.0f;
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            float dx = static_cast<float>(x) - cx;
            float dy = static_cast<float>(y) - cy;
            float d  = std::sqrt(dx * dx + dy * dy) / outer;

            float peak = 0.78f;
            float w    = 0.12f;
            float a    = std::max(0.0f, 1.0f - std::abs(d - peak) / w);
            a          = std::pow(a, 2.0f);

            float inner = std::max(0.0f, 1.0f - d / peak);
            a += std::pow(inner, 3.0f) * 0.25f;
            a = std::min(1.0f, a);

            uint8_t ru           = 255;
            uint8_t gu           = 220;
            uint8_t bu           = 160;
            auto    au           = static_cast<uint8_t>(a * 255.0f);
            pixels[y * size + x] = (au << 24) | (bu << 16) | (gu << 8) | ru;
        }
    }
    return pixels;
}

// ============================================================================
// Particle & Component Structs
// ============================================================================

struct ExplosionParticle {
    JPH::Vec3 position;
    JPH::Vec3 velocity;
    float     life      = 0.0f;
    float     maxLife   = 1.0f;
    float     startSize = 1.0f;
    float     endSize   = 1.0f;
    float     drag      = 1.0f;
    float     gravity   = 0.0f;
    JPH::Vec3 colorStart;
    JPH::Vec3 colorMid;
    JPH::Vec3 colorEnd;
};

export struct ExplosionComponent {
    JPH::Vec3 origin;
    float     scale    = 1.0f;
    float     age      = 0.0f;
    float     duration = 5.0f;

    std::vector<ExplosionParticle> fireball;
    std::vector<ExplosionParticle> smoke;
    std::vector<ExplosionParticle> sparks;

    // GPU Storage buffers (1 batch per group)
    BufferHandle fireballGpuBuf = BufferHandle::Invalid;
    BufferHandle smokeGpuBuf    = BufferHandle::Invalid;
    BufferHandle sparksGpuBuf   = BufferHandle::Invalid;

    static void OnDestroy(ExplosionComponent* c) noexcept {
        if (auto* engine = GetEngineContext()) {
            auto& rc = engine->GetRenderContext();
            if (c->fireballGpuBuf != BufferHandle::Invalid) {
                rc.DestroyBuffer(c->fireballGpuBuf);
            }
            if (c->smokeGpuBuf != BufferHandle::Invalid) {
                rc.DestroyBuffer(c->smokeGpuBuf);
            }
            if (c->sparksGpuBuf != BufferHandle::Invalid) {
                rc.DestroyBuffer(c->sparksGpuBuf);
            }
        }
        c->fireballGpuBuf = c->smokeGpuBuf = c->sparksGpuBuf = BufferHandle::Invalid;
    }
};

// ============================================================================
// Explosion System
// ============================================================================

export class ExplosionSystem {
  public:
    static void Init(RenderContext& rc) {
        if (s_Init) {
            return;
        }
        s_Init = true;

        s_FireTex      = rc.CreateTexture(GenerateFireTexture(256).data(), 256, 256, true).value_or(1);
        s_SmokeTex     = rc.CreateTexture(GenerateSmokeTexture(256).data(), 256, 256, true).value_or(1);
        s_SparkTex     = rc.CreateTexture(GenerateSparkTexture(128).data(), 128, 128, true).value_or(1);
        s_ShockwaveTex = rc.CreateTexture(GenerateShockwaveTexture(512).data(), 512, 512, true).value_or(1);

        s_QuadMesh                 = CreativeWorksFactory::CreatePlane(rc, 0.5f);
        s_ShockwaveMat             = CreativeWorksFactory::CreateBasicMaterial(rc, true, true, true).value_or(Material {});
        s_ShockwaveMat.albedoIndex = s_ShockwaveTex;
        s_ShockwaveMat.alphaMode   = 2;
    }

    // Spawns a single ECS Entity containing Explosion + Light + Transform
    static Entity Spawn(Engine& engine, const JPH::Vec3& origin, float scale = 1.0f) {
        auto& reg = engine.GetRegistry();
        Init(engine.GetRenderContext());

        Entity e = reg.Create();
        reg.Add(e, Components::TransformComponent {.position = origin + JPH::Vec3(0.0f, 1.0f * scale, 0.0f)});
        reg.Add(
            e, Components::LightComponent {
                   .type = LightType::Point, .color = JPH::Vec3(1.0f, 0.66f, 0.33f), .intensity = 0.0f, .radius = 0.8f, .range = 40.0f * scale
               }
        );

        auto& expComp  = reg.Add(e, ExplosionComponent {});
        expComp.origin = origin;
        expComp.scale  = scale;

        std::mt19937 gen(std::random_device {}());
        InitParticles(expComp, gen);

        engine.GetAudioContext().PlayProceduralBeep(90.0f, 0.40f, 0.85f);
        engine.GetAudioContext().PlayProceduralBeep(45.0f, 0.55f, 0.95f);

        return e;
    }

    // ECS System Update Loop
    static void Update(Engine& engine, float dt) {
        if (!s_Init) {
            return;
        }

        auto& reg = engine.GetRegistry();
        auto& rc  = engine.GetRenderContext();

        auto entities   = reg.GetEntitiesWith<ExplosionComponent>();
        auto explosions = reg.GetRawArray<ExplosionComponent>();

        std::vector<Entity> deadEntities;

        for (size_t i = 0; i < entities.size(); ++i) {
            Entity              e   = entities[i];
            ExplosionComponent& exp = explosions[i];

            exp.age += dt;

            // 1. Update Light Flash Intensity directly on the Entity's LightComponent
            if (auto* light = reg.Get<Components::LightComponent>(e)) {
                float flashT     = std::max(0.0f, 1.0f - exp.age / 0.35f);
                light->intensity = flashT * flashT * 1500.0f * exp.scale * exp.scale;
                light->color     = JPH::Vec3(1.0f, 0.7f * flashT + 0.2f, 0.3f * flashT + 0.05f);
            }

            // 2. CPU Physics Update (Preserves exact spark bouncing, gravity, and drag)
            UpdateGroup(exp.fireball, dt, true, exp.origin);
            UpdateGroup(exp.smoke, dt, false, exp.origin);
            UpdateSparks(exp.sparks, dt, exp.origin);

            // 3. Batch GPU Render Submissions (4 Draw Calls Total instead of 300!)
            RenderBatchGPU(rc, exp);

            if (exp.age > exp.duration) {
                deadEntities.push_back(e);
            }
        }

        // Clean up expired explosion entities
        for (Entity e: deadEntities) {
            reg.Destroy(e);
        }
    }

  private:
    static void InitParticles(ExplosionComponent& exp, std::mt19937& gen) {
        exp.fireball.resize(120);
        for (auto& p: exp.fireball) {
            JPH::Vec3 dir = RandomInUnitSphere(gen);
            dir.SetY(std::abs(dir.GetY()) * 0.6f + dir.GetY() * 0.4f);
            float speed  = (3.0f + Hash31(dir * 1.5f) * 6.0f) * exp.scale;
            p.position   = dir * (0.15f * exp.scale);
            p.velocity   = dir * speed;
            p.maxLife    = 0.7f + Hash31(dir * 2.5f) * 0.7f;
            p.startSize  = (0.5f + Hash31(dir * 3.5f) * 0.8f) * exp.scale;
            p.endSize    = (1.5f + Hash31(dir * 4.5f) * 1.5f) * exp.scale;
            p.drag       = 2.5f;
            p.gravity    = -1.0f;
            p.colorStart = JPH::Vec3(4.0f, 3.0f, 1.4f);
            p.colorMid   = JPH::Vec3(2.2f, 0.9f, 0.25f);
            p.colorEnd   = JPH::Vec3(0.4f, 0.06f, 0.02f);
        }

        exp.smoke.resize(80);
        for (auto& p: exp.smoke) {
            JPH::Vec3 dir = RandomInUnitSphere(gen);
            dir.SetY(std::abs(dir.GetY()) * 0.8f + dir.GetY() * 0.2f);
            float speed = (1.5f + Hash31(dir * 1.5f) * 3.5f) * exp.scale;
            p.position  = dir * (0.3f * exp.scale);
            p.velocity  = dir * speed;
            p.velocity.SetY(p.velocity.GetY() + 0.8f * exp.scale);
            p.maxLife    = 2.5f + Hash31(dir * 2.5f) * 2.0f;
            p.startSize  = (1.0f + Hash31(dir * 4.5f) * 1.0f) * exp.scale;
            p.endSize    = (4.0f + Hash31(dir * 5.5f) * 3.0f) * exp.scale;
            p.drag       = 1.2f;
            p.gravity    = 0.6f;
            float gray   = 0.05f + Hash31(dir * 6.5f) * 0.15f;
            p.colorStart = JPH::Vec3(1.4f, 0.6f, 0.15f);
            p.colorMid   = JPH::Vec3(0.35f, 0.25f, 0.2f);
            p.colorEnd   = JPH::Vec3(gray, gray, gray * 0.95f);
        }

        exp.sparks.resize(100);
        for (auto& p: exp.sparks) {
            JPH::Vec3 dir = RandomOnUnitSphere(gen);
            dir.SetY(std::abs(dir.GetY()) * 0.8f + dir.GetY() * 0.2f);
            float speed  = (8.0f + Hash31(dir * 1.5f) * 12.0f) * exp.scale;
            p.position   = dir * (0.1f * exp.scale);
            p.velocity   = dir * speed;
            p.maxLife    = 0.9f + Hash31(dir * 2.5f) * 1.4f;
            p.startSize  = (0.08f + Hash31(dir * 3.5f) * 0.1f) * exp.scale;
            p.endSize    = 0.02f * exp.scale;
            p.drag       = 0.6f;
            p.gravity    = -9.8f * 0.5f;
            p.colorStart = JPH::Vec3(3.5f, 2.5f, 1.0f);
            p.colorMid   = JPH::Vec3(2.5f, 1.0f, 0.2f);
            p.colorEnd   = JPH::Vec3(0.5f, 0.05f, 0.02f);
        }
    }

    static void UpdateGroup(std::vector<ExplosionParticle>& group, float dt, bool isFire, const JPH::Vec3& /*origin*/) {
        for (auto& p: group) {
            p.life += dt;
            if (p.life < 0) {
                continue;
            }
            float dragFactor = std::exp(-p.drag * dt);
            p.velocity *= dragFactor;
            p.velocity.SetY(p.velocity.GetY() + p.gravity * dt * (isFire ? 0.4f : 1.0f));
            p.position += p.velocity * dt;
        }
    }

    static void UpdateSparks(std::vector<ExplosionParticle>& sparks, float dt, const JPH::Vec3& origin) {
        for (auto& p: sparks) {
            p.life += dt;
            float dragFactor = std::exp(-p.drag * dt);
            p.velocity *= dragFactor;
            p.velocity.SetY(p.velocity.GetY() + p.gravity * dt);
            p.position += p.velocity * dt;

            float localGroundY = -origin.GetY() + 0.02f;
            if (p.position.GetY() < localGroundY && p.velocity.GetY() < 0.0f) {
                p.position.SetY(localGroundY);
                p.velocity.SetY(p.velocity.GetY() * -0.35f);
                p.velocity.SetX(p.velocity.GetX() * 0.6f);
                p.velocity.SetZ(p.velocity.GetZ() * 0.6f);
            }
        }
    }

    static void RenderBatchGPU(RenderContext& rc, ExplosionComponent& exp) {
        // --- 1. BATCH FIREBALL (1 GPU Draw Call) ---
        float fireOpacity = std::max(0.0f, 1.0f - exp.age / 1.4f);
        if (fireOpacity > 0.001f) {
            // Allocate GPU storage buffer ONCE on first frame
            if (exp.fireballGpuBuf == BufferHandle::Invalid) {
                exp.fireballGpuBuf = rc.CreateStorageBuffer(exp.fireball.size() * sizeof(Particle));
            }

            std::vector<Particle> gpuParticles(exp.fireball.size());
            for (size_t i = 0; i < exp.fireball.size(); ++i) {
                const auto& p = exp.fireball[i];
                float       t = std::min(1.0f, p.life / p.maxLife);

                JPH::Vec3 color = (t < 0.5f) ? p.colorStart + (p.colorMid - p.colorStart) * (t / 0.5f) :
                                               p.colorMid + (p.colorEnd - p.colorMid) * ((t - 0.5f) / 0.5f);

                float fade = 1.0f - std::pow(t, 3.0f);
                float size = std::lerp(p.startSize, p.endSize, t);

                JPH::Vec3 finalColor = color * (fade * fireOpacity * 15.0f);

                gpuParticles[i].position = JPH::Vec4(exp.origin + p.position, 1.0f);
                gpuParticles[i].color    = JPH::Vec4(finalColor, fade * fireOpacity);
                gpuParticles[i].params   = JPH::Vec4(p.life, p.maxLife, size, 0.0f);
            }

            // Stream new frame data into existing GPU buffer in-place
            rc.UpdateBuffer(exp.fireballGpuBuf, gpuParticles.data(), gpuParticles.size() * sizeof(Particle));

            rc.SubmitParticleEmitter(
                exp.fireballGpuBuf, static_cast<uint32_t>(exp.fireball.size()),
                {
                    .textureIndex = s_FireTex,
                    .alignment    = ParticleAlignment::CameraBillboard,
                    .blendMode    = 1 // Additive blend
                }
            );
        }

        // --- 2. BATCH SMOKE (1 GPU Draw Call) ---
        float smokeOpacity = std::max(0.0f, 1.0f - std::max(0.0f, exp.age - 1.5f) / 3.0f);
        if (smokeOpacity > 0.001f) {
            if (exp.smokeGpuBuf == BufferHandle::Invalid) {
                exp.smokeGpuBuf = rc.CreateStorageBuffer(exp.smoke.size() * sizeof(Particle));
            }

            std::vector<Particle> gpuParticles(exp.smoke.size());
            for (size_t i = 0; i < exp.smoke.size(); ++i) {
                const auto& p = exp.smoke[i];
                float       t = std::min(1.0f, p.life / p.maxLife);

                JPH::Vec3 color = (t < 0.5f) ? p.colorStart + (p.colorMid - p.colorStart) * (t / 0.5f) :
                                               p.colorMid + (p.colorEnd - p.colorMid) * ((t - 0.5f) / 0.5f);

                float fade = 1.0f - std::pow(t, 3.0f);
                float size = std::lerp(p.startSize, p.endSize, t);

                gpuParticles[i].position = JPH::Vec4(exp.origin + p.position, 1.0f);
                gpuParticles[i].color    = JPH::Vec4(color, fade * smokeOpacity);
                gpuParticles[i].params   = JPH::Vec4(p.life, p.maxLife, size, 0.0f);
            }

            rc.UpdateBuffer(exp.smokeGpuBuf, gpuParticles.data(), gpuParticles.size() * sizeof(Particle));

            rc.SubmitParticleEmitter(
                exp.smokeGpuBuf, static_cast<uint32_t>(exp.smoke.size()),
                {
                    .textureIndex = s_SmokeTex,
                    .alignment    = ParticleAlignment::CameraBillboard,
                    .blendMode    = 0 // Alpha blend
                }
            );
        }

        // --- 3. BATCH SPARKS (1 GPU Draw Call) ---
        float sparkOpacity = std::max(0.0f, 1.0f - exp.age / 1.8f);
        if (sparkOpacity > 0.001f) {
            if (exp.sparksGpuBuf == BufferHandle::Invalid) {
                exp.sparksGpuBuf = rc.CreateStorageBuffer(exp.sparks.size() * sizeof(Particle));
            }

            std::vector<Particle> gpuParticles(exp.sparks.size());
            for (size_t i = 0; i < exp.sparks.size(); ++i) {
                const auto& p = exp.sparks[i];
                float       t = std::min(1.0f, p.life / p.maxLife);

                JPH::Vec3 color = (t < 0.4f) ? p.colorStart + (p.colorMid - p.colorStart) * (t / 0.4f) :
                                               p.colorMid + (p.colorEnd - p.colorMid) * ((t - 0.4f) / 0.6f);

                float flick = 0.7f + Hash31(p.position) * 0.6f;
                float fade  = 1.0f - std::pow(t, 2.0f);
                float size  = std::lerp(p.startSize, p.endSize, t);

                JPH::Vec3 finalColor = color * (fade * flick * sparkOpacity * 25.0f);

                gpuParticles[i].position = JPH::Vec4(exp.origin + p.position, 1.0f);
                gpuParticles[i].velocity = JPH::Vec4(p.velocity, 0.0f);
                gpuParticles[i].color    = JPH::Vec4(finalColor, fade * sparkOpacity);
                gpuParticles[i].params   = JPH::Vec4(p.life, p.maxLife, size, 0.0f);
            }

            rc.UpdateBuffer(exp.sparksGpuBuf, gpuParticles.data(), gpuParticles.size() * sizeof(Particle));

            rc.SubmitParticleEmitter(
                exp.sparksGpuBuf, static_cast<uint32_t>(exp.sparks.size()),
                {.textureIndex = s_SparkTex, .alignment = ParticleAlignment::VelocityStretched, .blendMode = 1}
            );
        }

        // --- 4. SHOCKWAVE (1 GPU Draw Call) ---
        float shockOpacity = std::max(0.0f, 1.0f - exp.age / 0.7f);
        if (shockOpacity > 0.001f) {
            float swT    = std::min(1.0f, exp.age / 0.6f);
            float swSize = swT * 28.0f * exp.scale;

            JPH::Mat44 transform =
                Math::CreateTransform(JPH::Vec3(exp.origin.GetX(), 0.05f, exp.origin.GetZ()), JPH::Quat::sIdentity(), JPH::Vec3(swSize, 1.0f, swSize));

            DrawParams params;
            params.transform        = transform;
            params.prevTransform    = transform;
            params.cullRadius       = swSize;
            params.flags            = DrawFlags::VisibleInMain | DrawFlags::ExcludeFromTLAS;
            params.colorOverride    = {1.0f, 0.8f, 0.5f, shockOpacity};
            params.emissiveOverride = {15.0f, 10.0f, 4.0f, shockOpacity};

            Renderer::Draw(rc, s_ShockwaveMat, s_QuadMesh, params);
        }
    }

  private:
    inline static bool     s_Init         = false;
    inline static uint32_t s_FireTex      = 1;
    inline static uint32_t s_SmokeTex     = 1;
    inline static uint32_t s_SparkTex     = 1;
    inline static uint32_t s_ShockwaveTex = 1;

    inline static Mesh     s_QuadMesh {};
    inline static Material s_ShockwaveMat {};
};

} // namespace ZHLN
