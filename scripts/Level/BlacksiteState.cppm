// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module;
#include <Jolt/Jolt.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <array>
#include <string>
#include <vector>

export module ZHLN.BlacksiteState;

import ZHLN.MainMenu;
import ZHLN.Weapons;
import ZHLN.CombatFX;
import ZHLN.Pickups;

export namespace ZHLN::BlacksiteState {

struct KillFeedItem {
    uint32_t    id   = 0;
    std::string text = "";
    bool        head = false;
};

struct SceneState {
    MainMenu mainMenu;
    bool     gameStarted = false;

    Entity playerEnt    = NullEntity;
    Entity weaponEntity = NullEntity;
    Entity floorPlane   = NullEntity;
    Entity sunLight     = NullEntity;

    MaterialID concreteMat = 0;
    MaterialID crateMat    = 0;
    MaterialID barrierMat  = 0;
    MaterialID sandbagMat  = 0;
    MaterialID metalMat    = 0;
    MaterialID enemyMat    = 0;

    Material tracerMat;
    Material particleMat;

    Entity                hudVitalsBg      = NullEntity;
    Entity                hudVitalsBar     = NullEntity;
    Entity                hudAmmoText      = NullEntity;
    Entity                hudCrosshair     = NullEntity;
    Entity                hudWaveText      = NullEntity;
    std::array<Entity, 5> hudKillFeedTexts = {NullEntity, NullEntity, NullEntity, NullEntity, NullEntity};

    std::vector<Entity>                     worldEntities;
    std::vector<Entity>                     enemies;
    std::vector<Pickups::WeaponPickup>      pickups;
    std::vector<CombatFX::VisualParticle>   particles;
    std::vector<CombatFX::BulletTracer>     tracers;
    std::vector<CombatFX::KineticShockwave> shockwaves;
    std::vector<KillFeedItem>               killFeed;

    uint32_t feedCounter = 0;
    uint32_t score       = 0;
    uint32_t kills       = 0;
    uint32_t headshots   = 0;
    uint32_t wave        = 1;
    float    waveTimer   = 0.0f;

    bool     hordeMode   = false;
    uint32_t hordeTarget = 40;
};

inline SceneState g_SceneState;

inline SceneState& GetSceneState() noexcept {
    return g_SceneState;
}

void PushKillFeed(const std::string& text, bool headshot) {
    g_SceneState.killFeed.insert(g_SceneState.killFeed.begin(), KillFeedItem {.id = ++g_SceneState.feedCounter, .text = text, .head = headshot});
    if (g_SceneState.killFeed.size() > 5) {
        g_SceneState.killFeed.pop_back();
    }
}

void AddBox(Engine* engine, JPH::Vec3 pos, JPH::Vec3 size, MaterialID mat, bool solid = true) {
    auto& reg = engine->GetRegistry();
    auto& pc  = engine->GetPhysicsContext();

    Entity e = reg.Create();
    reg.Add(e, Components::TransformComponent {.position = pos, .rotation = JPH::Quat::sIdentity(), .scale = size});
    reg.Add(e, Components::NameComponent {.name = String64("StaticCover")});

    AssetID unitBoxAsset = HashAssetID("unit_box");
    reg.Add(e, Components::MeshComponent {.meshAsset = unitBoxAsset, .materialAsset = mat, .cullRadius = size.Length() * 0.5f});
    reg.Add(e, Components::PBRComponent {.roughness = 0.86f, .metallic = 0.02f});

    if (solid) {
        auto boxShape = Physics::GetOrCreateShape(pc, Physics::ShapeType::Box, size.GetX() * 0.5f, size.GetY() * 0.5f, size.GetZ() * 0.5f);
        reg.Add(e, Components::PhysicsComponent {Physics::CreateRigidBody(pc, boxShape, JPH::RVec3(pos), JPH::Quat::sIdentity(), JPH::EMotionType::Static, 0)});
    }

    g_SceneState.worldEntities.push_back(e);
}

} // namespace ZHLN::BlacksiteState
