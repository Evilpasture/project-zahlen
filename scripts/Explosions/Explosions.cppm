module;
#include <Jolt/Jolt.h>
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Zahlen/Audio.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/ecs/ECS.hpp>

export module ZHLN.Explosions;
import std;

namespace ZHLN {

// Fast procedural 3D Hash
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

// Procedural texture generators
inline std::vector<uint32_t> GenerateFireTexture(uint32_t size) {
    std::vector<uint32_t> pixels(size * size);
    float                 cx      = size / 2.0f;
    float                 cy      = size / 2.0f;
    float                 maxDist = size / 2.0f;
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            float dx = x - cx;
            float dy = y - cy;
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

            uint8_t ru           = static_cast<uint8_t>(std::clamp(r, 0.0f, 1.0f) * 255.0f);
            uint8_t gu           = static_cast<uint8_t>(std::clamp(g, 0.0f, 1.0f) * 255.0f);
            uint8_t bu           = static_cast<uint8_t>(std::clamp(b, 0.0f, 1.0f) * 255.0f);
            uint8_t au           = static_cast<uint8_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f);
            pixels[y * size + x] = (au << 24) | (bu << 16) | (gu << 8) | ru;
        }
    }
    return pixels;
}

inline std::vector<uint32_t> GenerateSmokeTexture(uint32_t size) {
    std::vector<uint32_t> pixels(size * size);
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
    for (int i = 0; i < 30; ++i) {
        puffs.push_back({cx + dis(gen) * size, cy + dis(gen) * size, radDis(gen) * size, alphaDis(gen)});
    }

    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            float dx = x - cx;
            float dy = y - cy;
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
                float pdx = x - puff.x;
                float pdy = y - puff.y;
                float pd  = std::sqrt(pdx * pdx + pdy * pdy) / puff.r;
                if (pd <= 1.0f) {
                    puffAlphaSum += puff.a * (1.0f - pd);
                }
            }

            float   finalAlpha   = std::clamp(baseAlpha + puffAlphaSum, 0.0f, 1.0f);
            uint8_t au           = static_cast<uint8_t>(finalAlpha * 255.0f);
            pixels[y * size + x] = (au << 24) | (255 << 16) | (255 << 8) | 255;
        }
    }
    return pixels;
}

inline std::vector<uint32_t> GenerateSparkTexture(uint32_t size) {
    std::vector<uint32_t> pixels(size * size);
    float                 cx      = size / 2.0f;
    float                 cy      = size / 2.0f;
    float                 maxDist = size / 2.0f;
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            float dx = x - cx;
            float dy = y - cy;
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

            uint8_t ru           = static_cast<uint8_t>(std::clamp(r, 0.0f, 1.0f) * 255.0f);
            uint8_t gu           = static_cast<uint8_t>(std::clamp(g, 0.0f, 1.0f) * 255.0f);
            uint8_t bu           = static_cast<uint8_t>(std::clamp(b, 0.0f, 1.0f) * 255.0f);
            uint8_t au           = static_cast<uint8_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f);
            pixels[y * size + x] = (au << 24) | (bu << 16) | (gu << 8) | ru;
        }
    }
    return pixels;
}

inline std::vector<uint32_t> GenerateShockwaveTexture(uint32_t size) {
    std::vector<uint32_t> pixels(size * size);
    float                 cx    = size / 2.0f;
    float                 cy    = size / 2.0f;
    float                 outer = size / 2.0f;
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            float dx = x - cx;
            float dy = y - cy;
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
            uint8_t au           = static_cast<uint8_t>(a * 255.0f);
            pixels[y * size + x] = (au << 24) | (bu << 16) | (gu << 8) | ru;
        }
    }
    return pixels;
}

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

struct ExplosionInstance {
    JPH::Vec3 origin;
    float     scale;
    float     age      = 0.0f;
    float     duration = 5.0f;
    bool      finished = false;

    std::vector<ExplosionParticle> fireball;
    std::vector<ExplosionParticle> smoke;
    std::vector<ExplosionParticle> sparks;

