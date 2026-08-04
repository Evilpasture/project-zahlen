// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Zahlen/Audio.hpp"
#include "Zahlen/Components.hpp"
#include "Zahlen/CreativeWorksFactory.hpp"
#include "Zahlen/CreativeWorksManager.hpp"
#include "Zahlen/Engine.hpp"
#include "Zahlen/Input.hpp"
#include "Zahlen/Log.hpp"
#include "Zahlen/Math3D.hpp"
#include "Zahlen/Profiler.hpp"
#include "Zahlen/ScriptECSBridge.hpp"
#include "Zahlen/Window.hpp"
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>

// C++ Standard Library & Engine Modules
import std;
import ZHLN.MainMenu;
import ZHLN.Lightning;
import ZHLN.Explosions;

#if defined(_WIN32)
#define GAMEPLAY_API extern "C" __declspec(dllexport)
#else
#define GAMEPLAY_API extern "C" [[gnu::visibility("default")]]
#endif

namespace Game {

struct GameplayComponents {
    struct Combat {
        float hp         = 100.0f;
        float maxHp      = 100.0f;
        bool  isPoisoned = false;
    };

    struct Vector3 {
        float x = 0.0f, y = 0.0f, z = 0.0f;
    };

    struct Path {
        std::vector<Vector3> waypoints;
    };
};

} // namespace Game

namespace Game {

using namespace ZHLN;

struct SnowSceneState {
    ScriptECSBridge*    bridge = nullptr;
    MainMenu            mainMenu;
    LightningSimulation lightningSim;
    bool                gameStarted = false;
    bool                wonGame     = false;
    bool                wasLMouseDown = false;
    bool                wasRDown    = false;
    float               totalTime   = 0.0f;

    Entity playerEnt       = NullEntity;
    Entity playerCameraEnt = NullEntity; // Cached once in RespawnPlayer
    Entity snowTerrain     = NullEntity;
    Entity testPlatform    = NullEntity;
    Entity campfireLight   = NullEntity;
    Entity summitLight     = NullEntity;
    Entity wisp1           = NullEntity;
    Entity wisp2           = NullEntity;
    Entity blizzardEmitter = NullEntity; // Track active particle emitter

