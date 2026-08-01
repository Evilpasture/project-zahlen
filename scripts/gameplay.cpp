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
#include "Zahlen/ScriptECSBridge.hpp"
#include "Zahlen/Window.hpp"
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>

// C++ Standard Library & Engine Modules
import std;
import ZHLN.MainMenu;

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
    ScriptECSBridge* bridge = nullptr;
    MainMenu         mainMenu;
    bool             gameStarted = false;
    bool             wonGame     = false;
    bool             wasRDown    = false;
    float            totalTime   = 0.0f;

    Entity playerEnt     = NullEntity;
    Entity snowTerrain   = NullEntity;
    Entity campfireLight = NullEntity;
    Entity summitLight   = NullEntity;
    Entity wisp1         = NullEntity;
    Entity wisp2         = NullEntity;

    std::vector<Entity> charParts;
    std::string         currentAnimState = "IDLE";
};

static SnowSceneState g_State;

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

} // namespace TerrainGen

void PlayTrack(ECS::Registry& reg, Entity ent, int trackIdx, float blend = 0.15f, bool loop = true, float speed = 1.0f) {
    if (auto* anim = reg.Get<Components::AnimatorComponent>(ent)) {
        if (anim->currentTrackIdx != trackIdx) {
            anim->prevTrackIdx         = anim->currentTrackIdx;
            anim->prevTrackTime        = anim->currentTrackTime;
            anim->currentTrackIdx      = trackIdx;
            anim->currentTrackTime     = 0.0f;
            anim->currentPlaybackSpeed = speed;
            anim->currentLoop          = loop;
            anim->blendFactor          = 0.0f;
            anim->blendDuration        = blend;
            anim->isFinished           = false;
        }
    }
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

    g_State.playerEnt = reg.Create();
    reg.Add(g_State.playerEnt, Components::PlayerTagComponent {});
    reg.Add(g_State.playerEnt, Components::TransformComponent {.position = JPH::Vec3(0.0f, 3.0f, 0.0f)});
    reg.Add(g_State.playerEnt, Components::MovementComponent {});
    reg.Add(g_State.playerEnt, Components::InputComponent {});
    reg.Add(g_State.playerEnt, Components::PhysicsComponent {Physics::CreateCharacter(pc, JPH::RVec3(0.0f, 3.0f, 0.0f))});
    reg.Add(g_State.playerEnt, Components::PhysicsStateComponent {.currPosition = JPH::Vec3(0.0f, 3.0f, 0.0f), .prevPosition = JPH::Vec3(0.0f, 3.0f, 0.0f)});
    reg.Add(g_State.playerEnt, GameplayComponents::Combat {.hp = 100.0f, .maxHp = 100.0f});

    auto camEnts = reg.GetEntitiesWith<Components::MainCameraTagComponent>();
    if (!camEnts.empty()) {
        Entity camEnt    = camEnts[0];
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
        if (auto* rootTrans = reg.Get<Components::TransformComponent>(charRoot)) {
            rootTrans->position = JPH::Vec3(0.0f, -0.8f, 0.0f);
        }
        reg.Add(charRoot, Components::HierarchyComponent {.parent = g_State.playerEnt});
        CreativeWorksFactory::SetupPlayerRagdoll(*engine, g_State.playerEnt, g_State.charParts);
    }

    ZHLN::Log("[Snow Scene] Player successfully respawned!");
}

