// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module;
#include <Jolt/Jolt.h>
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
#include <Zahlen/Camera.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Types.hpp>
#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

export module ZHLN.CombatFX;

export namespace ZHLN::CombatFX {

struct VisualParticle {
    JPH::Vec3 position;
    JPH::Vec3 velocity;
    JPH::Vec4 color;
    float     size;
    float     life;
    float     maxLife;
    float     drag;
    float     gravity;
};

struct BulletTracer {
    JPH::Vec3 start;
    JPH::Vec3 direction;
    float     speed;
    float     length;
    float     totalDistance;
    float     traveled;
    JPH::Vec3 color;
};

struct KineticShockwave {
    JPH::Vec3 position;
    JPH::Vec3 direction;
    float     radius;
    float     life;
    float     maxLife;
};

struct BulletDecal {
    JPH::Mat44 transform;
    float      life;
    float      maxLife;
};

inline std::vector<BulletDecal> g_Decals;
inline uint32_t                 g_DecalTex = 0;

inline std::vector<uint32_t> GenerateBulletHoleTexture(uint32_t size = 128) {
    std::vector<uint32_t> pixels(size * size);
    float                 cx   = size / 2.0f;
    float                 cy   = size / 2.0f;
    float                 maxR = size / 2.0f;

    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            float dx = x - cx;
            float dy = y - cy;
            float r  = std::sqrt(dx * dx + dy * dy) / maxR;

            float   alpha = 0.0f;
            uint8_t col   = 18;
            if (r <= 0.20f) {
                alpha = 0.95f;
                col   = 12;
            } else if (r <= 0.60f) {
                float t = (r - 0.20f) / 0.40f;
                alpha   = (1.0f - t) * 0.85f;
                col     = static_cast<uint8_t>(12 + t * 35);
            } else if (r <= 1.0f) {
                float t = (r - 0.60f) / 0.40f;
                alpha   = (1.0f - t) * 0.25f;
                col     = static_cast<uint8_t>(47 + t * 25);
            }

            uint8_t a            = static_cast<uint8_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
            pixels[y * size + x] = (a << 24) | (col << 16) | (col << 8) | col;
        }
    }
    return pixels;
}

void SpawnDecal(const JPH::Vec3& point, const JPH::Vec3& normal, float size = 0.28f) {
    JPH::Vec3 zAxis = -normal.Normalized();
    JPH::Vec3 up    = (std::abs(zAxis.GetY()) > 0.95f) ? JPH::Vec3(1, 0, 0) : JPH::Vec3(0, 1, 0);
    JPH::Vec3 xAxis = up.Cross(zAxis).Normalized();
    JPH::Vec3 yAxis = zAxis.Cross(xAxis).Normalized();

    JPH::Mat44 rot = JPH::Mat44(JPH::Vec4(xAxis, 0.0f), JPH::Vec4(yAxis, 0.0f), JPH::Vec4(zAxis, 0.0f), JPH::Vec4(0.0f, 0.0f, 0.0f, 1.0f));

    JPH::Vec3  scale(size, size, 0.4f);
    JPH::Mat44 transform = Math::CreateTransform(point, rot.GetQuaternion().Normalized(), scale);

    BulletDecal decal;
    decal.transform = transform;
    decal.life      = 20.0f;
    decal.maxLife   = 20.0f;

    g_Decals.push_back(decal);
    if (g_Decals.size() > 64) {
        g_Decals.erase(g_Decals.begin());
    }
}