    Entity lightFlashEntity = NullEntity;

    ExplosionInstance(Engine* engine, const JPH::Vec3& org, float sc): origin(org), scale(sc) {
        std::mt19937 gen(std::random_device {}());

        // -------- FIREBALL --------
        uint32_t fireCount = 120;
        fireball.resize(fireCount);
        for (auto& p: fireball) {
            JPH::Vec3 dir = RandomInUnitSphere(gen);
            dir.SetY(std::abs(dir.GetY()) * 0.6f + dir.GetY() * 0.4f);
            float speed  = (3.0f + Hash31(dir * 1.5f) * 6.0f) * scale;
            p.position   = dir * (0.15f * scale);
            p.velocity   = dir * speed;
            p.maxLife    = 0.7f + Hash31(dir * 2.5f) * 0.7f;
            p.life       = 0.0f;
            p.startSize  = (0.5f + Hash31(dir * 3.5f) * 0.8f) * scale;
            p.endSize    = (1.5f + Hash31(dir * 4.5f) * 1.5f) * scale;
            p.drag       = 2.5f;
            p.gravity    = -1.0f;
            p.colorStart = JPH::Vec3(4.0f, 3.0f, 1.4f);
            p.colorMid   = JPH::Vec3(2.2f, 0.9f, 0.25f);
            p.colorEnd   = JPH::Vec3(0.4f, 0.06f, 0.02f);
        }

        // -------- SMOKE --------
        uint32_t smokeCount = 80;
        smoke.resize(smokeCount);
        for (auto& p: smoke) {
            JPH::Vec3 dir = RandomInUnitSphere(gen);
            dir.SetY(std::abs(dir.GetY()) * 0.8f + dir.GetY() * 0.2f);
            float speed = (1.5f + Hash31(dir * 1.5f) * 3.5f) * scale;
            p.position  = dir * (0.3f * scale);
            p.velocity  = dir * speed;
            p.velocity.SetY(p.velocity.GetY() + 0.8f * scale);
            p.maxLife    = 2.5f + Hash31(dir * 2.5f) * 2.0f;
            p.life       = -Hash31(dir * 3.5f) * 0.15f;
            p.startSize  = (1.0f + Hash31(dir * 4.5f) * 1.0f) * scale;
            p.endSize    = (4.0f + Hash31(dir * 5.5f) * 3.0f) * scale;
            p.drag       = 1.2f;
            p.gravity    = 0.6f;
            float gray   = 0.05f + Hash31(dir * 6.5f) * 0.15f;
            p.colorStart = JPH::Vec3(1.4f, 0.6f, 0.15f);
            p.colorMid   = JPH::Vec3(0.35f, 0.25f, 0.2f);
            p.colorEnd   = JPH::Vec3(gray, gray, gray * 0.95f);
        }

        // -------- SPARKS --------
        uint32_t sparkCount = 100;
        sparks.resize(sparkCount);
        for (auto& p: sparks) {
            JPH::Vec3 dir = RandomOnUnitSphere(gen);
            dir.SetY(std::abs(dir.GetY()) * 0.8f + dir.GetY() * 0.2f);
            float speed  = (8.0f + Hash31(dir * 1.5f) * 12.0f) * scale;
            p.position   = dir * (0.1f * scale);
            p.velocity   = dir * speed;
            p.maxLife    = 0.9f + Hash31(dir * 2.5f) * 1.4f;
            p.life       = 0.0f;
            p.startSize  = (0.08f + Hash31(dir * 3.5f) * 0.1f) * scale;
            p.endSize    = 0.02f * scale;
            p.drag       = 0.6f;
            p.gravity    = -9.8f * 0.5f;
            p.colorStart = JPH::Vec3(3.5f, 2.5f, 1.0f);
            p.colorMid   = JPH::Vec3(2.5f, 1.0f, 0.2f);
            p.colorEnd   = JPH::Vec3(0.5f, 0.05f, 0.02f);
        }

        // -------- LIGHT FLASH (ECS Entity) --------
        auto& reg        = engine->GetRegistry();
        lightFlashEntity = reg.Create();
        reg.Add(lightFlashEntity, ZHLN::Components::TransformComponent {.position = origin + JPH::Vec3(0.0f, 1.0f * scale, 0.0f)});
        reg.Add(
            lightFlashEntity, ZHLN::Components::LightComponent {
                                  .type = LightType::Point, .color = JPH::Vec3(1.0f, 0.66f, 0.33f), .intensity = 0.0f, .radius = 0.8f, .range = 40.0f * scale
                              }
        );

        // Trigger dynamic procedural audio sound
        engine->GetAudioContext().PlayProceduralBeep(90.0f, 0.40f, 0.85f);
        engine->GetAudioContext().PlayProceduralBeep(45.0f, 0.55f, 0.95f);
    }