void StartGame(Engine* engine) {
    auto& reg = engine->GetRegistry();
    auto& pc  = engine->GetPhysicsContext();

    ZHLN::Log("[Snow Scene] Generating Volumetric Nighttime Environment...");
    g_State.wonGame   = false;
    g_State.totalTime = 0.0f;

    auto settingsEntities = reg.GetEntitiesWith<Components::GlobalSettingsTagComponent>();
    if (!settingsEntities.empty()) {
        if (auto* pp = reg.Get<Components::PostProcessSettingsComponent>(settingsEntities[0])) {
            pp->giMode            = 2;
            pp->aoRadius          = 1.4f;
            pp->aoBias            = 0.02f;
            pp->aoPower           = 2.2f;
            pp->giIntensity       = 2.2f;
            pp->giSamples         = 24;
            pp->useLocalProbe     = 0;
            pp->vignetteIntensity = 1.25f;
            pp->vignettePower     = 1.8f;
            pp->enableSSR         = 0;
            pp->enableRTR         = 1;
            pp->ambientExposure   = 4.5f;
        }
    }

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

    CreativeWorksFactory::SpawnParams p;
    p.position = JPH::RVec3(0.0f, 80.0f, -500.0f);
    p.rotation = JPH::Quat(0.35f, 0.25f, 0.1f, 0.9f).Normalized();
    p.scale    = JPH::Vec3(5.0f, 5.0f, 5.0f);

    CreativeWorksFactory::InstantiatePrefab(*engine, "murderdrones/Copper9_Celestials.glb", p);

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

    g_State.campfireLight = reg.Create();
    reg.Add(g_State.campfireLight, Components::TransformComponent {.position = JPH::Vec3(4.0f, 3.5f, 4.0f)});
    reg.Add(
        g_State.campfireLight, Components::LightComponent {
                                   .type        = LightType::Point,
                                   .color       = JPH::Vec3(1.0f, 0.45f, 0.08f),
                                   .intensity   = 120.0f,
                                   .radius      = 0.6f,
                                   .direction   = JPH::Vec3(0, -1, 0),
                                   .range       = 25.0f,
                                   .points      = JPH::Mat44::sIdentity(),
                                   .twoSided    = 0,
                                   .shadowLayer = -1
                               }
    );

    g_State.wisp1 = reg.Create();
    reg.Add(g_State.wisp1, Components::TransformComponent {.position = JPH::Vec3(-15.0f, 12.0f, -15.0f)});
    reg.Add(
        g_State.wisp1, Components::LightComponent {
                           .type        = LightType::Point,
                           .color       = JPH::Vec3(0.1f, 0.75f, 1.0f),
                           .intensity   = 60.0f,
                           .radius      = 0.4f,
                           .direction   = JPH::Vec3(0, -1, 0),
                           .range       = 20.0f,
                           .points      = JPH::Mat44::sIdentity(),
                           .twoSided    = 0,
                           .shadowLayer = -1
                       }
    );

    g_State.wisp2 = reg.Create();
    reg.Add(g_State.wisp2, Components::TransformComponent {.position = JPH::Vec3(20.0f, 18.0f, -30.0f)});
    reg.Add(
        g_State.wisp2, Components::LightComponent {
                           .type        = LightType::Point,
                           .color       = JPH::Vec3(0.3f, 0.6f, 1.0f),
                           .intensity   = 60.0f,
                           .radius      = 0.4f,
                           .direction   = JPH::Vec3(0, -1, 0),
                           .range       = 20.0f,
                           .points      = JPH::Mat44::sIdentity(),
                           .twoSided    = 0,
                           .shadowLayer = -1
                       }
    );

    g_State.summitLight = reg.Create();
    reg.Add(g_State.summitLight, Components::TransformComponent {.position = JPH::Vec3(-50.0f, 38.0f, -50.0f)});
    reg.Add(
        g_State.summitLight, Components::LightComponent {
                                 .type        = LightType::Point,
                                 .color       = JPH::Vec3(1.0f, 0.85f, 0.2f),
                                 .intensity   = 400.0f,
                                 .radius      = 0.8f,
                                 .direction   = JPH::Vec3(0, -1, 0),
                                 .range       = 50.0f,
                                 .points      = JPH::Mat44::sIdentity(),
                                 .twoSided    = 0,
                                 .shadowLayer = -1
                             }
    );

    g_State.gameStarted = true;
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
            if (auto* ragdoll = reg.Get<Components::RagdollComponent>(e)) {
                if (!g_State.charParts.empty()) {
                    Entity charRoot = g_State.charParts[0];
                    if (ragdoll->state == RagdollState::Inactive) {
                        ragdoll->state = RagdollState::Limp;
                        reg.Remove<Components::HierarchyComponent>(charRoot);
                        if (auto* rootTrans = reg.Get<Components::TransformComponent>(charRoot)) {
                            rootTrans->position.SetY(0.0f);
                        }
                        ZHLN::Log("Player collapsed into the blizzard!");
                        engine->GetAudioContext().PlayProceduralBeep(150.0f, 0.25f, 0.3f);
                    } else {
                        ragdoll->state = RagdollState::Inactive;
                        if (auto* rootTrans = reg.Get<Components::TransformComponent>(charRoot)) {
                            rootTrans->position.SetY(-0.8f);
                        }
                        reg.Add(charRoot, Components::HierarchyComponent {.parent = e});
                        ZHLN::Log("Player stood up in the blizzard!");
                    }
                }
            }
        }
        g_State.wasRDown = isRDown;
    }
}

void BlizzardWindSystem(Engine* engine, float dt) {
    g_State.totalTime += dt;
    auto& reg = engine->GetRegistry();

    if (auto* fireLight = reg.Get<Components::LightComponent>(g_State.campfireLight)) {
        float gust           = 300.0f + 55.0f * std::sin(g_State.totalTime * 14.0f) + 30.0f * std::cos(g_State.totalTime * 28.0f);
        fireLight->intensity = gust;
    }

    if (auto* w1 = reg.Get<Components::TransformComponent>(g_State.wisp1)) {
        float angle = g_State.totalTime * 0.8f;
        w1->position.SetX(-20.0f + std::cos(angle) * 25.0f);
        w1->position.SetY(14.0f + std::sin(g_State.totalTime * 2.1f) * 4.0f);
        w1->position.SetZ(-20.0f + std::sin(angle) * 25.0f);
    }
    if (auto* w2 = reg.Get<Components::TransformComponent>(g_State.wisp2)) {
        float angle = g_State.totalTime * -0.6f + 1.57f;
        w2->position.SetX(15.0f + std::cos(angle) * 32.0f);
        w2->position.SetY(20.0f + std::cos(g_State.totalTime * 1.7f) * 5.0f);
        w2->position.SetZ(-30.0f + std::sin(angle) * 32.0f);
    }
}