void SpawnImpactParticles(std::vector<VisualParticle>& particles, const JPH::Vec3& point, const JPH::Vec3& normal, uint32_t materialType) {
    std::mt19937                          gen(std::random_device {}());
    std::uniform_real_distribution<float> randomDist(-1.0f, 1.0f);

    if (materialType == 1) { // Blood
        JPH::Vec4 bloodColor(0.61f, 0.11f, 0.11f, 0.95f);
        for (int i = 0; i < 12; ++i) {
            VisualParticle p;
            p.position = point;
            p.velocity = normal * (1.5f + (randomDist(gen) + 1.0f) * 1.75f) +
                         JPH::Vec3(randomDist(gen) * 1.3f, (randomDist(gen) + 1.0f) * 0.9f, randomDist(gen) * 1.3f);
            p.color    = bloodColor;
            p.size     = 0.055f + (randomDist(gen) + 1.0f) * 0.025f;
            p.life     = 0.55f + (randomDist(gen) + 1.0f) * 0.2f;
            p.maxLife  = p.life;
            p.drag     = 1.2f;
            p.gravity  = -11.0f;
            particles.push_back(p);
        }
    } else { // Concrete / Metal (Sparks + Decals)
        SpawnDecal(point, normal, 0.28f);

        JPH::Vec4 sparkColor(1.0f, 0.81f, 0.56f, 1.0f);
        int       count = (materialType == 2) ? 12 : 7;
        for (int i = 0; i < count; ++i) {
            VisualParticle p;
            p.position = point;
            p.velocity = normal * (2.0f + (randomDist(gen) + 1.0f) * 2.5f) +
                         JPH::Vec3(randomDist(gen) * 2.0f, (randomDist(gen) + 1.0f) * 1.25f, randomDist(gen) * 2.0f);
            p.color    = sparkColor;
            p.size     = 0.03f + (randomDist(gen) + 1.0f) * 0.015f;
            p.life     = 0.25f + (randomDist(gen) + 1.0f) * 0.15f;
            p.maxLife  = p.life;
            p.drag     = 1.5f;
            p.gravity  = -14.0f;
            particles.push_back(p);
        }
    }
}