    std::vector<Entity> charParts;
    std::string         currentAnimState = "IDLE";
};

static SnowSceneState g_State;

// Small combinators removing the repetitive null-check dance

template<typename T, typename Fn>
inline bool Patch(ECS::Registry& reg, Entity e, Fn&& fn) {
    if (auto* c = reg.Get<T>(e)) {
        fn(*c);
        return true;
    }
    return false;
}

inline Entity SpawnPointLight(ECS::Registry& reg, JPH::Vec3 pos, JPH::Vec3 color, float intensity, float range, float radius = 0.4f) {
    Entity e = reg.Create();
    reg.Add(e, Components::TransformComponent{.position = pos});
    reg.Add(
        e, Components::LightComponent {
               .type        = LightType::Point,
               .color       = color,
               .intensity   = intensity,
               .radius      = radius,
               .direction   = JPH::Vec3(0, -1, 0),
               .range       = range,
               .points      = JPH::Mat44::sIdentity(),
               .twoSided    = 0,
               .shadowLayer = -1
           }
    );
    return e;
}

namespace TerrainGen {

inline float Hash2D(float x, float y) {
    float n = x * 1597.0f + y * 5147.0f;
    float h = std::fmod(n * 43758.5453f, 1.0f);
    return (h < 0.0f) ? (h + 1.0f) : h;
}

inline float Lerp(float a, float b, float t) {
    return a + t * (b - a);
}

inline float Noise2D(float x, float y) {
    float ix = std::floor(x);
    float iy = std::floor(y);
    float fx = x - ix;
    float fy = y - iy;

    float ux = fx * fx * fx * (fx * (fx * 6.0f - 15.0f) + 10.0f);
    float uy = fy * fy * fy * (fy * (fy * 6.0f - 15.0f) + 10.0f);

    float n00 = Hash2D(ix, iy);
    float n10 = Hash2D(ix + 1.0f, iy);
    float n01 = Hash2D(ix, iy + 1.0f);
    float n11 = Hash2D(ix + 1.0f, iy + 1.0f);

    return Lerp(Lerp(n00, n10, ux), Lerp(n01, n11, ux), uy);
}

inline float ComputeHeight(float x, float z, [[maybe_unused]] float maxHeight) {
    float peakX = -50.0f, peakZ = -50.0f;
    float dx = x - peakX, dz = z - peakZ;
    float distPeak     = std::sqrt(dx * dx + dz * dz);
    float mountainMask = std::exp(-(distPeak * distPeak) / (75.0f * 75.0f));

    float ridgeDist = std::abs(x * 0.6f + z * 0.8f + 15.0f);
    float ridgeMask = std::exp(-ridgeDist / 30.0f);

    float distSpawn     = std::sqrt(x * x + z * z);
    float spawnClearing = std::max(0.0f, std::min(1.0f, (distSpawn - 12.0f) / 20.0f));

    float tx = x * 0.015f, tz = z * 0.015f;
    float warpX = Noise2D(tx + 1.7f, tz + 2.3f);
    float warpZ = Noise2D(tx + 4.1f, tz + 8.5f);

    float n1 = Noise2D(tx * 2.0f + warpX * 0.6f, tz * 2.0f + warpZ * 0.6f);
    float n2 = Noise2D(tx * 4.5f, tz * 4.5f) * 0.5f;
    float n3 = Noise2D(tx * 9.0f, tz * 9.0f) * 0.25f;

    float r          = 1.0f - std::abs(Noise2D(tx * 3.5f, tz * 3.5f) * 2.0f - 1.0f);
    float sharpRidge = r * r * r;

    float baseHills         = (n1 + n2 + n3) * 5.0f;
    float mountainElevation = (mountainMask * 32.0f + ridgeMask * 16.0f) * (0.5f + sharpRidge * 0.9f);

    return ((baseHills + mountainElevation) * spawnClearing) + ((1.0f - spawnClearing) * 2.0f);
}

inline void GenerateMountainData(uint32_t sampleCount, float worldSize, float maxHeight, ZHLN::Array<float>& outHeights, ZHLN::Array<float>& outColors) {
    size_t totalVerts = static_cast<size_t>(sampleCount * sampleCount);
    outHeights.resize(totalVerts);
    outColors.resize(totalVerts * 4);

    float halfSize = worldSize / 2.0f;
    float step     = worldSize / static_cast<float>(sampleCount - 1);

    for (uint32_t z = 0; z < sampleCount; ++z) {
        for (uint32_t x = 0; x < sampleCount; ++x) {
            float  worldX   = -halfSize + x * step;
            float  worldZ   = -halfSize + z * step;
            size_t idx      = x + z * sampleCount;
            outHeights[idx] = ComputeHeight(worldX, worldZ, maxHeight);
        }
    }

    for (uint32_t z = 0; z < sampleCount; ++z) {
        for (uint32_t x = 0; x < sampleCount; ++x) {
            size_t idx  = x + z * sampleCount;
            size_t cIdx = idx * 4;
            float  y    = outHeights[idx];

            uint32_t xLeft  = (x > 0) ? x - 1 : 0;
            uint32_t xRight = std::min(sampleCount - 1, x + 1);
            uint32_t zDown  = (z > 0) ? z - 1 : 0;
            uint32_t zUp    = std::min(sampleCount - 1, z + 1);

            float hL = outHeights[xLeft + z * sampleCount];
            float hR = outHeights[xRight + z * sampleCount];
            float hD = outHeights[x + zDown * sampleCount];
            float hU = outHeights[x + zUp * sampleCount];

            float dxH   = hL - hR;
            float dzH   = hD - hU;
            float dyH   = 2.0f * step;
            float len   = std::sqrt(dxH * dxH + dyH * dyH + dzH * dzH);
            float slope = dyH / len;
            float normY = y / maxHeight;

            if (slope < 0.68f) {
                float rockN         = Noise2D(x * 0.1f, z * 0.1f);
                outColors[cIdx + 0] = 0.11f + rockN * 0.04f;
                outColors[cIdx + 1] = 0.13f + rockN * 0.04f;
                outColors[cIdx + 2] = 0.16f + rockN * 0.05f;
                outColors[cIdx + 3] = 1.0f;
            } else if (slope < 0.82f) {
                float t             = (slope - 0.68f) / 0.14f;
                outColors[cIdx + 0] = Lerp(0.12f, 0.88f, t);
                outColors[cIdx + 1] = Lerp(0.14f, 0.92f, t);
                outColors[cIdx + 2] = Lerp(0.17f, 0.98f, t);
                outColors[cIdx + 3] = 1.0f;
            } else if (normY > 0.65f) {
                outColors[cIdx + 0] = 0.97f;
                outColors[cIdx + 1] = 0.98f;
                outColors[cIdx + 2] = 1.00f;
                outColors[cIdx + 3] = 1.00f;
            } else if (normY < 0.12f) {
                outColors[cIdx + 0] = 0.78f;
                outColors[cIdx + 1] = 0.88f;
                outColors[cIdx + 2] = 0.95f;
                outColors[cIdx + 3] = 1.00f;
            } else {
                float snowV         = 0.90f + 0.05f * std::sin(y * 0.4f);
                outColors[cIdx + 0] = snowV * 0.94f;
                outColors[cIdx + 1] = snowV * 0.97f;
                outColors[cIdx + 2] = snowV;
                outColors[cIdx + 3] = 1.0f;
            }
        }
    }
}

// --- Procedural Mathematical Snowflake Texture Generation ---

inline float DistanceToSegment(float px, float py, float ax, float ay, float bx, float by) {
    float abx = bx - ax;
    float aby = by - ay;
    float apx = px - ax;
    float apy = py - ay;
    float ab2 = abx * abx + aby * aby;
    if (ab2 < 1e-6f)
        return std::sqrt(apx * apx + apy * apy);
    float t        = (apx * abx + apy * aby) / ab2;
    t              = std::max(0.0f, std::min(1.0f, t));
    float closestX = ax + t * abx;
    float closestY = ay + t * aby;
    float dx       = px - closestX;
    float dy       = py - closestY;
    return std::sqrt(dx * dx + dy * dy);
}

inline float DistanceToCircle(float px, float py, float cx, float cy, float r) {
    float dx = px - cx;
    float dy = py - cy;
    return std::max(0.0f, std::sqrt(dx * dx + dy * dy) - r);
}

inline std::vector<uint32_t> GenerateSnowflakeTexture(uint32_t size) {
    std::vector<uint32_t> pixels(size * size, 0);
    float                 center = size / 2.0f;
    float                 scale  = (size / 2.0f) / 100.0f; // Map canvas pixel index to SVG viewBox space [-100, 100]

    for (uint32_t cy = 0; cy < size; ++cy) {
        for (uint32_t cx = 0; cx < size; ++cx) {
            float x = (static_cast<float>(cx) - center) / scale;
            float y = (static_cast<float>(cy) - center) / scale;

            float maxAlpha = 0.0f;

            // 1. Center Accent Dot
            {
                float d = std::sqrt(x * x + y * y);
                if (d <= 4.0f) {
                    maxAlpha = 1.0f;
                } else if (d <= 5.0f) {
                    maxAlpha = std::max(maxAlpha, 1.0f - (d - 4.0f));
                }
            }

            // 2. Center Hexagon (points: (0,-12) to (10.4,-6) to (10.4,6) to (0,12) to (-10.4,6) to (-10.4,-6))
            static const float hexX[6] = {0.0f, 10.4f, 10.4f, 0.0f, -10.4f, -10.4f};
            static const float hexY[6] = {-12.0f, -6.0f, 6.0f, 12.0f, 6.0f, -6.0f};
            for (int i = 0; i < 6; ++i) {
                float ax         = hexX[i];
                float ay         = hexY[i];
                float bx         = hexX[(i + 1) % 6];
                float by         = hexY[(i + 1) % 6];
                float d          = DistanceToSegment(x, y, ax, ay, bx, by);
                float half_thick = 1.0f; // stroke-width = 2 -> half-thickness = 1.0
                if (d <= half_thick) {
                    maxAlpha = std::max(maxAlpha, 1.0f);
                } else if (d <= half_thick + 1.0f) {
                    maxAlpha = std::max(maxAlpha, 1.0f - (d - half_thick));
                }
            }

            // 3. Repeat the single arm 6 times at 60-degree increments
            for (int k = 0; k < 6; ++k) {
                float angle = k * (3.1415926535f / 3.0f);
                float cosA  = std::cos(angle);
                float sinA  = std::sin(angle);

                // Rotated space coordinate transform (negative rotation to align)
                float rx = x * cosA + y * sinA;
                float ry = -x * sinA + y * cosA;

                auto EvalLine = [&](float ax, float ay, float bx, float by, float thick) {
                    float d          = DistanceToSegment(rx, ry, ax, ay, bx, by);
                    float half_thick = thick * 0.5f;
                    if (d <= half_thick) {
                        maxAlpha = std::max(maxAlpha, 1.0f);
                    } else if (d <= half_thick + 1.0f) {
                        maxAlpha = std::max(maxAlpha, 1.0f - (d - half_thick));
                    }
                };

                auto EvalCircle = [&](float cx, float cy, float r) {
                    float d = DistanceToCircle(rx, ry, cx, cy, r);
                    if (d <= 0.0f) {
                        maxAlpha = std::max(maxAlpha, 1.0f);
                    } else if (d <= 1.0f) {
                        maxAlpha = std::max(maxAlpha, 1.0f - d);
                    }
                };

                // Stem: line (0,0) -> (0,-85), stroke-width=4
                EvalLine(0.0f, 0.0f, 0.0f, -85.0f, 4.0f);

                // Outer large branches: (0,-60) -> (20,-75) & (0,-60) -> (-20,-75), stroke-width=3.5
                EvalLine(0.0f, -60.0f, 20.0f, -75.0f, 3.5f);
                EvalLine(0.0f, -60.0f, -20.0f, -75.0f, 3.5f);

                // Middle branches: (0,-40) -> (25,-55) & (0,-40) -> (-25,-55), stroke-width=3.5
                EvalLine(0.0f, -40.0f, 25.0f, -55.0f, 3.5f);
                EvalLine(0.0f, -40.0f, -25.0f, -55.0f, 3.5f);

                // Sub-tips: (18,-51) -> (22,-43) & (-18,-51) -> (-22,-43), stroke-width=2.5
                EvalLine(18.0f, -51.0f, 22.0f, -43.0f, 2.5f);
                EvalLine(-18.0f, -51.0f, -22.0f, -43.0f, 2.5f);

                // Inner small branches: (0,-20) -> (15,-30) & (0,-20) -> (-15,-30), stroke-width=3
                EvalLine(0.0f, -20.0f, 15.0f, -30.0f, 3.0f);
                EvalLine(0.0f, -20.0f, -15.0f, -30.0f, 3.0f);

                // Circular accents
                EvalCircle(0.0f, -85.0f, 3.0f);
                EvalCircle(20.0f, -75.0f, 2.0f);
                EvalCircle(-20.0f, -75.0f, 2.0f);
            }

            // Pack into color format (A8B8G8R8) - light blue-white #e0f2fe
            uint8_t r = static_cast<uint8_t>(224.0f * maxAlpha);
            uint8_t g = static_cast<uint8_t>(242.0f * maxAlpha);
            uint8_t b = static_cast<uint8_t>(254.0f * maxAlpha);
            uint8_t a = static_cast<uint8_t>(255.0f * maxAlpha);

            pixels[cy * size + cx] = (a << 24) | (b << 16) | (g << 8) | r;
        }
    }
    return pixels;
}

} // namespace TerrainGen

void PlayTrack(ECS::Registry& reg, Entity ent, int trackIdx, float blend = 0.15f, bool loop = true, float speed = 1.0f) {
    Patch<Components::AnimatorComponent>(reg, ent, [&](auto& anim) {
        if (anim.currentTrackIdx != trackIdx) {
            anim.prevTrackIdx         = anim.currentTrackIdx;
            anim.prevTrackTime        = anim.currentTrackTime;
            anim.currentTrackIdx      = trackIdx;
            anim.currentTrackTime     = 0.0f;
            anim.currentPlaybackSpeed = speed;
            anim.currentLoop          = loop;
            anim.blendFactor          = 0.0f;
            anim.blendDuration        = blend;
            anim.isFinished           = false;
        }
    });
}

void RespawnPlayer(Engine* engine) {
    auto& reg = engine->GetRegistry();
    auto& pc  = engine->GetPhysicsContext();

    if (g_State.playerEnt != NullEntity) {
        for (Entity part: g_State.charParts) {
            reg.Destroy(part);
        }
        g_State.charParts.clear();
        reg.Destroy(g_State.playerEnt);
    }

    JPH::Vec3 spawnPos(0.0f, 13.5f, 0.0f);

    g_State.playerEnt = reg.Create();
    reg.Add(
        g_State.playerEnt,
        Components::PlayerTagComponent {},
        Components::TransformComponent {.position = spawnPos},
        Components::MovementComponent {},
        Components::InputComponent {},
        Components::PhysicsComponent {Physics::CreateCharacter(pc, JPH::RVec3(spawnPos))},
        Components::PhysicsStateComponent {.currPosition = spawnPos, .prevPosition = spawnPos},
        GameplayComponents::Combat {.hp = 100.0f, .maxHp = 100.0f}
    );

    auto camEnts = reg.GetEntitiesWith<Components::MainCameraTagComponent>();
    if (!camEnts.empty()) {
        Entity camEnt    = camEnts[0];
        g_State.playerCameraEnt = camEnt;
        auto*  targetCam = reg.Get<Components::TargetCameraComponent>(camEnt);
        if (!targetCam) {
            targetCam = &reg.Add(camEnt, Components::TargetCameraComponent {});
        }
        targetCam->target              = g_State.playerEnt;
        targetCam->distance            = 4.5f;
        targetCam->targetDistance      = 4.5f;
        targetCam->yaw                 = -90.0f;
        targetCam->pitch               = -10.0f;
        targetCam->stiffness           = 15.0f;
        targetCam->targetOffset        = JPH::Vec3(0.0f, 1.3f, 0.0f);
        targetCam->hasInitSmoothTarget = 0;
    }

    CreativeWorksFactory::SpawnParams params;
    params.isAnimated    = true;
    params.createPhysics = false;

    g_State.charParts.resize(32);
    uint32_t count = CreativeWorksFactory::InstantiatePrefab(*engine, "murderdrones/Uzi.glb", params, g_State.charParts.data(), 32);
    g_State.charParts.resize(count);

    if (!g_State.charParts.empty()) {
        Entity charRoot = g_State.charParts[0];
        Patch<Components::TransformComponent>(reg, charRoot, [&](auto& rootTrans) {
            rootTrans.position = JPH::Vec3(0.0f, -0.8f, 0.0f);
        });
        reg.Add(charRoot, Components::HierarchyComponent {.parent = g_State.playerEnt});
        CreativeWorksFactory::SetupPlayerRagdoll(*engine, g_State.playerEnt, g_State.charParts);
    }

    ZHLN::Log("[Snow Scene] Player successfully respawned!");
}

void StartGame(Engine* engine) {
    auto& reg = engine->GetRegistry();
    auto& pc  = engine->GetPhysicsContext();
    auto& rc  = engine->GetRenderContext();

    ZHLN::Log("[Snow Scene] Generating Volumetric Nighttime Environment...");
    g_State.wonGame   = false;
    g_State.totalTime = 0.0f;

    // Destroy previous blizzard emitter if any
    if (g_State.blizzardEmitter != NullEntity) {
        reg.Destroy(g_State.blizzardEmitter);
        g_State.blizzardEmitter = NullEntity;
    }

    auto settingsEntities = reg.GetEntitiesWith<Components::GlobalSettingsTagComponent>();
    if (!settingsEntities.empty()) {
        Patch<Components::PostProcessSettingsComponent>(reg, settingsEntities[0], [&](auto& pp) {
            pp.giMode            = 2;
            pp.aoRadius          = 1.4f;
            pp.aoBias            = 0.02f;
            pp.aoPower           = 2.2f;
            pp.giIntensity       = 2.2f;
            pp.giSamples         = 24;
            pp.useLocalProbe     = 0;
            pp.vignetteIntensity = 1.25f;
            pp.vignettePower     = 1.8f;
            pp.enableSSR         = 0;
            pp.enableRTR         = 1;
            pp.ambientExposure   = 4.5f;
        });
    }

    JPH::Vec3 halfExtents(20.0f, 0.5f, 20.0f);
    Mesh      boxMesh  = CreativeWorksFactory::CreateBox(rc, halfExtents, {0.15f, 0.22f, 0.35f, 1.0f});
    auto      boxShape = Physics::GetOrCreateShape(pc, Physics::ShapeType::Box, halfExtents.GetX(), halfExtents.GetY(), halfExtents.GetZ());

    AssetID platformMeshAsset = HashAssetID("test_platform_mesh");
    AssetID platformMatAsset  = HashAssetID("test_platform_mat");

    rc.RegisterGPUMesh(platformMeshAsset, boxMesh);

    auto mat_res = CreativeWorksFactory::CreateBasicMaterial(rc);
    if (mat_res) {
        Material mat        = mat_res.value();
        mat.roughnessFactor = 0.15f;
        mat.metallicFactor  = 0.10f;
        rc.RegisterGPUMaterial(platformMatAsset, mat);
    }

    g_State.testPlatform = reg.Create();
    reg.Add(g_State.testPlatform, Components::NameComponent {.name = String64("TestPlatform")});
    reg.Add(g_State.testPlatform, Components::TransformComponent {.position = JPH::Vec3(0.0f, 12.0f, 0.0f)});
    reg.Add(g_State.testPlatform, Components::MeshComponent {.meshAsset = platformMeshAsset, .materialAsset = platformMatAsset, .cullRadius = 100.0f});
    reg.Add(
        g_State.testPlatform, Components::PhysicsComponent {
                                  Physics::CreateRigidBody(pc, boxShape, JPH::RVec3(0.0f, 12.0f, 0.0f), JPH::Quat::sIdentity(), JPH::EMotionType::Static, 0)
                              }
    );
    reg.Add(g_State.testPlatform, Components::PBRComponent {.roughness = 0.15f, .metallic = 0.10f});

    RespawnPlayer(engine);

    uint32_t           samples   = 128;
    float              worldSize = 280.0f;
    float              maxHeight = 35.0f;
    ZHLN::Array<float> heights, colors;
    TerrainGen::GenerateMountainData(samples, worldSize, maxHeight, heights, colors);

    auto terrainShape = Physics::CreateHeightFieldShape(heights.data(), samples, worldSize);

    g_State.snowTerrain = reg.Create();
    reg.Add(g_State.snowTerrain, Components::TransformComponent {});

    AssetID    terrainMeshAsset = HashAssetID("terrain_mountain_mesh");
    MaterialID terrainMatAsset  = HashAssetID("terrain_mountain_mat");

    reg.Add(g_State.snowTerrain, Components::MeshComponent {.meshAsset = terrainMeshAsset, .materialAsset = terrainMatAsset, .cullRadius = 400.0f});

    reg.Add(
        g_State.snowTerrain, Components::TerrainComponent {
                                 .sampleCount = samples,
                                 .worldSize   = worldSize,
                                 .maxHeight   = maxHeight,
                                 .roughness   = 0.85f,
                                 .metallic    = 0.05f,
                                 .heights     = std::move(heights),
                                 .colors      = std::move(colors)
                             }
    );

    reg.Add(
        g_State.snowTerrain,
        Components::PhysicsComponent {Physics::CreateRigidBody(pc, terrainShape, JPH::RVec3::sZero(), JPH::Quat::sIdentity(), JPH::EMotionType::Static, 0)}
    );
    reg.Add(g_State.snowTerrain, Components::PBRComponent {.roughness = 0.85f, .metallic = 0.05f});

    CreativeWorksFactory::SpawnParams cp;
    cp.position = JPH::RVec3(0.0f, 80.0f, -500.0f);
    cp.rotation = JPH::Quat(0.35f, 0.25f, 0.1f, 0.9f).Normalized();
    cp.scale    = JPH::Vec3(5.0f, 5.0f, 5.0f);

    CreativeWorksFactory::InstantiatePrefab(*engine, "murderdrones/Copper9_Celestials.glb", cp);

    Entity moonlight = reg.Create();
    reg.Add(
        moonlight, Components::TransformComponent {
                       .position = JPH::Vec3::sZero(), .rotation = JPH::Quat::sFromTo(JPH::Vec3::sAxisZ(), JPH::Vec3(0.0f, 0.28f, -0.96f).Normalized())
                   }
    );
    reg.Add(
        moonlight, Components::LightComponent {
                       .type        = LightType::Sun,
                       .color       = JPH::Vec3(0.82f, 0.92f, 1.0f),
                       .intensity   = 18.0f,
                       .radius      = 1.2f,
                       .direction   = JPH::Vec3(0.0f, 0.28f, -0.96f).Normalized(),
                       .range       = 600.0f,
                       .points      = JPH::Mat44::sIdentity(),
                       .twoSided    = 0,
                       .shadowLayer = -1
                   }
    );
    reg.Add(moonlight, Components::SunTagComponent {});

    g_State.campfireLight = SpawnPointLight(reg, JPH::Vec3(4.0f, 3.5f, 4.0f), JPH::Vec3(1.0f, 0.45f, 0.08f), 120.0f, 25.0f, 0.6f);

    g_State.wisp1 = SpawnPointLight(reg, JPH::Vec3(-15.0f, 12.0f, -15.0f), JPH::Vec3(0.1f, 0.75f, 1.0f), 60.0f, 20.0f, 0.4f);

    g_State.wisp2 = SpawnPointLight(reg, JPH::Vec3(20.0f, 18.0f, -30.0f), JPH::Vec3(0.3f, 0.6f, 1.0f), 60.0f, 20.0f, 0.4f);

    g_State.summitLight = SpawnPointLight(reg, JPH::Vec3(-50.0f, 38.0f, -50.0f), JPH::Vec3(1.0f, 0.85f, 0.2f), 400.0f, 50.0f, 0.8f);

    // 1. Bake the anti-aliased mathematical SVG snowflake texture onto the GPU
    auto     snowPixels   = TerrainGen::GenerateSnowflakeTexture(256);
    auto     snowRes      = rc.CreateTexture(snowPixels.data(), 256, 256, false);
    uint32_t snowTexIndex = snowRes.value_or(1);

    // 2. Create the camera-relative spatial blizzard emitter
    g_State.blizzardEmitter = reg.Create();
    reg.Add(g_State.blizzardEmitter, Components::TransformComponent {});
    reg.Add(g_State.blizzardEmitter, Components::NameComponent {.name = String64("BlizzardEmitter")});

    ParticleEmitterParams pParams {};
    pParams.gravity        = {0.0f, -1.8f, 0.0f}; // Soft falling speed
    pParams.drag           = 0.12f;
    pParams.turbulence     = {1.8f, 0.4f, 1.8f}; // Dynamic drifting winds
    pParams.turbulenceFreq = 0.15f;
    pParams.spawnBoxExtent = {60.0f, 25.0f, 60.0f}; // Local bounding volume around camera
    pParams.initVelMin     = {-1.5f, -0.5f, -1.5f};
    pParams.initVelMax     = {1.5f, 0.2f, 1.5f};
    pParams.lifetimeMin    = 5.0f;
    pParams.lifetimeMax    = 9.0f;
    pParams.startColor     = {0.88f, 0.95f, 1.0f, 0.85f}; // Light blue-white #e0f2fe
    pParams.endColor       = {0.88f, 0.95f, 1.0f, 0.0f};  // Fade out
    pParams.startSize      = {0.35f, 0.35f};
    pParams.endSize        = {0.20f, 0.20f};
    pParams.spinSpeed      = 1.2f; // Angular spin
    pParams.textureIndex   = snowTexIndex;
    pParams.alignment      = ParticleAlignment::CameraBillboard;
    pParams.loopBoundary   = 1.0f; // Wrap particles within camera volume

    reg.Add(
        g_State.blizzardEmitter, Components::ParticleEmitterComponent {
                                     .params         = pParams,
                                     .maxParticles   = 16384,
                                     .active         = true,
                                     .attachToCamera = true // Track player camera coordinates
                                 }
    );

    g_State.gameStarted = true;
}

// ============================================================================
// LIGHTNING CLICK STRIKE SYSTEM
// ============================================================================
void LightningClickStrikeSystem(Engine* engine, float dt) {
    const auto& input = engine->GetInput();
    const auto& cam   = engine->GetCamera();
    auto        mouse = input.GetMouse();
    auto        win   = engine->GetWindow().GetSize();

    bool isLMouseDown = input.IsMouseButtonDown(KeyCode::LButton);

    if (isLMouseDown && !g_State.wasLMouseDown) {
        if (win.width > 0 && win.height > 0) {
            float ndcX   = (2.0f * mouse.x) / static_cast<float>(win.width) - 1.0f;
            float ndcY   = (2.0f * mouse.y) / static_cast<float>(win.height) - 1.0f;
            float aspect = static_cast<float>(win.width) / static_cast<float>(win.height);

            JPH::Mat44 invVP     = (cam.GetProjectionMatrix(aspect) * cam.GetViewMatrix()).Inversed();
            JPH::Vec4  nearWorld = invVP * JPH::Vec4(ndcX, ndcY, 0.0f, 1.0f);
            JPH::Vec4  farWorld  = invVP * JPH::Vec4(ndcX, ndcY, 1.0f, 1.0f);

            JPH::Vec3 pNear = JPH::Vec3(nearWorld.GetX() / nearWorld.GetW(), nearWorld.GetY() / nearWorld.GetW(), nearWorld.GetZ() / nearWorld.GetW());
            JPH::Vec3 pFar  = JPH::Vec3(farWorld.GetX() / farWorld.GetW(), farWorld.GetY() / farWorld.GetW(), farWorld.GetZ() / farWorld.GetW());
            JPH::Vec3 dir   = (pFar - pNear).Normalized();

            Entity ignorePhys = NullEntity;
            Patch<Components::PhysicsComponent>(engine->GetRegistry(), g_State.playerEnt, [&](auto& phys) {
                ignorePhys = phys.physicsHandle;
            });

            auto hit = Physics::Raycast(engine->GetPhysicsContext(), JPH::RVec3(pNear), dir, 2000.0f, ignorePhys);

            JPH::RVec3 impactPos;
            if (hit.hasHit) {
                impactPos = hit.position;
            } else {
                if (std::abs(dir.GetY()) > 1e-4f) {
                    float t = -pNear.GetY() / dir.GetY();
                    if (t > 0.0f) {
                        impactPos = JPH::RVec3(pNear + dir * t);
                    } else {
                        impactPos = JPH::RVec3(0.0f, 0.0f, 0.0f);
                    }
                }
            }

            float surfaceHeight = TerrainGen::ComputeHeight(static_cast<float>(impactPos.GetX()), static_cast<float>(impactPos.GetZ()), 35.0f);
            impactPos.SetY(std::max(impactPos.GetY(), static_cast<double>(surfaceHeight)));

            JPH::RVec3 cloudPos = impactPos + JPH::RVec3(0.0f, 140.0f, 0.0f);

            ZHLN::LightningConfig cfg;
            cfg.peakCurrentKA = 50.0f;
            cfg.timeDilation  = 1.0f;

            g_State.lightningSim.TriggerStrike(*engine, cloudPos, impactPos, cfg);

            // Spawn explosion exactly at impact coordinate
            ExplosionSystem::Spawn(engine, JPH::Vec3(impactPos), 2.2f);
        }
    }
    g_State.wasLMouseDown = isLMouseDown;

    g_State.lightningSim.Update(*engine, dt);
    ExplosionSystem::Update(engine, dt);
}

void PlayerInputSystem(Engine* engine, [[maybe_unused]] float dt) {
    auto&       reg   = engine->GetRegistry();
    const auto& input = engine->GetInput();
    const auto& cam   = engine->GetCamera();

    for (Entity e: reg.GetEntitiesWith<Components::MovementComponent>()) {
        auto* move = reg.Get<Components::MovementComponent>(e);
        if (!move)
            continue;

        float yawRad   = JPH::DegreesToRadians(cam.yaw);
        float forwardX = std::cos(yawRad), forwardZ = std::sin(yawRad);
        float rightX = -std::sin(yawRad), rightZ = std::cos(yawRad);

        float moveX = 0.0f, moveZ = 0.0f;
        if (input.IsKeyDown(KeyCode::W)) {
            moveX += forwardX;
            moveZ += forwardZ;
        }
        if (input.IsKeyDown(KeyCode::S)) {
            moveX -= forwardX;
            moveZ -= forwardZ;
        }
        if (input.IsKeyDown(KeyCode::A)) {
            moveX -= rightX;
            moveZ -= rightZ;
        }
        if (input.IsKeyDown(KeyCode::D)) {
            moveX += rightX;
            moveZ += rightZ;
        }

        float len = std::sqrt(moveX * moveX + moveZ * moveZ);
        if (len > 0.001f) {
            move->inputX = moveX / len;
            move->inputZ = moveZ / len;
        } else {
            move->inputX = 0.0f;
            move->inputZ = 0.0f;
        }

        move->isSprinting = input.IsKeyDown(KeyCode::LShift) && (len > 0.001f);
        if (input.IsKeyDown(KeyCode::Space)) {
            move->jumpRequested = true;
            engine->GetAudioContext().PlayProceduralBeep(660.0f, 0.1f, 0.2f);
        }

        bool isRDown = input.IsKeyDown(KeyCode::R);
        if (isRDown && !g_State.wasRDown) {
            Patch<Components::RagdollComponent>(reg, e, [&](auto& ragdoll) {
                if (!g_State.charParts.empty()) {
                    Entity charRoot = g_State.charParts[0];
                    if (ragdoll.state == RagdollState::Inactive) {
                        ragdoll.state = RagdollState::Limp;
                        reg.Remove<Components::HierarchyComponent>(charRoot);
                        Patch<Components::TransformComponent>(reg, charRoot, [&](auto& rootTrans) {
                            rootTrans.position.SetY(0.0f);
                        });
                        ZHLN::Log("Player collapsed into the blizzard!");
                        engine->GetAudioContext().PlayProceduralBeep(150.0f, 0.25f, 0.3f);
                    } else {
                        ragdoll.state = RagdollState::Inactive;
                        Patch<Components::TransformComponent>(reg, charRoot, [&](auto& rootTrans) {
                            rootTrans.position.SetY(-0.8f);
                        });
                        reg.Add(charRoot, Components::HierarchyComponent {.parent = e});
                        ZHLN::Log("Player stood up in the blizzard!");
                    }
                }
            });
        }
        g_State.wasRDown = isRDown;
    }
}

void BlizzardWindSystem(Engine* engine, float dt) {
    g_State.totalTime += dt;
    auto& reg = engine->GetRegistry();

    Patch<Components::LightComponent>(reg, g_State.campfireLight, [&](auto& fireLight) {
        float gust = 300.0f + 55.0f * std::sin(g_State.totalTime * 14.0f) + 30.0f * std::cos(g_State.totalTime * 28.0f);
        fireLight.intensity = gust;
    });

    Patch<Components::TransformComponent>(reg, g_State.wisp1, [&](auto& w1) {
        float angle = g_State.totalTime * 0.8f;
        w1.position.SetX(-20.0f + std::cos(angle) * 25.0f);
        w1.position.SetY(14.0f + std::sin(g_State.totalTime * 2.1f) * 4.0f);
        w1.position.SetZ(-20.0f + std::sin(angle) * 25.0f);
    });
    Patch<Components::TransformComponent>(reg, g_State.wisp2, [&](auto& w2) {
        float angle = g_State.totalTime * -0.6f + 1.57f;
        w2.position.SetX(15.0f + std::cos(angle) * 32.0f);
        w2.position.SetY(20.0f + std::cos(g_State.totalTime * 1.7f) * 5.0f);
        w2.position.SetZ(-30.0f + std::sin(angle) * 32.0f);
    });
}

void CameraFovSystem(Engine* engine, [[maybe_unused]] float dt) {
    auto& reg = engine->GetRegistry();
    for (Entity pEnt: reg.GetEntitiesWith<Components::MovementComponent>()) {
        auto* move = reg.Get<Components::MovementComponent>(pEnt);
        if (!move) continue;
        if (g_State.playerCameraEnt != NullEntity) {
            if (auto* cam = reg.Get<Components::TargetCameraComponent>(g_State.playerCameraEnt)) {
                if (cam->target == pEnt) {
                    cam->targetFov = move->isSprinting ? 55.0f : 45.0f;
                }
            }
        }
    }
}

void VisualFeedbackSystem(Engine* engine, [[maybe_unused]] float dt) {
    auto& reg = engine->GetRegistry();
    for (Entity pEnt: reg.GetEntitiesWith<GameplayComponents::Combat>()) {
        auto* combat = reg.Get<GameplayComponents::Combat>(pEnt);
        if (!combat) continue;
        if (g_State.playerCameraEnt != NullEntity) {
            if (auto* cam = reg.Get<Components::TargetCameraComponent>(g_State.playerCameraEnt)) {
                if (cam->target == pEnt) {
                    if (combat->hp < 40.0f) {
                        float pulse            = std::sin(g_State.totalTime * 6.0f);
                        cam->vignetteIntensity = 1.4f + 0.35f * pulse;
                        cam->vignettePower     = 2.0f;
                    } else {
                        cam->vignetteIntensity = 1.15f;
                        cam->vignettePower     = 1.6f;
                    }
                }
            }
        }
    }
}

void PlayerAnimationSystem(Engine* engine, [[maybe_unused]] float dt) {
    auto& reg = engine->GetRegistry();
    if (g_State.playerEnt == NullEntity || g_State.charParts.empty())
        return;

    auto* ragdoll = reg.Get<Components::RagdollComponent>(g_State.playerEnt);
    Patch<Components::MovementComponent>(reg, g_State.playerEnt, [&](auto& move) {
        if (ragdoll && (ragdoll->state == RagdollState::Limp || ragdoll->state == RagdollState::KeyframeMotor))
            return;

        std::string targetState = "IDLE";
    if (!move.isGrounded) {
        targetState = (move.currentYVel > 1.0f) ? "JUMP" : "FALL";
    } else if (move.landingTimer > 0.0f) {
        targetState = "LAND";
    } else {
        float velSq = move.inputX * move.inputX + move.inputZ * move.inputZ;
        if (velSq > 0.01f) {
            targetState = move.isSprinting ? "RUN" : "WALK";
        }
    }

    if (targetState != g_State.currentAnimState) {
        g_State.currentAnimState = targetState;

        for (Entity part: g_State.charParts) {
            Patch<Components::AnimatorComponent>(reg, part, [&](auto& anim) {
                int trackIdx = -1;
                if (anim.prefab) {
                    for (size_t i = 0; i < anim.prefab->animations.size(); ++i) {
                        std::string name = anim.prefab->animations[i].name.c_str();
                        std::transform(name.begin(), name.end(), name.begin(), ::toupper);
                        if (name.find(targetState) != std::string::npos) {
                            trackIdx = static_cast<int>(i);
                            break;
                        }
                    }
                }
                if (trackIdx >= 0) {
                    bool  loop  = !(targetState == "JUMP" || targetState == "LAND");
                    float speed = (targetState == "LAND") ? 1.6f : 1.0f;
                    PlayTrack(reg, part, trackIdx, 0.15f, loop, speed);
                }
            });
        }
    }
    });
}

void CheckFallSystem(Engine* engine, [[maybe_unused]] float dt) {
    auto& reg = engine->GetRegistry();
    if (g_State.playerEnt == NullEntity)
        return;

    Patch<Components::PhysicsStateComponent>(reg, g_State.playerEnt, [&](auto& state) {
        if (state.currPosition.GetY() < -10.0f) {
            ZHLN::Log("[Snow Scene] Player fell off the mountain! Respawning...");
            RespawnPlayer(engine);
        }
    });
}

void SummitVictorySystem(Engine* engine, [[maybe_unused]] float dt) {
    auto& reg = engine->GetRegistry();
    if (g_State.playerEnt == NullEntity || g_State.wonGame)
        return;

    Patch<Components::PhysicsStateComponent>(reg, g_State.playerEnt, [&](auto& state) {
        JPH::Vec3 pos = state.currPosition;
        JPH::Vec3 beacon(-50.0f, 35.0f, -50.0f);
        if ((pos - beacon).LengthSq() < (8.0f * 8.0f)) {
            g_State.wonGame = true;
            ZHLN::Log("[Snow Scene] VICTORY! You climbed through the blizzard to the Summit Beacon!");
            engine->GetAudioContext().PlayProceduralBeep(880.0f, 0.25f, 0.3f);
            engine->GetAudioContext().PlayProceduralBeep(1100.0f, 0.25f, 0.3f);
            engine->GetAudioContext().PlayProceduralBeep(1320.0f, 0.45f, 0.3f);
        }
    });
}

} // namespace Game