void CameraFovSystem(Engine* engine, [[maybe_unused]] float dt) {
    auto& reg = engine->GetRegistry();
    for (Entity pEnt: reg.GetEntitiesWith<Components::MovementComponent>()) {
        auto* move = reg.Get<Components::MovementComponent>(pEnt);
        for (Entity cEnt: reg.GetEntitiesWith<Components::TargetCameraComponent>()) {
            auto* cam = reg.Get<Components::TargetCameraComponent>(cEnt);
            if (cam && cam->target == pEnt) {
                cam->targetFov = move->isSprinting ? 55.0f : 45.0f;
            }
        }
    }
}

void VisualFeedbackSystem(Engine* engine, [[maybe_unused]] float dt) {
    auto& reg = engine->GetRegistry();
    for (Entity pEnt: reg.GetEntitiesWith<GameplayComponents::Combat>()) {
        auto* combat = reg.Get<GameplayComponents::Combat>(pEnt);
        for (Entity cEnt: reg.GetEntitiesWith<Components::TargetCameraComponent>()) {
            auto* cam = reg.Get<Components::TargetCameraComponent>(cEnt);
            if (cam && cam->target == pEnt) {
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

void PlayerAnimationSystem(Engine* engine, [[maybe_unused]] float dt) {
    auto& reg = engine->GetRegistry();
    if (g_State.playerEnt == NullEntity || g_State.charParts.empty())
        return;

    auto* move    = reg.Get<Components::MovementComponent>(g_State.playerEnt);
    auto* ragdoll = reg.Get<Components::RagdollComponent>(g_State.playerEnt);
    if (!move)
        return;
    if (ragdoll && (ragdoll->state == RagdollState::Limp || ragdoll->state == RagdollState::KeyframeMotor))
        return;

    std::string targetState = "IDLE";
    if (!move->isGrounded) {
        targetState = (move->currentYVel > 1.0f) ? "JUMP" : "FALL";
    } else if (move->landingTimer > 0.0f) {
        targetState = "LAND";
    } else {
        float velSq = move->inputX * move->inputX + move->inputZ * move->inputZ;
        if (velSq > 0.01f) {
            targetState = move->isSprinting ? "RUN" : "WALK";
        }
    }

    if (targetState != g_State.currentAnimState) {
        g_State.currentAnimState = targetState;

        for (Entity part: g_State.charParts) {
            if (auto* anim = reg.Get<Components::AnimatorComponent>(part)) {
                int trackIdx = -1;
                if (anim->prefab) {
                    for (size_t i = 0; i < anim->prefab->animations.size(); ++i) {
                        std::string name = anim->prefab->animations[i].name.c_str();
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
            }
        }
    }
}

void CheckFallSystem(Engine* engine, [[maybe_unused]] float dt) {
    auto& reg = engine->GetRegistry();
    if (g_State.playerEnt == NullEntity)
        return;

    if (auto* state = reg.Get<Components::PhysicsStateComponent>(g_State.playerEnt)) {
        if (state->currPosition.GetY() < -10.0f) {
            ZHLN::Log("[Snow Scene] Player fell off the mountain! Respawning...");
            RespawnPlayer(engine);
        }
    }
}

void SummitVictorySystem(Engine* engine, [[maybe_unused]] float dt) {
    auto& reg = engine->GetRegistry();
    if (g_State.playerEnt == NullEntity || g_State.wonGame)
        return;

    if (auto* state = reg.Get<Components::PhysicsStateComponent>(g_State.playerEnt)) {
        JPH::Vec3 pos = state->currPosition;
        JPH::Vec3 beacon(-50.0f, 35.0f, -50.0f);
        if ((pos - beacon).LengthSq() < (8.0f * 8.0f)) {
            g_State.wonGame = true;
            ZHLN::Log("[Snow Scene] VICTORY! You climbed through the blizzard to the Summit Beacon!");
            engine->GetAudioContext().PlayProceduralBeep(880.0f, 0.25f, 0.3f);
            engine->GetAudioContext().PlayProceduralBeep(1100.0f, 0.25f, 0.3f);
            engine->GetAudioContext().PlayProceduralBeep(1320.0f, 0.45f, 0.3f);
        }
    }
}

} // namespace Game

GAMEPLAY_API void NativeGameplayUpdate(ZHLN::Engine* engine, float dt) {
    if (!engine)
        return;

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
        return;
    }

    if (Game::g_State.gameStarted) {
        Game::PlayerInputSystem(engine, dt);
        Game::BlizzardWindSystem(engine, dt);
        Game::CameraFovSystem(engine, dt);
        Game::VisualFeedbackSystem(engine, dt);
        Game::PlayerAnimationSystem(engine, dt);
        Game::CheckFallSystem(engine, dt);
        Game::SummitVictorySystem(engine, dt);
    }
}