void ProcessRenderTick(
    Engine*                        engine,
    float                          dt,
    std::vector<BulletTracer>&     tracers,
    std::vector<KineticShockwave>& shockwaves,
    std::vector<VisualParticle>&   particles,
    const Material&                tracerMat,
    const Material&                particleMat
) {
    auto& rc  = engine->GetRenderContext();
    auto& cam = engine->GetCamera();

    // 1. Process Screenspace Decals
    if (g_DecalTex == 0) {
        auto holePixels = GenerateBulletHoleTexture(128);
        auto texRes     = rc.CreateTexture(holePixels.data(), 128, 128, true);
        if (texRes) {
            g_DecalTex = texRes.value();
        }
    }

    if (g_DecalTex != 0) {
        for (auto it = g_Decals.begin(); it != g_Decals.end();) {
            it->life -= dt;
            if (it->life <= 0.0f) {
                it = g_Decals.erase(it);
            } else {
                DecalParams dParams;
                dParams.transform    = it->transform;
                dParams.invTransform = it->transform.Inversed();
                dParams.albedoIndex  = g_DecalTex;
                dParams.normalIndex  = 2; // Flat normal
                dParams.roughness    = 0.9f;
                dParams.metallic     = 0.05f;

                Renderer::DrawDecal(rc, dParams);
                ++it;
            }
        }
    }

    AssetID unitBoxAsset = HashAssetID("unit_box");
    if (!rc.GetGPUMesh(unitBoxAsset).has_value()) {
        rc.RegisterGPUMesh(unitBoxAsset, CreativeWorksFactory::CreateBox(rc, JPH::Vec3(0.5f, 0.5f, 0.5f)));
    }

    // 2. Process CPU Tracer Lines
    for (auto it = tracers.begin(); it != tracers.end();) {
        it->traveled += it->speed * dt;
        if (it->traveled - it->length > it->totalDistance) {
            it = tracers.erase(it);
        } else {
            float     head      = std::min(it->traveled, it->totalDistance);
            float     tail      = std::max(0.0f, it->traveled - it->length);
            JPH::Vec3 headPoint = it->start + it->direction * head;
            JPH::Vec3 tailPoint = it->start + it->direction * tail;

            float     length   = (headPoint - tailPoint).Length();
            JPH::Vec3 midPoint = (headPoint + tailPoint) * 0.5f;

            if (length > 0.01f) {
                JPH::Mat44 transform =
                    Math::CreateTransform(midPoint, JPH::Quat::sFromTo(JPH::Vec3::sAxisZ(), it->direction), JPH::Vec3(0.015f, 0.015f, length));

                DrawParams params;
                params.transform        = transform;
                params.prevTransform    = transform;
                params.cullRadius       = length;
                params.flags            = DrawFlags::VisibleInMain | DrawFlags::ExcludeFromTLAS;
                params.colorOverride    = {it->color.GetX(), it->color.GetY(), it->color.GetZ(), 1.0f};
                params.emissiveOverride = {it->color.GetX() * 25.0f, it->color.GetY() * 25.0f, it->color.GetZ() * 25.0f, 1.0f};

                Renderer::Draw(rc, tracerMat, *rc.GetGPUMesh(unitBoxAsset), params);
            }
            ++it;
        }
    }

    // 3. Process Kinetic Shockwave Rings
    AssetID planeMesh = HashAssetID("unit_plane");
    if (!rc.GetGPUMesh(planeMesh).has_value()) {
        rc.RegisterGPUMesh(planeMesh, CreativeWorksFactory::CreatePlane(rc, 1.0f));
    }

    for (auto it = shockwaves.begin(); it != shockwaves.end();) {
        it->life -= dt;
        if (it->life <= 0.0f) {
            it = shockwaves.erase(it);
        } else {
            float     t         = 1.0f - (it->life / it->maxLife);
            float     curRadius = it->radius * t;
            JPH::Quat rot       = JPH::Quat::sFromTo(JPH::Vec3::sAxisY(), it->direction);

            JPH::Mat44 transform = Math::CreateTransform(it->position, rot, JPH::Vec3(curRadius, 1.0f, curRadius));

            DrawParams params;
            params.transform        = transform;
            params.prevTransform    = transform;
            params.cullRadius       = curRadius;
            params.flags            = DrawFlags::VisibleInMain | DrawFlags::ExcludeFromTLAS;
            params.colorOverride    = {0.65f, 0.90f, 1.00f, (1.0f - t) * 0.8f};
            params.emissiveOverride = {10.0f, 20.0f, 30.0f, 1.0f};

            Renderer::Draw(rc, tracerMat, *rc.GetGPUMesh(planeMesh), params);
            ++it;
        }
    }

    // 4. Process Billboard Particles
    JPH::Mat44 invView = cam.GetViewMatrix().Inversed();
    JPH::Vec3  right   = invView.GetColumn3(0).Normalized();
    JPH::Vec3  up      = invView.GetColumn3(1).Normalized();
    JPH::Vec3  back    = invView.GetColumn3(2).Normalized();

    JPH::Mat44 billboardMat(JPH::Vec4(right, 0.0f), JPH::Vec4(back, 0.0f), JPH::Vec4(-up, 0.0f), JPH::Vec4(0.0f, 0.0f, 0.0f, 1.0f));
    JPH::Quat  billboardRot = billboardMat.GetQuaternion().Normalized();

    AssetID particleMesh = HashAssetID("procedural_particle_mesh");
    if (!rc.GetGPUMesh(particleMesh).has_value()) {
        rc.RegisterGPUMesh(particleMesh, CreativeWorksFactory::CreatePlane(rc, 0.5f));
    }

    for (auto it = particles.begin(); it != particles.end();) {
        it->life -= dt;
        if (it->life <= 0.0f) {
            it = particles.erase(it);
        } else {
            it->velocity.SetY(it->velocity.GetY() + it->gravity * dt);
            it->velocity *= std::max(0.0f, 1.0f - it->drag * dt);
            it->position += it->velocity * dt;

            if (it->position.GetY() < 0.02f) {
                it->position.SetY(0.02f);
                it->velocity.SetY(it->velocity.GetY() * -0.25f);
                it->velocity.SetX(it->velocity.GetX() * 0.6f);
                it->velocity.SetZ(it->velocity.GetZ() * 0.6f);
            }

            float      t         = it->life / it->maxLife;
            float      size      = it->size * (0.4f + t * 0.8f);
            JPH::Mat44 transform = Math::CreateTransform(it->position, billboardRot, JPH::Vec3::sReplicate(size));

            DrawParams params;
            params.transform        = transform;
            params.prevTransform    = transform;
            params.cullRadius       = size;
            params.flags            = DrawFlags::VisibleInMain | DrawFlags::ExcludeFromTLAS;
            params.colorOverride    = {it->color.GetX(), it->color.GetY(), it->color.GetZ(), std::min(1.0f, t * 1.6f)};
            params.emissiveOverride = {it->color.GetX() * 15.0f, it->color.GetY() * 15.0f, it->color.GetZ() * 15.0f, 1.0f};

            Renderer::Draw(rc, particleMat, *rc.GetGPUMesh(particleMesh), params);
            ++it;
        }
    }
}

} // namespace ZHLN::CombatFX