    void Update(Engine* engine, float dt) {
        age += dt;

        // 1. Update Flash Light Intensity
        float flashT = std::max(0.0f, 1.0f - age / 0.35f);
        auto& reg    = engine->GetRegistry();
        if (auto* light = reg.Get<ZHLN::Components::LightComponent>(lightFlashEntity)) {
            light->intensity = flashT * flashT * 1500.0f * scale * scale;
            light->color     = JPH::Vec3(1.0f, 0.7f * flashT + 0.2f, 0.3f * flashT + 0.05f);
        }

        // 2. Update Particles on CPU
        UpdateGroup(fireball, dt, true);
        UpdateGroup(smoke, dt, false);
        UpdateSparks(dt);

        if (age > duration) {
            finished = true;
        }
    }

    void Destroy(Engine* engine) {
        if (lightFlashEntity != NullEntity) {
            engine->GetRegistry().Destroy(lightFlashEntity);
            lightFlashEntity = NullEntity;
        }
    }

    void Render(
        RenderContext&  rc,
        const Camera&   cam,
        const Mesh&     quadMesh,
        const Material& fireMat,
        const Material& smokeMat,
        const Material& sparkMat,
        const Material& shockwaveMat
    ) const noexcept {
        // --- VIEW-PLANE ALIGNED CAMERA BILLBOARDING ---
        JPH::Mat44 invView = cam.GetViewMatrix().Inversed();
        JPH::Vec3  right   = invView.GetColumn3(0).Normalized();
        JPH::Vec3  up      = invView.GetColumn3(1).Normalized();
        JPH::Vec3  back    = invView.GetColumn3(2).Normalized();

        JPH::Mat44 billboardMat(JPH::Vec4(right, 0.0f), JPH::Vec4(back, 0.0f), JPH::Vec4(-up, 0.0f), JPH::Vec4(0.0f, 0.0f, 0.0f, 1.0f));
        JPH::Quat  finalBillboardRot = billboardMat.GetQuaternion().Normalized();

        // 1. DRAW SMOKE FIRST (Background volume)
        float smokeOpacity = std::max(0.0f, 1.0f - std::max(0.0f, age - 1.5f) / 3.0f);
        if (smokeOpacity > 0.001f) {
            float emissiveBoost = std::max(0.0f, 1.0f - age / 1.0f) * 20.0f;
            for (const auto& p: smoke) {
                if (p.life < 0.0f)
                    continue;
                float t = std::min(1.0f, p.life / p.maxLife);

                JPH::Vec3 color;
                if (t < 0.5f) {
                    color = p.colorStart + (p.colorMid - p.colorStart) * (t / 0.5f);
                } else {
                    color = p.colorMid + (p.colorEnd - p.colorMid) * ((t - 0.5f) / 0.5f);
                }
                float fade       = 1.0f - std::pow(t, 3.0f);
                float finalAlpha = fade * smokeOpacity;

                float      size      = std::lerp(p.startSize, p.endSize, t);
                JPH::Mat44 transform = Math::CreateTransform(origin + p.position, finalBillboardRot, JPH::Vec3::sReplicate(size));

                DrawParams params;
                params.transform        = transform;
                params.prevTransform    = transform;
                params.cullRadius       = size;
                params.flags            = DrawFlags::VisibleInMain | DrawFlags::ExcludeFromTLAS;
                params.colorOverride    = {color.GetX(), color.GetY(), color.GetZ(), finalAlpha};
                params.emissiveOverride = {color.GetX() * emissiveBoost, color.GetY() * emissiveBoost, color.GetZ() * emissiveBoost, 1.0f};

                Renderer::Draw(rc, smokeMat, quadMesh, params);
            }
        }

        // 2. DRAW FIREBALL SECOND (Additive glowing core in center)
        float fireOpacity = std::max(0.0f, 1.0f - age / 1.4f);
        if (fireOpacity > 0.001f) {
            for (const auto& p: fireball) {
                if (p.life < 0.0f)
                    continue;
                float t = std::min(1.0f, p.life / p.maxLife);

                JPH::Vec3 color;
                if (t < 0.5f) {
                    color = p.colorStart + (p.colorMid - p.colorStart) * (t / 0.5f);
                } else {
                    color = p.colorMid + (p.colorEnd - p.colorMid) * ((t - 0.5f) / 0.5f);
                }
                float     fade       = 1.0f - std::pow(t, 3.0f);
                JPH::Vec3 finalColor = color * (fade * fireOpacity);

                float      size      = std::lerp(p.startSize, p.endSize, t);
                JPH::Mat44 transform = Math::CreateTransform(origin + p.position, finalBillboardRot, JPH::Vec3::sReplicate(size));

                DrawParams params;
                params.transform        = transform;
                params.prevTransform    = transform;
                params.cullRadius       = size;
                params.flags            = DrawFlags::VisibleInMain | DrawFlags::ExcludeFromTLAS;
                params.colorOverride    = {finalColor.GetX(), finalColor.GetY(), finalColor.GetZ(), fade * fireOpacity};
                params.emissiveOverride = {finalColor.GetX() * 15.0f, finalColor.GetY() * 15.0f, finalColor.GetZ() * 15.0f, 1.0f};

                Renderer::Draw(rc, fireMat, quadMesh, params);
            }
        }

        // 3. DRAW SHOCKWAVE (Ground ring)
        float shockOpacity = std::max(0.0f, 1.0f - age / 0.7f);
        if (shockOpacity > 0.001f) {
            float swT    = std::min(1.0f, age / 0.6f);
            float swSize = swT * 28.0f * scale;

            JPH::Mat44 transform =
                Math::CreateTransform(JPH::Vec3(origin.GetX(), 0.05f, origin.GetZ()), JPH::Quat::sIdentity(), JPH::Vec3(swSize, 1.0f, swSize));

            DrawParams params;
            params.transform        = transform;
            params.prevTransform    = transform;
            params.cullRadius       = swSize;
            params.flags            = DrawFlags::VisibleInMain | DrawFlags::ExcludeFromTLAS;
            params.colorOverride    = {1.0f, 0.8f, 0.5f, shockOpacity};
            params.emissiveOverride = {15.0f, 10.0f, 4.0f, shockOpacity};

            Renderer::Draw(rc, shockwaveMat, quadMesh, params);
        }

        // 4. DRAW SPARKS LAST (Foreground bright embers)
        float sparkOpacity = std::max(0.0f, 1.0f - age / 1.8f);
        if (sparkOpacity > 0.001f) {
            for (const auto& p: sparks) {
                if (p.life < 0.0f)
                    continue;
                float t = std::min(1.0f, p.life / p.maxLife);

                JPH::Vec3 color;
                if (t < 0.4f) {
                    color = p.colorStart + (p.colorMid - p.colorStart) * (t / 0.4f);
                } else {
                    color = p.colorMid + (p.colorEnd - p.colorMid) * ((t - 0.4f) / 0.6f);
                }
                float     flick      = 0.7f + Hash31(p.position) * 0.6f;
                float     fade       = 1.0f - std::pow(t, 2.0f);
                JPH::Vec3 finalColor = color * (fade * flick * sparkOpacity);

                float      size      = std::lerp(p.startSize, p.endSize, t);
                JPH::Mat44 transform = Math::CreateTransform(origin + p.position, finalBillboardRot, JPH::Vec3::sReplicate(size));

                DrawParams params;
                params.transform        = transform;
                params.prevTransform    = transform;
                params.cullRadius       = size;
                params.flags            = DrawFlags::VisibleInMain | DrawFlags::ExcludeFromTLAS;
                params.colorOverride    = {finalColor.GetX(), finalColor.GetY(), finalColor.GetZ(), fade * sparkOpacity};
                params.emissiveOverride = {finalColor.GetX() * 25.0f, finalColor.GetY() * 25.0f, finalColor.GetZ() * 25.0f, 1.0f};

                Renderer::Draw(rc, sparkMat, quadMesh, params);
            }
        }
    }