GAMEPLAY_API ZHLN::GameplayStatus NativeGameplayUpdate(ZHLN::Engine* engine, float dt) {
    if (!engine) {
        return ZHLN::GameplayStatus::Error;
    }

    ZHLN_PROFILE_SCOPE("ECS System: Native Gameplay Update");

    if (!Game::g_State.bridge) {
        Game::g_State.bridge = new ZHLN::ScriptECSBridge(engine->GetRegistry());
        Game::g_State.bridge->RegisterComponentManifest<Game::GameplayComponents>();

        auto playerEnts = engine->GetRegistry().GetEntitiesWith<ZHLN::Components::PlayerTagComponent>();
        if (!playerEnts.empty()) {
            Game::g_State.playerEnt   = playerEnts[0];
            Game::g_State.gameStarted = true;
            ZHLN::Log("[Hot-Reload Success] Gameplay module re-attached to live ECS session!");
        } else {
            ZHLN::MenuConfig cfg;
            cfg.titleLogoPrefab = "TADCLogo.glb";
            cfg.logoPosition    = JPH::RVec3(0.0f, 0.0f, -5.0f);
            cfg.themeMusicPath  = "resources/assets/audio/theme.mp3";
            cfg.cameraPosition  = JPH::Vec3(0.0f, 1.5f, 12.0f);
            cfg.cameraYaw       = -90.0f;
            cfg.cameraPitch     = 0.0f;

            cfg.buttons.push_back(
                {.text = "START GAME",
                 .onClick =
                     [](ZHLN::Engine* eng) {
                         Game::StartGame(eng);
                         Game::g_State.mainMenu.Destroy(eng);
                     },
                 .textX = 55.0f,
                 .textY = 25.0f}
            );

            cfg.buttons.push_back({.text = "QUIT", .onClick = [](ZHLN::Engine* eng) { eng->GetWindow().Close(); }, .textX = 80.0f, .textY = 25.0f});

            Game::g_State.mainMenu.Build(engine, cfg);
            ZHLN::Log("[Snow Scene] Main menu initialized via C++26 module.");
        }
    }

    if (Game::g_State.mainMenu.IsActive()) {
        Game::g_State.mainMenu.Update(engine, dt);
        return ZHLN::GameplayStatus::OK;
    }

    if (Game::g_State.gameStarted) {
        {
            ZHLN_PROFILE_SCOPE("ECS System: Player Input");
            Game::PlayerInputSystem(engine, dt);
        }
        {
            ZHLN_PROFILE_SCOPE("ECS System: Blizzard Wind");
            Game::BlizzardWindSystem(engine, dt);
        }
        {
            ZHLN_PROFILE_SCOPE("ECS System: Camera FOV");
            Game::CameraFovSystem(engine, dt);
        }
        {
            ZHLN_PROFILE_SCOPE("ECS System: Visual Feedback");
            Game::VisualFeedbackSystem(engine, dt);
        }
        {
            ZHLN_PROFILE_SCOPE("ECS System: Player Animation");
            Game::PlayerAnimationSystem(engine, dt);
        }
        {
            ZHLN_PROFILE_SCOPE("ECS System: Check Fall");
            Game::CheckFallSystem(engine, dt);
        }
        {
            ZHLN_PROFILE_SCOPE("ECS System: Summit Victory");
            Game::SummitVictorySystem(engine, dt);
        }
        {
            ZHLN_PROFILE_SCOPE("ECS System: Lightning & Explosions");
            Game::LightningClickStrikeSystem(engine, dt);
        }
    }
    return ZHLN::GameplayStatus::OK;
}