  private:
    void UpdateGroup(std::vector<ExplosionParticle>& particles, float dt, bool isFire) {
        for (auto& p: particles) {
            p.life += dt;
            if (p.life < 0)
                continue;

            float dragFactor = std::exp(-p.drag * dt);
            p.velocity *= dragFactor;
            p.velocity.SetY(p.velocity.GetY() + p.gravity * dt * (isFire ? 0.4f : 1.0f));
            p.position += p.velocity * dt;
        }
    }

    void UpdateSparks(float dt) {
        for (auto& p: sparks) {
            p.life += dt;
            float dragFactor = std::exp(-p.drag * dt);
            p.velocity *= dragFactor;
            p.velocity.SetY(p.velocity.GetY() + p.gravity * dt);
            p.position += p.velocity * dt;

            // Simple analytical collision against local ground plane
            float localGroundY = -origin.GetY() + 0.02f;
            if (p.position.GetY() < localGroundY && p.velocity.GetY() < 0.0f) {
                p.position.SetY(localGroundY);
                p.velocity.SetY(p.velocity.GetY() * -0.35f);
                p.velocity.SetX(p.velocity.GetX() * 0.6f);
                p.velocity.SetZ(p.velocity.GetZ() * 0.6f);
            }
        }
    }
};

export class ExplosionSystem {
  public:
    static void Init(RenderContext& rc) {
        if (s_Init)
            return;
        s_Init = true;

        ZHLN::Log("[Explosions] Initializing procedural visual assets...");

        // Generate and upload textures with explicit diagnostic logging
        auto firePixels = GenerateFireTexture(256);
        auto fireRes    = rc.CreateTexture(firePixels.data(), 256, 256, true);
        if (fireRes) {
            s_FireTex = fireRes.value();
            ZHLN::Log("[Explosions] Fire texture loaded successfully at slot {}.", s_FireTex);
        } else {
            s_FireTex = 1;
            ZHLN::Log("[Explosions] ERROR: Failed to load fire texture. Falling back to slot 1.");
        }

        auto smokePixels = GenerateSmokeTexture(256);
        auto smokeRes    = rc.CreateTexture(smokePixels.data(), 256, 256, true);
        if (smokeRes) {
            s_SmokeTex = smokeRes.value();
            ZHLN::Log("[Explosions] Smoke texture loaded successfully at slot {}.", s_SmokeTex);
        } else {
            s_SmokeTex = 1;
            ZHLN::Log("[Explosions] ERROR: Failed to load smoke texture. Falling back to slot 1.");
        }

        auto sparkPixels = GenerateSparkTexture(128);
        auto sparkRes    = rc.CreateTexture(sparkPixels.data(), 128, 128, true);
        if (sparkRes) {
            s_SparkTex = sparkRes.value();
            ZHLN::Log("[Explosions] Spark texture loaded successfully at slot {}.", s_SparkTex);
        } else {
            s_SparkTex = 1;
            ZHLN::Log("[Explosions] ERROR: Failed to load spark texture. Falling back to slot 1.");
        }

        auto shockwavePixels = GenerateShockwaveTexture(512);
        auto shockwaveRes    = rc.CreateTexture(shockwavePixels.data(), 512, 512, true);
        if (shockwaveRes) {
            s_ShockwaveTex = shockwaveRes.value();
            ZHLN::Log("[Explosions] Shockwave texture loaded successfully at slot {}.", s_ShockwaveTex);
        } else {
            s_ShockwaveTex = 1;
            ZHLN::Log("[Explosions] ERROR: Failed to load shockwave texture. Falling back to slot 1.");
        }

        // Load unit quad mesh
        s_QuadMesh = CreativeWorksFactory::CreatePlane(rc, 0.5f);

        // Build translucency materials
        auto fireMat_res = CreativeWorksFactory::CreateBasicMaterial(rc, true, true, true);
        if (fireMat_res) {
            s_FireMat               = fireMat_res.value();
            s_FireMat.albedoIndex   = s_FireTex;
            s_FireMat.emissiveIndex = s_FireTex; // Bind emissive map to particle texture
            s_FireMat.alphaMode     = 2;         // Translucent
        }

        auto smokeMat_res = CreativeWorksFactory::CreateBasicMaterial(rc, true, true, false);
        if (smokeMat_res) {
            s_SmokeMat               = smokeMat_res.value();
            s_SmokeMat.albedoIndex   = s_SmokeTex;
            s_SmokeMat.emissiveIndex = s_SmokeTex; // Bind emissive map to particle texture
            s_SmokeMat.alphaMode     = 2;
        }

        auto sparkMat_res = CreativeWorksFactory::CreateBasicMaterial(rc, true, true, true);
        if (sparkMat_res) {
            s_SparkMat               = sparkMat_res.value();
            s_SparkMat.albedoIndex   = s_SparkTex;
            s_SparkMat.emissiveIndex = s_SparkTex; // Bind emissive map to particle texture
            s_SparkMat.alphaMode     = 2;
        }

        auto shockwaveMat_res = CreativeWorksFactory::CreateBasicMaterial(rc, true, true, true);
        if (shockwaveMat_res) {
            s_ShockwaveMat               = shockwaveMat_res.value();
            s_ShockwaveMat.albedoIndex   = s_ShockwaveTex;
            s_ShockwaveMat.emissiveIndex = s_ShockwaveTex; // Bind emissive map to particle texture
            s_ShockwaveMat.alphaMode     = 2;
        }
    }

    static void Spawn(Engine* engine, const JPH::Vec3& position, float scale = 1.0f) {
        Init(engine->GetRenderContext());
        s_Explosions.emplace_back(engine, position, scale);
    }

    static void Update(Engine* engine, float dt) {
        if (!s_Init)
            return;

        auto& rc  = engine->GetRenderContext();
        auto& cam = engine->GetCamera();

        for (auto it = s_Explosions.begin(); it != s_Explosions.end();) {
            it->Update(engine, dt);
            if (it->finished) {
                it->Destroy(engine);
                it = s_Explosions.erase(it);
            } else {
                it->Render(rc, cam, s_QuadMesh, s_FireMat, s_SmokeMat, s_SparkMat, s_ShockwaveMat);
                ++it;
            }
        }
    }

    static void Reset() noexcept {
        s_Explosions.clear();
    }

  private:
    inline static bool     s_Init         = false;
    inline static uint32_t s_FireTex      = 1;
    inline static uint32_t s_SmokeTex     = 1;
    inline static uint32_t s_SparkTex     = 1;
    inline static uint32_t s_ShockwaveTex = 1;

    inline static Mesh     s_QuadMesh {};
    inline static Material s_FireMat {};
    inline static Material s_SmokeMat {};
    inline static Material s_SparkMat {};
    inline static Material s_ShockwaveMat {};

    inline static std::vector<ExplosionInstance> s_Explosions;
};

} // namespace ZHLN
