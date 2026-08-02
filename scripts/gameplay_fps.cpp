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
#include "Zahlen/Render.hpp"
#include "Zahlen/Window.hpp"
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>

// Zahlen C++26 Module Imports
import ZHLN.MathUtils;
import ZHLN.Rig;
import ZHLN.FPS;
import ZHLN.Ragdoll;
import ZHLN.Animator;
import ZHLN.Actor;
import ZHLN.MainMenu;
import ZHLN.Soldier;

import std;

#if defined(_WIN32)
#define GAMEPLAY_API extern "C" __declspec(dllexport)
#else
#define GAMEPLAY_API extern "C" [[gnu::visibility("default")]]
#endif

namespace Game {

// Single variable controlling global FPS camera eye height (Y = 0.80 + 0.55 = 1.35m)
constexpr float PLAYER_EYE_OFFSET_Y = 0.55f;

using namespace ZHLN;

// ============================================================================
// CONSTANTS & CONFIGURATIONS
// ============================================================================
constexpr float HIP_FOV        = 78.0f;
constexpr float ADS_FOV        = 42.0f;
constexpr float SIGHT_HEIGHT   = 0.079f;
constexpr float SIGHT_Z        = 0.125f;
constexpr float ADS_SIGHT_Z    = -0.285f;
constexpr float BLAST_RANGE    = 17.0f;
constexpr float BLAST_FORCE    = 26.0f;
constexpr float BLAST_COOLDOWN = 6.0f;
constexpr float BODY_PEN_COST  = 0.55f;

enum class WeaponId : uint8_t { Rifle = 0, Shotgun = 1, Minigun = 2, Count = 3 };

struct WeaponDef {
    WeaponId    id               = WeaponId::Rifle;
    std::string name             = "MK-18";
    std::string caliber          = "5.56";
    uint32_t    pellets          = 1;
    float       damage           = 30.0f;
    int32_t     magSize          = 30;
    int32_t     startReserve     = 210;
    float       fireRate         = 0.086f;
    float       reloadTime       = 2.1f;
    float       shellReload      = 0.0f;
    float       baseSpread       = 0.005f;
    float       patternSpread    = 0.0f;
    float       adsTighten       = 0.8f;
    float       falloffStart     = 45.0f;
    float       falloffEnd       = 110.0f;
    float       falloffMin       = 0.55f;
    float       range            = 220.0f;
    float       recoilPitch      = 0.55f;
    float       recoilYaw        = 0.5f;
    float       kick             = 1.9f;
    float       adsFov           = 42.0f;
    uint32_t    tracerColor      = 0xfff0b0;
    float       flashScale       = 0.75f;
    float       spinUp           = 0.0f;
    float       moveScale        = 1.0f;
    bool        godOnly          = false;
    bool        noAds            = false;
    float       penetration      = 1.35f;
    float       bodyPenRetain    = 0.55f;
    float       surfacePenRetain = 0.72f;
    uint32_t    maxBodyPierces   = 2;
};

static const std::array<WeaponDef, static_cast<size_t>(WeaponId::Count)> WEAPON_DEFS = {
    {{.id               = WeaponId::Rifle,
      .name             = "MK-18",
      .caliber          = "5.56",
      .pellets          = 1,
      .damage           = 30.0f,
      .magSize          = 30,
      .startReserve     = 210,
      .fireRate         = 0.086f,
      .reloadTime       = 2.1f,
      .shellReload      = 0.0f,
      .baseSpread       = 0.005f,
      .patternSpread    = 0.0f,
      .adsTighten       = 0.8f,
      .falloffStart     = 45.0f,
      .falloffEnd       = 110.0f,
      .falloffMin       = 0.55f,
      .range            = 220.0f,
      .recoilPitch      = 0.55f,
      .recoilYaw        = 0.5f,
      .kick             = 1.9f,
      .adsFov           = 42.0f,
      .tracerColor      = 0xfff0b0,
      .flashScale       = 0.75f,
      .spinUp           = 0.0f,
      .moveScale        = 1.0f,
      .godOnly          = false,
      .noAds            = false,
      .penetration      = 1.35f,
      .bodyPenRetain    = 0.55f,
      .surfacePenRetain = 0.72f,
      .maxBodyPierces   = 2},
     {.id               = WeaponId::Shotgun,
      .name             = "M870 BREACHER",
      .caliber          = "12GA",
      .pellets          = 9,
      .damage           = 16.0f,
      .magSize          = 7,
      .startReserve     = 70,
      .fireRate         = 0.62f,
      .reloadTime       = 0.0f,
      .shellReload      = 0.45f,
      .baseSpread       = 0.012f,
      .patternSpread    = 0.05f,
      .adsTighten       = 0.7f,
      .falloffStart     = 11.0f,
      .falloffEnd       = 40.0f,
      .falloffMin       = 0.2f,
      .range            = 60.0f,
      .recoilPitch      = 2.1f,
      .recoilYaw        = 0.9f,
      .kick             = 5.5f,
      .adsFov           = 52.0f,
      .tracerColor      = 0xffc070,
      .flashScale       = 1.35f,
      .spinUp           = 0.0f,
      .moveScale        = 1.0f,
      .godOnly          = false,
      .noAds            = false,
      .penetration      = 0.0f,
      .bodyPenRetain    = 0.0f,
      .surfacePenRetain = 0.0f,
      .maxBodyPierces   = 0},
     {.id               = WeaponId::Minigun,
      .name             = "M134 VULCAN",
      .caliber          = "7.62 BELT",
      .pellets          = 1,
      .damage           = 17.0f,
      .magSize          = 600,
      .startReserve     = 2400,
      .fireRate         = 0.028f,
      .reloadTime       = 5.5f,
      .shellReload      = 0.0f,
      .baseSpread       = 0.03f,
      .patternSpread    = 0.0f,
      .adsTighten       = 0.0f,
      .falloffStart     = 40.0f,
      .falloffEnd       = 120.0f,
      .falloffMin       = 0.5f,
      .range            = 190.0f,
      .recoilPitch      = 0.11f,
      .recoilYaw        = 0.16f,
      .kick             = 0.45f,
      .adsFov           = 70.0f,
      .tracerColor      = 0xffdd66,
      .flashScale       = 1.15f,
      .spinUp           = 0.62f,
      .moveScale        = 0.55f,
      .godOnly          = true,
      .noAds            = true,
      .penetration      = 3.6f,
      .bodyPenRetain    = 0.72f,
      .surfacePenRetain = 0.85f,
      .maxBodyPierces   = 4}}
};

inline const WeaponDef& GetWeaponDef(WeaponId id) {
    return WEAPON_DEFS[static_cast<size_t>(id)];
}

inline float FalloffAt(const WeaponDef& def, float dist) {
    if (dist <= def.falloffStart)
        return 1.0f;
    if (dist >= def.falloffEnd)
        return def.falloffMin;
    float t = (dist - def.falloffStart) / (def.falloffEnd - def.falloffStart);
    return 1.0f + (def.falloffMin - 1.0f) * t;
}

// ============================================================================
// PROCEDURAL AUDIO SYNTHESIZERS
// ============================================================================

inline void PlaySound_Shoot(Engine* engine, float dist) {
    float atten = std::max(0.08f, 1.0f - dist / 55.0f);
    auto& audio = engine->GetAudioContext();
    audio.PlayProceduralBeep(180.0f, 0.16f, 0.35f * atten);
    audio.PlayProceduralBeep(1400.0f, 0.14f, 0.50f * atten);
}

inline void PlaySound_Shotgun(Engine* engine, float dist) {
    float atten = std::max(0.08f, 1.0f - dist / 65.0f);
    auto& audio = engine->GetAudioContext();
    audio.PlayProceduralBeep(190.0f, 0.30f, 0.60f * atten);
    audio.PlayProceduralBeep(900.0f, 0.25f, 0.75f * atten);
}

inline void PlaySound_Pump(Engine* engine, float dist) {
    if (dist > 30.0f)
        return;
    float atten = std::max(0.1f, 1.0f - dist / 28.0f);
    engine->GetAudioContext().PlayProceduralBeep(2400.0f, 0.05f, 0.30f * atten);
}

inline void PlaySound_Minigun(Engine* engine) {
    auto& audio = engine->GetAudioContext();
    audio.PlayProceduralBeep(1900.0f, 0.05f, 0.25f);
    audio.PlayProceduralBeep(150.0f, 0.06f, 0.30f);
}

inline void PlaySound_Impact(Engine* engine, float dist) {
    float atten = std::max(0.05f, 1.0f - dist / 45.0f);
    engine->GetAudioContext().PlayProceduralBeep(2600.0f, 0.07f, 0.25f * atten);
}

inline void PlaySound_Flesh(Engine* engine, float dist) {
    float atten = std::max(0.05f, 1.0f - dist / 45.0f);
    engine->GetAudioContext().PlayProceduralBeep(420.0f, 0.10f, 0.40f * atten);
}

inline void PlaySound_Hitmark(Engine* engine) {
    engine->GetAudioContext().PlayProceduralBeep(1500.0f, 0.05f, 0.14f);
}

inline void PlaySound_Kill(Engine* engine) {
    engine->GetAudioContext().PlayProceduralBeep(700.0f, 0.18f, 0.18f);
}

inline void PlaySound_Hurt(Engine* engine) {
    auto& audio = engine->GetAudioContext();
    audio.PlayProceduralBeep(120.0f, 0.30f, 0.30f);
    audio.PlayProceduralBeep(260.0f, 0.25f, 0.50f);
}

inline void PlaySound_Step(Engine* engine) {
    engine->GetAudioContext().PlayProceduralBeep(900.0f, 0.06f, 0.09f);
}

inline void PlaySound_Empty(Engine* engine) {
    engine->GetAudioContext().PlayProceduralBeep(4200.0f, 0.03f, 0.20f);
}

inline void PlaySound_Reload(Engine* engine) {
    auto& audio = engine->GetAudioContext();
    audio.PlayProceduralBeep(3000.0f, 0.06f, 0.30f);
    audio.PlayProceduralBeep(1800.0f, 0.08f, 0.30f);
    audio.PlayProceduralBeep(2600.0f, 0.06f, 0.35f);
}

inline void PlaySound_Blast(Engine* engine) {
    auto& audio = engine->GetAudioContext();
    audio.PlayProceduralBeep(150.0f, 0.55f, 0.60f);
    audio.PlayProceduralBeep(320.0f, 0.40f, 0.35f);
}

inline void PlaySound_Pickup(Engine* engine) {
    auto& audio = engine->GetAudioContext();
    audio.PlayProceduralBeep(1400.0f, 0.09f, 0.22f);
    audio.PlayProceduralBeep(2000.0f, 0.07f, 0.16f);
}

inline void PlaySound_Pierce(Engine* engine) {
    engine->GetAudioContext().PlayProceduralBeep(3400.0f, 0.05f, 0.18f);
}

// ============================================================================
// PARTICLE & FX STRUCTURES
// ============================================================================

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

struct WeaponPickup {
    Entity    entity   = NullEntity;
    WeaponId  weaponId = WeaponId::Rifle;
    int32_t   ammo     = 30;
    JPH::Vec3 position = JPH::Vec3::sZero();
    JPH::Vec3 velocity = JPH::Vec3::sZero();
    JPH::Vec3 spin     = JPH::Vec3::sZero();
    float     age      = 0.0f;
    float     ttl      = 45.0f;
    bool      rested   = false;
    bool      claimed  = false;
};

struct KillFeedItem {
    uint32_t    id   = 0;
    std::string text = "";
    bool        head = false;
};

struct AnimInput {
    float speed;
    float crouch;
    float aimYaw;
    float aimPitch;
    float aiming;
};

// ============================================================================
// PROCEDURAL WEAPON STANCE & ANIMATOR
// ============================================================================

class SoldierAnimator {
  public:
    float phase     = 0.0f;
    float breath    = 0.0f;
    float recoil    = 0.0f;
    float recoilVel = 0.0f;
    float reloadT   = -1.0f;
    float lowReady  = 1.0f;

    JPH::Vec3 weaponPos   = JPH::Vec3::sZero();
    JPH::Quat weaponQuat  = JPH::Quat::sIdentity();
    JPH::Vec3 muzzleWorld = JPH::Vec3::sZero();
    JPH::Vec3 aimDir      = JPH::Vec3::sAxisZ();

    void Fire(float power = 1.0f) {
        recoilVel += 3.6f * power;
    }
    void StartReload() {
        reloadT = 0.0f;
    }

    void Update(float dt, const AnimInput& in) {
        recoilVel += (-260.0f * recoil - 22.0f * recoilVel) * dt;
        recoil += recoilVel * dt;
        breath += dt;
        if (reloadT >= 0.0f) {
            reloadT += dt / 2.2f;
            if (reloadT >= 1.0f)
                reloadT = -1.0f;
        }
        lowReady = MathUtils::Damp(lowReady, 1.0f - in.aiming, 6.0f, dt);

        float stride = in.speed > 3.2f ? 1.85f : 1.25f;
        phase += (in.speed / stride) * dt;

        float pitch     = MathUtils::Clamp(in.aimPitch, -1.1f, 0.9f);
        weaponQuat      = MathUtils::EulerYXZ(pitch + lowReady * 0.62f, in.aimYaw, 0.0f);
        JPH::Quat kickQ = MathUtils::EulerYXZ(-recoil * 0.55f, recoil * 0.12f, recoil * 0.2f);
        weaponQuat      = (weaponQuat * kickQ).Normalized();

        JPH::Vec3 offset(-0.085f - lowReady * 0.02f, 0.22f - lowReady * 0.14f, 0.27f - lowReady * 0.02f - recoil * 0.05f);
        weaponPos = JPH::Vec3(0.0f, 1.4f, 0.0f) + weaponQuat * offset;

        muzzleWorld = weaponPos + weaponQuat * JPH::Vec3(0, 0.012f, 0.66f);
        aimDir      = weaponQuat * JPH::Vec3(0, 0, 1);
    }
};

// ============================================================================
// ECS COMPONENT ABSTRACTIONS
// ============================================================================

struct GameplayComponents {
    struct PlayerController {
        FPS::Spring3D     weaponSpring;
        FPS::Spring3D     swaySpring;
        FPS::Spring1D     pitchRecoil;
        FPS::Spring1D     yawRecoil;
        FPS::Spring1D     kickSpring;
        FPS::BobEvaluator bobber;
        FPS::SwaySolver   sway;

        float baseYaw    = -90.0f;
        float basePitch  = 0.0f;
        float totalTime  = 0.0f;
        float bobPhase   = 0.0f;
        float bobAmt     = 0.0f;
        float landDip    = 0.0f;
        float landVel    = 0.0f;
        float stepPhase  = 0.0f;
        float lastHeight = 1.5f;

        float health    = 100.0f;
        float maxHealth = 100.0f;
        bool  alive     = true;

        WeaponId currentWeapon = WeaponId::Rifle;
        WeaponId pendingWeapon = WeaponId::Rifle;
        float    swapT         = 0.0f;

        struct AmmoState {
            int32_t mag     = 30;
            int32_t reserve = 210;
        };
        std::array<AmmoState, static_cast<size_t>(WeaponId::Count)> ammo = {{
            {.mag = 30, .reserve = 210},  // Rifle
            {.mag = 7, .reserve = 70},    // Shotgun
            {.mag = 600, .reserve = 2400} // Minigun
        }};

        float   reloading    = 0.0f;
        int32_t shellsToLoad = 0;
        float   shellTimer   = 0.0f;
        float   fireCd       = 0.0f;
        int32_t shotsFired   = 0;
        float   ads          = 0.0f;
        float   spin         = 0.0f;
        float   barrelAngle  = 0.0f;

        float blastCd   = 0.0f;
        float blastTime = -99.0f;

        bool godMode      = false;
        bool infiniteAmmo = false;
        bool adsToggle    = false;

        float hurtFlash        = 0.0f;
        float hurtDir          = 0.0f;
        float hitMarkerTime    = -99.0f;
        float headMarkerTime   = -99.0f;
        float pierceMarkerTime = -99.0f;

        float       pickupFlash     = 0.0f;
        std::string pickupFlashText = "";
    };

    struct EnemyController {
        Actor::StandardActor behavior;
        SoldierAnimator      anim;
        std::vector<Entity>  limbEntities;
        Entity               weaponEntity = NullEntity;
        WeaponId             weaponId     = WeaponId::Rifle;
        float                phase        = 0.0f;
        float                breath       = 0.0f;
        float                recoil       = 0.0f;
        float                recoilVel    = 0.0f;
        float                reloadT      = -1.0f;
        float                lowReady     = 1.0f;
    };
};

struct BlacksiteSceneState {
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

    Entity hudVitalsBg  = NullEntity;
    Entity hudVitalsBar = NullEntity;
    Entity hudAmmoText  = NullEntity;
    Entity hudCrosshair = NullEntity;
    Entity hudWaveText  = NullEntity;
    Entity hudScoreText = NullEntity;
    Entity hudFeedText  = NullEntity;

    std::vector<Entity>           worldEntities;
    std::vector<Entity>           enemies;
    std::vector<WeaponPickup>     pickups;
    std::vector<VisualParticle>   particles;
    std::vector<BulletTracer>     tracers;
    std::vector<KineticShockwave> shockwaves;
    std::vector<KillFeedItem>     killFeed;

    uint32_t feedCounter = 0;
    uint32_t score       = 0;
    uint32_t kills       = 0;
    uint32_t headshots   = 0;
    uint32_t wave        = 1;
    float    waveTimer   = 0.0f;

    bool     hordeMode   = false;
    uint32_t hordeTarget = 40;
};

static BlacksiteSceneState g_State;

// ============================================================================
// LEVEL DESIGN & PROCEDURAL COLLIDER POPULATION
// ============================================================================

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

    g_State.worldEntities.push_back(e);
}

// ============================================================================
// SINGLE-MESH PROCEDURAL WEAPON GENERATOR
// ============================================================================

struct ProceduralWeaponMeshBuilder {
    std::vector<VertexPosition>   positions;
    std::vector<VertexAttributes> attributes;
    std::vector<uint32_t>         indices;

    void AddBox(JPH::Vec3Arg center, JPH::Vec3Arg size, JPH::Vec4Arg color, JPH::QuatArg rot = JPH::Quat::sIdentity()) {
        JPH::Vec3 hs = size * 0.5f;

        struct Face {
            JPH::Vec3 n, u, v;
        };
        std::array<Face, 6> faces = {{
            {JPH::Vec3(0, 0, 1), JPH::Vec3(1, 0, 0), JPH::Vec3(0, 1, 0)},
            {JPH::Vec3(0, 0, -1), JPH::Vec3(-1, 0, 0), JPH::Vec3(0, 1, 0)},
            {JPH::Vec3(1, 0, 0), JPH::Vec3(0, 0, -1), JPH::Vec3(0, 1, 0)},
            {JPH::Vec3(-1, 0, 0), JPH::Vec3(0, 0, 1), JPH::Vec3(0, 1, 0)},
            {JPH::Vec3(0, 1, 0), JPH::Vec3(1, 0, 0), JPH::Vec3(0, 0, -1)},
            {JPH::Vec3(0, -1, 0), JPH::Vec3(1, 0, 0), JPH::Vec3(0, 0, 1)},
        }};

        PackedRGBA8 col = Math::PackColor(color.GetX(), color.GetY(), color.GetZ(), color.GetW());

        for (const auto& f: faces) {
            uint32_t      fBase      = static_cast<uint32_t>(positions.size());
            JPH::Vec3     norm       = rot * f.n;
            Packed1010102 packedNorm = Math::PackNormal(norm.GetX(), norm.GetY(), norm.GetZ());
            Packed1010102 packedTang = Math::PackNormal(1.0f, 0.0f, 0.0f);

            std::array<std::pair<float, float>, 4> uvs = {{{-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}}};
            for (const auto& [su, sv]: uvs) {
                JPH::Vec3 offset = f.n * hs + (f.u * hs) * su + (f.v * hs) * sv;
                JPH::Vec3 pos    = center + rot * offset;

                positions.push_back({{pos.GetX(), pos.GetY(), pos.GetZ()}});
                attributes.push_back({.normal = packedNorm, .tangent = packedTang, .uv = Math::PackUV(0.0f, 0.0f), .color = col});
            }

            indices.push_back(fBase);
            indices.push_back(fBase + 1);
            indices.push_back(fBase + 2);
            indices.push_back(fBase);
            indices.push_back(fBase + 2);
            indices.push_back(fBase + 3);
        }
    }

    Mesh Build(RenderContext& rc) {
        Mesh mesh;
        mesh.posBuffer   = rc.CreateVertexBuffer(positions.data(), positions.size() * sizeof(VertexPosition));
        mesh.attrBuffer  = rc.CreateVertexBuffer(attributes.data(), attributes.size() * sizeof(VertexAttributes));
        mesh.indexBuffer = rc.CreateIndexBuffer(indices.data(), indices.size() * sizeof(uint32_t));
        mesh.vertexCount = static_cast<uint32_t>(positions.size());
        mesh.indexCount  = static_cast<uint32_t>(indices.size());
        return mesh;
    }
};

AssetID GetOrGenerateWeaponMeshAsset(RenderContext& rc, WeaponId id) {
    std::string assetName = std::format("procedural_weapon_single_mesh_{}", static_cast<uint32_t>(id));
    AssetID     assetId   = HashAssetID(assetName);

    if (!rc.GetGPUMesh(assetId).has_value()) {
        ProceduralWeaponMeshBuilder builder;
        JPH::Vec4                   metalCol(0.20f, 0.22f, 0.24f, 1.0f);
        JPH::Vec4                   woodCol(0.40f, 0.25f, 0.12f, 1.0f);
        float                       scale = 0.85f;

        if (id == WeaponId::Shotgun) {
            builder.AddBox(JPH::Vec3(0.000f, 0.000f, 0.040f) * scale, JPH::Vec3(0.062f, 0.095f, 0.400f) * scale, metalCol);
            builder.AddBox(JPH::Vec3(0.000f, 0.036f, 0.440f) * scale, JPH::Vec3(0.042f, 0.042f, 0.560f) * scale, metalCol);
            builder.AddBox(JPH::Vec3(0.000f, -0.014f, 0.400f) * scale, JPH::Vec3(0.036f, 0.036f, 0.460f) * scale, metalCol);
            builder.AddBox(JPH::Vec3(0.000f, -0.012f, 0.350f) * scale, JPH::Vec3(0.062f, 0.062f, 0.190f) * scale, woodCol);
            builder.AddBox(JPH::Vec3(0.000f, -0.082f, -0.280f) * scale, JPH::Vec3(0.050f, 0.100f, 0.070f) * scale, woodCol);
            builder.AddBox(JPH::Vec3(0.000f, -0.008f, -0.260f) * scale, JPH::Vec3(0.055f, 0.100f, 0.280f) * scale, woodCol);
        } else if (id == WeaponId::Minigun) {
            builder.AddBox(JPH::Vec3(0.000f, 0.000f, 0.500f) * scale, JPH::Vec3(0.120f, 0.120f, 0.780f) * scale, metalCol);
            builder.AddBox(JPH::Vec3(0.000f, 0.000f, -0.060f) * scale, JPH::Vec3(0.200f, 0.216f, 0.340f) * scale, metalCol);
            builder.AddBox(JPH::Vec3(0.000f, -0.040f, -0.260f) * scale, JPH::Vec3(0.160f, 0.140f, 0.260f) * scale, metalCol);
            builder.AddBox(JPH::Vec3(-0.020f, -0.200f, -0.340f) * scale, JPH::Vec3(0.260f, 0.260f, 0.160f) * scale, metalCol);
        } else { // Rifle
            builder.AddBox(JPH::Vec3(0.000f, 0.000f, 0.060f) * scale, JPH::Vec3(0.058f, 0.088f, 0.460f) * scale, metalCol);
            builder.AddBox(JPH::Vec3(0.000f, 0.010f, 0.420f) * scale, JPH::Vec3(0.034f, 0.034f, 0.340f) * scale, metalCol);
            builder.AddBox(JPH::Vec3(0.000f, 0.010f, 0.620f) * scale, JPH::Vec3(0.044f, 0.044f, 0.060f) * scale, metalCol);
            builder.AddBox(
                JPH::Vec3(0.000f, -0.110f, 0.020f) * scale, JPH::Vec3(0.048f, 0.160f, 0.088f) * scale, metalCol,
                JPH::Quat::sRotation(JPH::Vec3::sAxisX(), 0.25f)
            );
            builder.AddBox(JPH::Vec3(0.000f, -0.080f, -0.300f) * scale, JPH::Vec3(0.044f, 0.100f, 0.070f) * scale, metalCol);
            builder.AddBox(JPH::Vec3(0.000f, 0.004f, -0.240f) * scale, JPH::Vec3(0.048f, 0.082f, 0.240f) * scale, metalCol);
            builder.AddBox(JPH::Vec3(0.000f, 0.050f, 0.125f) * scale, JPH::Vec3(0.026f, 0.038f, 0.055f) * scale, metalCol);
        }

        Mesh mesh = builder.Build(rc);
        rc.RegisterGPUMesh(assetId, mesh);
    }
    return assetId;
}

Entity CreateWeaponModel(Engine* engine, WeaponId id, MaterialID metalMat, MaterialID woodMat) {
    auto& reg = engine->GetRegistry();
    auto& rc  = engine->GetRenderContext();

    AssetID meshAsset = GetOrGenerateWeaponMeshAsset(rc, id);

    Entity weapon = reg.Create();
    reg.Add(weapon, Components::TransformComponent {});
    reg.Add(weapon, Components::NameComponent {.name = String64("ProceduralWeapon")});
    reg.Add(weapon, Components::MeshComponent {.meshAsset = meshAsset, .materialAsset = (id == WeaponId::Shotgun) ? woodMat : metalMat, .cullRadius = 2.0f});

    return weapon;
}

// ============================================================================
// PARTICLE GENERATOR
// ============================================================================

void SpawnImpactParticles(const JPH::Vec3& point, const JPH::Vec3& normal, uint32_t materialType) {
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
            g_State.particles.push_back(p);
        }
    } else { // Sparks
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
            g_State.particles.push_back(p);
        }
    }
}

void PushKillFeed(const std::string& text, bool headshot) {
    g_State.killFeed.insert(g_State.killFeed.begin(), KillFeedItem {.id = ++g_State.feedCounter, .text = text, .head = headshot});
    if (g_State.killFeed.size() > 5) {
        g_State.killFeed.pop_back();
    }
}

// ============================================================================
// WEAPON FIRE LOGIC WITH PENETRATION PIPELINE
// ============================================================================

void ProcessPlayerWeaponFire(Engine* engine, GameplayComponents::PlayerController& p) {
    const auto& def       = GetWeaponDef(p.currentWeapon);
    auto&       ammoState = p.ammo[static_cast<size_t>(p.currentWeapon)];

    if (p.spin < 1.0f && def.spinUp > 0.0f) {
        return;
    }

    if (ammoState.mag <= 0 || p.reloading > 0.0f || p.fireCd > 0.0f) {
        if (ammoState.mag <= 0 && p.reloading <= 0.0f && p.fireCd <= 0.0f) {
            PlaySound_Empty(engine);
            p.fireCd = 0.25f;
        }
        return;
    }

    if (!p.godMode && !p.infiniteAmmo) {
        ammoState.mag--;
    }
    p.shotsFired++;
    p.fireCd = def.fireRate;

    float adsScale = 1.0f - p.ads * def.adsTighten;
    float growth   = std::min(1.0f, static_cast<float>(p.shotsFired) / 9.0f);
    p.pitchRecoil.ApplyImpulse(def.recoilPitch * (0.75f + growth * 0.45f) * adsScale);
    p.yawRecoil.ApplyImpulse((((std::rand() % 100) / 100.0f) - 0.45f) * def.recoilYaw * (0.8f + growth * 0.7f) * adsScale);
    p.kickSpring.ApplyImpulse(def.kick);

    auto& pc  = engine->GetPhysicsContext();
    auto& reg = engine->GetRegistry();
    auto& cam = engine->GetCamera();

    JPH::Vec3 origin = cam.position;

    // --- UPDATE 1: Compute Muzzle World Position ---
    JPH::Vec3 muzzleWorld = origin;
    if (g_State.weaponEntity != NullEntity && reg.IsAlive(g_State.weaponEntity)) {
        if (auto* wTrans = reg.Get<Components::TransformComponent>(g_State.weaponEntity)) {
            muzzleWorld = wTrans->position + (wTrans->rotation * JPH::Vec3(0.0f, 0.012f, 0.66f));
        }
    }

    float     yawRad   = JPH::DegreesToRadians(cam.yaw);
    float     pitchRad = JPH::DegreesToRadians(cam.pitch);
    JPH::Vec3 baseDir(JPH::Cos(yawRad) * JPH::Cos(pitchRad), JPH::Sin(pitchRad), JPH::Sin(yawRad) * JPH::Cos(pitchRad));

    std::random_device                    rd;
    std::mt19937                          gen(rd());
    std::uniform_real_distribution<float> dis(-1.0f, 1.0f);

    float moving      = (p.bobAmt * 0.028f) * (def.pellets > 1 ? 0.45f : 1.0f);
    float totalSpread = (def.baseSpread + moving + std::min(1.0f, p.shotsFired / 10.0f) * 0.02f) * (1.0f - p.ads * def.adsTighten);

    Entity ignorePhys = NullEntity;
    if (g_State.playerEnt != NullEntity && reg.IsAlive(g_State.playerEnt)) {
        if (auto* phys = reg.Get<Components::PhysicsComponent>(g_State.playerEnt)) {
            ignorePhys = phys->physicsHandle;
        }
    }

    bool anyHit    = false;
    bool anyPierce = false;

    struct HitRecord {
        float                         t        = 220.0f;
        Entity                        victim   = NullEntity;
        std::optional<Actor::BodyHit> bodyHit  = std::nullopt;
        bool                          isGround = false;
        JPH::Vec3                     point    = JPH::Vec3::sZero();
        JPH::Vec3                     normal   = JPH::Vec3::sZero();
    };

    for (uint32_t pel = 0; pel < def.pellets; ++pel) {
        float cone = totalSpread;
        if (def.pellets > 1) {
            float r = (pel == 0) ? 0.05f : (pel <= 3) ? 0.45f : 1.0f;
            cone += def.patternSpread * (1.0f - p.ads * def.adsTighten) * r;
        }

        JPH::Vec3 dir = baseDir.Normalized() + JPH::Vec3(dis(gen) * cone, dis(gen) * cone, dis(gen) * cone);
        dir           = dir.Normalized();

        float maxT = def.range;

        // Collect all potential hits along the ray
        std::vector<HitRecord> hits;

        // 1. Raycast Static World
        auto worldHit = Physics::Raycast(pc, JPH::RVec3(origin), dir, maxT, ignorePhys);
        if (worldHit.hasHit) {
            hits.push_back(
                {.t        = static_cast<float>(worldHit.fraction) * maxT,
                 .victim   = NullEntity,
                 .bodyHit  = std::nullopt,
                 .isGround = false,
                 .point    = JPH::Vec3(worldHit.position),
                 .normal   = worldHit.normal}
            );
        }

        // 2. Raycast Enemies
        for (Entity enemyEnt: g_State.enemies) {
            if (!reg.IsAlive(enemyEnt))
                continue;
            auto* enemy = reg.Get<GameplayComponents::EnemyController>(enemyEnt);
            if (enemy) {
                auto bHit = enemy->behavior.Raycast(origin, dir, maxT);
                if (bHit) {
                    hits.push_back({.t = bHit->t, .victim = enemyEnt, .bodyHit = bHit, .isGround = false, .point = bHit->point, .normal = bHit->normal});
                }
            }
        }

        std::sort(hits.begin(), hits.end(), [](const HitRecord& a, const HitRecord& b) { return a.t < b.t; });

        float    pen           = def.penetration;
        float    dmgScale      = 1.0f;
        uint32_t bodiesPierced = 0;
        float    endT          = maxT;

        for (const auto& hit: hits) {
            if (hit.t > endT)
                break;

            if (hit.victim != NullEntity && hit.bodyHit) { // Enemy Hit
                auto* enemy = reg.Get<GameplayComponents::EnemyController>(hit.victim);
                if (enemy && enemy->behavior.alive) {
                    Actor::ActorContext dummyCtx;
                    dummyCtx.fx.spawnImpact = [](JPH::Vec3Arg pt, JPH::Vec3Arg n, uint32_t type) { SpawnImpactParticles(pt, n, type); };
                    dummyCtx.onKilled       = [&](bool hs) {
                        g_State.kills++;
                        g_State.score += hs ? 250 : 100;
                        if (hs)
                            g_State.headshots++;
                        PlaySound_Kill(engine);
                        PushKillFeed(hs ? "HEADSHOT — HOSTILE DOWN" : "HOSTILE DOWN", hs);
                    };

                    float falloff = FalloffAt(def, hit.t);
                    float dmg     = def.damage * falloff * dmgScale;

                    enemy->behavior.Damage(dmg, *hit.bodyHit, dir, dummyCtx, true);

                    anyHit          = true;
                    p.hitMarkerTime = p.totalTime;
                    if (hit.bodyHit->zone == 0) {
                        p.headMarkerTime = p.totalTime;
                    }

                    if (bodiesPierced > 0) {
                        anyPierce          = true;
                        p.pierceMarkerTime = p.totalTime;
                    }

                    bodiesPierced++;
                    if (pen < BODY_PEN_COST || bodiesPierced >= def.maxBodyPierces || def.bodyPenRetain <= 0.0f) {
                        endT = hit.t;
                        break;
                    }
                    pen -= BODY_PEN_COST;
                    dmgScale *= def.bodyPenRetain;
                }
            } else { // Static Geometry Hit
                SpawnImpactParticles(hit.point, hit.normal, 0);
                PlaySound_Impact(engine, hit.t);
                endT = hit.t;
                break;
            }
        }

        JPH::Vec3 endPoint = origin + dir * endT;

        // --- UPDATE 2: Emit tracer line from muzzleWorld ---
        BulletTracer tracer;
        tracer.start         = muzzleWorld;
        tracer.direction     = (endPoint - muzzleWorld).Normalized();
        tracer.speed         = 320.0f;
        tracer.length        = 3.2f;
        tracer.totalDistance = (endPoint - muzzleWorld).Length();
        tracer.traveled      = 0.0f;
        tracer.color         = JPH::Vec3(1.0f, 0.81f, 0.44f);
        g_State.tracers.push_back(tracer);
    }

    if (def.pellets > 1) {
        PlaySound_Shotgun(engine, 0.0f);
    } else if (def.spinUp > 0.0f) {
        PlaySound_Minigun(engine);
    } else {
        PlaySound_Shoot(engine, 0.0f);
    }

    if (anyPierce) {
        PlaySound_Pierce(engine);
    }
    if (anyHit) {
        PlaySound_Hitmark(engine);
    }
}

// ============================================================================
// KINETIC BLAST SYSTEM
// ============================================================================

void ProcessKineticBlast(Engine* engine, GameplayComponents::PlayerController& p) {
    if (!p.alive)
        return;
    if (p.blastCd > 0.0f) {
        PlaySound_Empty(engine);
        return;
    }

    p.blastCd   = BLAST_COOLDOWN;
    p.blastTime = p.totalTime;

    auto& reg = engine->GetRegistry();
    auto& cam = engine->GetCamera();

    float     yawRad   = JPH::DegreesToRadians(cam.yaw);
    float     pitchRad = JPH::DegreesToRadians(cam.pitch);
    JPH::Vec3 aimDir(JPH::Cos(yawRad) * JPH::Cos(pitchRad), JPH::Sin(pitchRad), JPH::Sin(yawRad) * JPH::Cos(pitchRad));
    aimDir = aimDir.Normalized();

    JPH::Vec3 origin   = cam.position + aimDir * 0.9f;
    float     cosAngle = JPH::Cos(JPH::DegreesToRadians(62.0f));

    uint32_t stunnedCount = 0;

    for (Entity enemyEnt: g_State.enemies) {
        if (!reg.IsAlive(enemyEnt))
            continue;
        auto* enemy = reg.Get<GameplayComponents::EnemyController>(enemyEnt);
        if (!enemy)
            continue;

        JPH::Vec3 targetPos = enemy->behavior.position + JPH::Vec3(0, 1.2f, 0);
        JPH::Vec3 toTarget  = targetPos - origin;
        float     dist      = toTarget.Length();

        if (dist > BLAST_RANGE || dist < 0.01f)
            continue;

        JPH::Vec3 dirToTarget = toTarget / dist;
        float     facing      = dirToTarget.Dot(aimDir);

        if (facing < cosAngle)
            continue;

        float     falloff   = std::pow(1.0f - dist / BLAST_RANGE, 0.7f);
        JPH::Vec3 launchDir = (aimDir * 0.55f + dirToTarget * 0.45f).Normalized();
        launchDir.SetY(launchDir.GetY() + 0.55f);
        launchDir = launchDir.Normalized();

        float     launchSpeed = BLAST_FORCE * (0.45f + 0.55f * falloff);
        JPH::Vec3 impulse     = launchDir * launchSpeed;

        if (enemy->behavior.alive) {
            Actor::BodyHit      hit {.t = dist, .point = targetPos, .normal = -aimDir, .joint = Rig::Joint::Chest, .mult = 1.0f, .zone = 1};
            Actor::ActorContext dummyCtx;
            enemy->behavior.Damage(20.0f * falloff, hit, launchDir, dummyCtx, true);
            enemy->behavior.state = Actor::AIState::Suppressed;
            enemy->behavior.ragdoll.ApplyImpulse(static_cast<uint32_t>(Rig::Joint::Chest), impulse);
            stunnedCount++;
        }
    }

    KineticShockwave wave {.position = origin, .direction = aimDir, .radius = BLAST_RANGE * 0.62f, .life = 0.55f, .maxLife = 0.55f};
    g_State.shockwaves.push_back(wave);

    p.pitchRecoil.ApplyImpulse(2.6f);
    p.kickSpring.ApplyImpulse(4.5f);
    PlaySound_Blast(engine);

    if (stunnedCount > 0) {
        PushKillFeed(std::format("{} HOSTILES STUNNED", stunnedCount), false);
        g_State.score += stunnedCount * 25;
    }
}

// ============================================================================
// PICKUPS SYSTEM
// ============================================================================

void UpdatePickupsSystem(Engine* engine, GameplayComponents::PlayerController& p, float dt) {
    auto& reg = engine->GetRegistry();

    JPH::Vec3 playerPos = JPH::Vec3::sZero();
    if (g_State.playerEnt != NullEntity && reg.IsAlive(g_State.playerEnt)) {
        if (auto* trans = reg.Get<Components::TransformComponent>(g_State.playerEnt)) {
            playerPos = trans->position;
        }
    }

    if (p.pickupFlash > 0.0f) {
        p.pickupFlash = std::max(0.0f, p.pickupFlash - dt);
    }

    for (auto it = g_State.pickups.begin(); it != g_State.pickups.end();) {
        it->age += dt;

        if (!it->rested) {
            it->velocity.SetY(it->velocity.GetY() - 20.0f * dt);
            it->position += it->velocity * dt;

            JPH::Quat rotX = JPH::Quat::sRotation(JPH::Vec3::sAxisX(), it->spin.GetX() * dt);
            JPH::Quat rotY = JPH::Quat::sRotation(JPH::Vec3::sAxisY(), it->spin.GetY() * dt);
            JPH::Quat rotZ = JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), it->spin.GetZ() * dt);
            JPH::Quat rot  = rotX * rotY * rotZ;

            if (it->entity != NullEntity && reg.IsAlive(it->entity)) {
                if (auto* trans = reg.Get<Components::TransformComponent>(it->entity)) {
                    trans->position = it->position;
                    trans->rotation = (rot * trans->rotation).Normalized();
                }
            }

            if (it->position.GetY() < 0.05f) {
                it->position.SetY(0.05f);
                it->velocity *= 0.35f;
                it->velocity.SetY(it->velocity.GetY() * -0.3f);
                it->spin *= 0.3f;
                if (it->velocity.LengthSq() < 0.15f) {
                    it->rested = true;
                }
            }
        }

        float distSq = (it->position - playerPos).LengthSq();
        if (p.alive && distSq < 1.5f * 1.5f && !it->claimed) {
            auto&   ammoState = p.ammo[static_cast<size_t>(it->weaponId)];
            int32_t cap       = GetWeaponDef(it->weaponId).startReserve * 2;
            int32_t room      = std::max(0, cap - ammoState.reserve);

            if (room > 0) {
                int32_t take = std::min(it->ammo, room);
                ammoState.reserve += take;
                it->ammo -= take;
                it->claimed = (it->ammo <= 0);

                PlaySound_Pickup(engine);
                p.pickupFlash     = 1.4f;
                p.pickupFlashText = std::format("+{} {}", take, GetWeaponDef(it->weaponId).caliber);
                PushKillFeed(std::format("+{} {} · {}", take, GetWeaponDef(it->weaponId).name, GetWeaponDef(it->weaponId).caliber), false);
            }
        }

        if (it->claimed || it->age >= it->ttl) {
            if (it->entity != NullEntity && reg.IsAlive(it->entity)) {
                reg.Destroy(it->entity);
            }
            it = g_State.pickups.erase(it);
        } else {
            ++it;
        }
    }
}

// ============================================================================
// GAME INITIALIZATION & SPAWN LOOPS
// ============================================================================

void SpawnEnemy(Engine* engine, JPH::Vec3Arg position, WeaponId weaponId = WeaponId::Rifle) {
    auto& reg = engine->GetRegistry();
    auto& pc  = engine->GetPhysicsContext();
    auto& rc  = engine->GetRenderContext();

    AssetID soldierMeshAsset = Soldier::GetOrGenerateSoldierMeshAsset(rc);

    static uint32_t s_NextJointOffset = 0;
    uint32_t        myJointOffset     = s_NextJointOffset;
    s_NextJointOffset += Rig::JointCount;

    Entity enemyEnt = reg.Create();
    reg.Add(enemyEnt, Components::TransformComponent {.position = position});
    reg.Add(enemyEnt, Components::NameComponent {.name = String64("TacticalSoldier")});
    reg.Add(
        enemyEnt, Components::MeshComponent {
                      .meshAsset = soldierMeshAsset, .materialAsset = g_State.enemyMat, .cullRadius = 2.5f, .jointOffset = myJointOffset, .isSkinned = true
                  }
    );

    // --- ADD JOLT CHARACTER CONTROLLER FOR SOLID COLLISION ---
    Entity physChar = Physics::CreateCharacter(pc, JPH::RVec3(position));
    reg.Add(enemyEnt, Components::PhysicsComponent {physChar});
    reg.Add(enemyEnt, Components::PhysicsStateComponent {.currPosition = position, .prevPosition = position});

    auto& enemy    = reg.Add(enemyEnt, GameplayComponents::EnemyController {});
    enemy.weaponId = weaponId;
    enemy.behavior.SetPosition(position);
    enemy.behavior.health = 100.0f + g_State.wave * 6.0f;

    enemy.weaponEntity = CreateWeaponModel(engine, weaponId, g_State.metalMat, g_State.crateMat);
    g_State.enemies.push_back(enemyEnt);
}

void EnemyAISystem(Engine* engine, float dt) {
    auto& reg = engine->GetRegistry();
    auto& pc  = engine->GetPhysicsContext();
    auto& rc  = engine->GetRenderContext();

    // 1. Cache T-Pose Bind World Positions & Inverses
    static std::array<JPH::Vec3, Rig::JointCount> s_BindWorldPos = []() {
        std::array<JPH::Vec3, Rig::JointCount> W;
        for (uint32_t i = 0; i < Rig::JointCount; ++i) {
            Rig::Joint j      = static_cast<Rig::Joint>(i);
            JPH::Vec3  local  = Rig::GetBindPosition(j);
            int32_t    parent = Rig::GetParentIndex(j);
            W[i]              = (parent >= 0) ? W[parent] + local : local;
        }
        return W;
    }();

    static std::array<JPH::Mat44, Rig::JointCount> s_InvBindMatrices = []() {
        std::array<JPH::Mat44, Rig::JointCount> invBind;
        for (uint32_t i = 0; i < Rig::JointCount; ++i) {
            invBind[i] = JPH::Mat44::sTranslation(-s_BindWorldPos[i]);
        }
        return invBind;
    }();

    static const std::array<int32_t, Rig::JointCount> s_JointChild = []() {
        std::array<int32_t, Rig::JointCount> childs;
        childs.fill(-1);
        childs[static_cast<size_t>(Rig::Joint::Hips)]      = static_cast<int32_t>(Rig::Joint::Spine);
        childs[static_cast<size_t>(Rig::Joint::Spine)]     = static_cast<int32_t>(Rig::Joint::Chest);
        childs[static_cast<size_t>(Rig::Joint::Chest)]     = static_cast<int32_t>(Rig::Joint::Neck);
        childs[static_cast<size_t>(Rig::Joint::Neck)]      = static_cast<int32_t>(Rig::Joint::Head);
        childs[static_cast<size_t>(Rig::Joint::Head)]      = static_cast<int32_t>(Rig::Joint::HeadEnd);
        childs[static_cast<size_t>(Rig::Joint::ClavicleL)] = static_cast<int32_t>(Rig::Joint::UpperArmL);
        childs[static_cast<size_t>(Rig::Joint::UpperArmL)] = static_cast<int32_t>(Rig::Joint::ForearmL);
        childs[static_cast<size_t>(Rig::Joint::ForearmL)]  = static_cast<int32_t>(Rig::Joint::HandL);
        childs[static_cast<size_t>(Rig::Joint::HandL)]     = static_cast<int32_t>(Rig::Joint::HandEndL);
        childs[static_cast<size_t>(Rig::Joint::ClavicleR)] = static_cast<int32_t>(Rig::Joint::UpperArmR);
        childs[static_cast<size_t>(Rig::Joint::UpperArmR)] = static_cast<int32_t>(Rig::Joint::ForearmR);
        childs[static_cast<size_t>(Rig::Joint::ForearmR)]  = static_cast<int32_t>(Rig::Joint::HandR);
        childs[static_cast<size_t>(Rig::Joint::HandR)]     = static_cast<int32_t>(Rig::Joint::HandEndR);
        childs[static_cast<size_t>(Rig::Joint::ThighL)]    = static_cast<int32_t>(Rig::Joint::ShinL);
        childs[static_cast<size_t>(Rig::Joint::ShinL)]     = static_cast<int32_t>(Rig::Joint::FootL);
        childs[static_cast<size_t>(Rig::Joint::FootL)]     = static_cast<int32_t>(Rig::Joint::ToeL);
        childs[static_cast<size_t>(Rig::Joint::ThighR)]    = static_cast<int32_t>(Rig::Joint::ShinR);
        childs[static_cast<size_t>(Rig::Joint::ShinR)]     = static_cast<int32_t>(Rig::Joint::FootR);
        childs[static_cast<size_t>(Rig::Joint::FootR)]     = static_cast<int32_t>(Rig::Joint::ToeR);
        return childs;
    }();

    for (auto it = g_State.enemies.begin(); it != g_State.enemies.end();) {
        Entity enemyEnt = *it;
        if (!reg.IsAlive(enemyEnt)) {
            it = g_State.enemies.erase(it);
            continue;
        }

        auto* enemyPtr = reg.Get<GameplayComponents::EnemyController>(enemyEnt);
        if (!enemyPtr) {
            ++it;
            continue;
        }
        auto& enemy = *enemyPtr;

        // 2. Setup AI & Stance Context
        Actor::ActorContext ctx;
        ctx.playerPos   = (g_State.playerEnt != NullEntity && reg.IsAlive(g_State.playerEnt) && reg.Get<Components::TransformComponent>(g_State.playerEnt)) ?
                              reg.Get<Components::TransformComponent>(g_State.playerEnt)->position :
                              JPH::Vec3::sZero();
        ctx.playerAlive = (g_State.playerEnt != NullEntity && reg.IsAlive(g_State.playerEnt));
        ctx.time        = static_cast<float>(engine->GetCurrentFrame()) * 0.0166f;
        ctx.floorY      = 0.0f;

        ctx.world.pointBlocked = [&](JPH::Vec3Arg pos, float radius) {
            JPH::Array<Entity> results;
            Physics::OverlapSphere(pc, JPH::RVec3(pos), radius, results);
            return !results.empty();
        };

        ctx.world.lineOfSight = [](JPH::Vec3Arg, JPH::Vec3Arg) { return true; };

        Entity ignorePhys = NullEntity;
        if (auto* phys = reg.Get<Components::PhysicsComponent>(enemyEnt)) {
            ignorePhys = phys->physicsHandle;
        }

        ctx.world.raycastWorld = [&](JPH::Vec3Arg origin, JPH::Vec3Arg direction, float maxDistance) -> std::optional<Actor::BodyHit> {
            auto hit = Physics::Raycast(pc, JPH::RVec3(origin), direction, maxDistance, ignorePhys);
            if (hit.hasHit) {
                return Actor::BodyHit {
                    .t = hit.fraction * maxDistance, .point = JPH::Vec3(hit.position), .normal = hit.normal, .joint = Rig::Joint::Hips, .mult = 1.0f, .zone = 2
                };
            }
            return std::nullopt;
        };

        ctx.fx.playBeep = [&](float freq, float dur, float vol) { engine->GetAudioContext().PlayProceduralBeep(freq, dur, vol); };

        ctx.updateAnimation = [&enemy](float frameDt, float speed, float crouch, float aimYaw, float aimPitch, float aiming) -> Actor::WeaponStance {
            AnimInput ai = {.speed = speed, .crouch = crouch, .aimYaw = aimYaw, .aimPitch = aimPitch, .aiming = aiming};
            enemy.anim.Update(frameDt, ai);

            JPH::Vec3 worldWeaponPos = enemy.behavior.position + enemy.anim.weaponPos;
            JPH::Vec3 worldMuzzle    = enemy.behavior.position + enemy.anim.muzzleWorld;

            return Actor::WeaponStance {.position = worldWeaponPos, .rotation = enemy.anim.weaponQuat, .muzzleWorld = worldMuzzle, .aimDir = enemy.anim.aimDir};
        };

        ctx.fx.fireWeapon   = [&enemy]() { enemy.anim.Fire(1.0f); };
        ctx.fx.reloadWeapon = [&enemy]() { enemy.anim.StartReload(); };

        enemy.behavior.Update(dt, ctx);

        auto* phys = reg.Get<Components::PhysicsComponent>(enemyEnt);

        if (enemy.behavior.alive) {
            if (phys && phys->physicsHandle != NullEntity) {
                Physics::SetCharacterVelocity(pc, phys->physicsHandle, enemy.behavior.velocity);
            }

            // Convert Jolt capsule center (Y = 0.80m) to floor level (Y = 0.0m)
            if (auto* state = reg.Get<Components::PhysicsStateComponent>(enemyEnt)) {
                float feetY             = state->currPosition.GetY() - 0.80f; // Subtract capsule half-height
                enemy.behavior.position = JPH::Vec3(state->currPosition.GetX(), std::max(0.0f, feetY), state->currPosition.GetZ());
            }
        }

        // Sync Root Transform to ground level
        if (auto* eTrans = reg.Get<Components::TransformComponent>(enemyEnt)) {
            eTrans->position = enemy.behavior.position; // <--- Feet sit directly at Y = 0.0m on floor
            eTrans->rotation = JPH::Quat::sRotation(JPH::Vec3::sAxisY(), enemy.behavior.yaw);
        }

        // 3. Animate Legs (Human Knees) & Weapon Stance Arms
        if (enemy.behavior.alive) {
            float speed  = enemy.behavior.speed;
            float stride = (speed > 3.2f) ? 1.85f : 1.25f;
            enemy.phase += (speed / stride) * dt;

            float speedRatio = std::min(1.0f, speed / 1.5f);
            float swingAngle = std::sin(enemy.phase * JPH::JPH_PI * 2.0f) * 0.42f * speedRatio;
            float kneeAngleL = std::max(0.0f, std::cos(enemy.phase * JPH::JPH_PI * 2.0f)) * 0.85f * speedRatio;
            float kneeAngleR = std::max(0.0f, -std::cos(enemy.phase * JPH::JPH_PI * 2.0f)) * 0.85f * speedRatio;

            JPH::Quat bodyRot = JPH::Quat::sRotation(JPH::Vec3::sAxisY(), enemy.behavior.yaw);
            JPH::Vec3 rootPos = enemy.behavior.position;

            auto& pos = enemy.behavior.boneWorldPositions;

            // --- HIPS (1.0m Above Ground Level) ---
            pos[static_cast<size_t>(Rig::Joint::Hips)] = rootPos + bodyRot * Rig::GetBindPosition(Rig::Joint::Hips); // <--- FIX: Lifts body 1.0m above floor

            // Torso & Head
            pos[static_cast<size_t>(Rig::Joint::Spine)]   = pos[static_cast<size_t>(Rig::Joint::Hips)] + bodyRot * Rig::GetBindPosition(Rig::Joint::Spine);
            pos[static_cast<size_t>(Rig::Joint::Chest)]   = pos[static_cast<size_t>(Rig::Joint::Spine)] + bodyRot * Rig::GetBindPosition(Rig::Joint::Chest);
            pos[static_cast<size_t>(Rig::Joint::Neck)]    = pos[static_cast<size_t>(Rig::Joint::Chest)] + bodyRot * Rig::GetBindPosition(Rig::Joint::Neck);
            pos[static_cast<size_t>(Rig::Joint::Head)]    = pos[static_cast<size_t>(Rig::Joint::Neck)] + bodyRot * Rig::GetBindPosition(Rig::Joint::Head);
            pos[static_cast<size_t>(Rig::Joint::HeadEnd)] = pos[static_cast<size_t>(Rig::Joint::Head)] + bodyRot * Rig::GetBindPosition(Rig::Joint::HeadEnd);

            // Left Leg
            JPH::Quat thighRotL = JPH::Quat::sRotation(JPH::Vec3::sAxisX(), swingAngle);
            JPH::Quat kneeRotL  = thighRotL * JPH::Quat::sRotation(JPH::Vec3::sAxisX(), kneeAngleL);

            pos[static_cast<size_t>(Rig::Joint::ThighL)] = pos[static_cast<size_t>(Rig::Joint::Hips)] + bodyRot * Rig::GetBindPosition(Rig::Joint::ThighL);
            pos[static_cast<size_t>(Rig::Joint::ShinL)]  = pos[static_cast<size_t>(Rig::Joint::ThighL)] +
                                                           bodyRot * (thighRotL * Rig::GetBindPosition(Rig::Joint::ShinL));
            pos[static_cast<size_t>(Rig::Joint::FootL)]  = pos[static_cast<size_t>(Rig::Joint::ShinL)] +
                                                           bodyRot * (kneeRotL * Rig::GetBindPosition(Rig::Joint::FootL));
            pos[static_cast<size_t>(Rig::Joint::ToeL)]   = pos[static_cast<size_t>(Rig::Joint::FootL)] +
                                                           bodyRot * (kneeRotL * Rig::GetBindPosition(Rig::Joint::ToeL));

            // Right Leg
            JPH::Quat thighRotR = JPH::Quat::sRotation(JPH::Vec3::sAxisX(), -swingAngle);
            JPH::Quat kneeRotR  = thighRotR * JPH::Quat::sRotation(JPH::Vec3::sAxisX(), kneeAngleR);

            pos[static_cast<size_t>(Rig::Joint::ThighR)] = pos[static_cast<size_t>(Rig::Joint::Hips)] + bodyRot * Rig::GetBindPosition(Rig::Joint::ThighR);
            pos[static_cast<size_t>(Rig::Joint::ShinR)]  = pos[static_cast<size_t>(Rig::Joint::ThighR)] +
                                                           bodyRot * (thighRotR * Rig::GetBindPosition(Rig::Joint::ShinR));
            pos[static_cast<size_t>(Rig::Joint::FootR)]  = pos[static_cast<size_t>(Rig::Joint::ShinR)] +
                                                           bodyRot * (kneeRotR * Rig::GetBindPosition(Rig::Joint::FootR));
            pos[static_cast<size_t>(Rig::Joint::ToeR)]   = pos[static_cast<size_t>(Rig::Joint::FootR)] +
                                                           bodyRot * (kneeRotR * Rig::GetBindPosition(Rig::Joint::ToeR));

            // Clavicles & Arms
            pos[static_cast<size_t>(Rig::Joint::ClavicleL)] = pos[static_cast<size_t>(Rig::Joint::Chest)] +
                                                              bodyRot * Rig::GetBindPosition(Rig::Joint::ClavicleL);
            pos[static_cast<size_t>(Rig::Joint::ClavicleR)] = pos[static_cast<size_t>(Rig::Joint::Chest)] +
                                                              bodyRot * Rig::GetBindPosition(Rig::Joint::ClavicleR);

            pos[static_cast<size_t>(Rig::Joint::UpperArmL)] = pos[static_cast<size_t>(Rig::Joint::ClavicleL)] +
                                                              bodyRot * Rig::GetBindPosition(Rig::Joint::UpperArmL);
            pos[static_cast<size_t>(Rig::Joint::UpperArmR)] = pos[static_cast<size_t>(Rig::Joint::ClavicleR)] +
                                                              bodyRot * Rig::GetBindPosition(Rig::Joint::UpperArmR);

            // Weapon Gripping Hands & Elbows
            JPH::Vec3 worldWeaponPos = enemy.behavior.position + enemy.anim.weaponPos;
            JPH::Quat weaponQuat     = enemy.anim.weaponQuat;

            JPH::Vec3 gripR = worldWeaponPos + weaponQuat * JPH::Vec3(0.0f, -0.075f, -0.115f);
            JPH::Vec3 gripL = worldWeaponPos + weaponQuat * JPH::Vec3(0.0f, -0.020f, 0.170f);

            // Right Arm (Trigger Hand)
            JPH::Vec3 shR                                  = pos[static_cast<size_t>(Rig::Joint::UpperArmR)];
            pos[static_cast<size_t>(Rig::Joint::ForearmR)] = (shR + gripR) * 0.5f + bodyRot * JPH::Vec3(-0.12f, -0.12f, -0.05f);
            pos[static_cast<size_t>(Rig::Joint::HandR)]    = gripR;
            pos[static_cast<size_t>(Rig::Joint::HandEndR)] = gripR + weaponQuat * JPH::Vec3(0.0f, 0.0f, 0.08f);

            // Left Arm (Support Hand)
            JPH::Vec3 shL                                  = pos[static_cast<size_t>(Rig::Joint::UpperArmL)];
            pos[static_cast<size_t>(Rig::Joint::ForearmL)] = (shL + gripL) * 0.5f + bodyRot * JPH::Vec3(0.12f, -0.12f, -0.05f);
            pos[static_cast<size_t>(Rig::Joint::HandL)]    = gripL;
            pos[static_cast<size_t>(Rig::Joint::HandEndL)] = gripL + weaponQuat * JPH::Vec3(0.0f, 0.0f, 0.08f);
        }

        // 4. Sync Root Transform
        if (auto* eTrans = reg.Get<Components::TransformComponent>(enemyEnt)) {
            eTrans->position = enemy.behavior.position;
            eTrans->rotation = JPH::Quat::sRotation(JPH::Vec3::sAxisY(), enemy.behavior.yaw);
        }

        // 5. Compute Full Oriented Joint Matrices for GPU Skinning
        if (auto* meshComp = reg.Get<Components::MeshComponent>(enemyEnt)) {
            JPH::Mat44 rootMatrix = JPH::Mat44::sRotationTranslation(JPH::Quat::sRotation(JPH::Vec3::sAxisY(), enemy.behavior.yaw), enemy.behavior.position);
            JPH::Mat44 invRoot    = rootMatrix.Inversed();

            std::array<JPH::Mat44, Rig::JointCount> jointWorldMatrices;

            for (uint32_t j = 0; j < Rig::JointCount; ++j) {
                int32_t   child = s_JointChild[j];
                JPH::Vec3 pJ    = enemy.behavior.boneWorldPositions[j];

                if (child >= 0) {
                    JPH::Vec3 pC      = enemy.behavior.boneWorldPositions[child];
                    JPH::Vec3 dir     = pC - pJ;
                    float     dLen    = dir.Length();
                    JPH::Vec3 bindDir = (s_BindWorldPos[child] - s_BindWorldPos[j]).Normalized();

                    if (dLen > 0.001f && bindDir.LengthSq() > 0.001f) {
                        JPH::Quat rot         = JPH::Quat::sFromTo(bindDir, dir / dLen);
                        jointWorldMatrices[j] = JPH::Mat44::sRotationTranslation(rot, pJ);
                    } else {
                        jointWorldMatrices[j] = JPH::Mat44::sTranslation(pJ);
                    }
                } else {
                    int32_t parent = Rig::GetParentIndex(static_cast<Rig::Joint>(j));
                    if (parent >= 0) {
                        jointWorldMatrices[j] = jointWorldMatrices[parent] * JPH::Mat44::sTranslation(Rig::GetBindPosition(static_cast<Rig::Joint>(j)));
                    } else {
                        jointWorldMatrices[j] = JPH::Mat44::sTranslation(pJ);
                    }
                }
            }

            std::array<JPH::Mat44, Rig::JointCount> gpuMatrices;
            for (uint32_t j = 0; j < Rig::JointCount; ++j) {
                gpuMatrices[j] = invRoot * jointWorldMatrices[j] * s_InvBindMatrices[j];
            }

            rc.UpdateJointMatrices(meshComp->jointOffset, gpuMatrices.data(), Rig::JointCount);
        }

        // 6. Sync Held Enemy Weapon Position
        if (!enemy.behavior.weaponDropped && enemy.weaponEntity != NullEntity && reg.IsAlive(enemy.weaponEntity)) {
            if (auto* wTrans = reg.Get<Components::TransformComponent>(enemy.weaponEntity)) {
                wTrans->position = enemy.behavior.position + enemy.anim.weaponPos;
                wTrans->rotation = enemy.anim.weaponQuat;
            }
        }

        // 7. On Enemy Death: Drop Weapon Pickup
        if (!enemy.behavior.alive && !enemy.behavior.weaponDropped) {
            WeaponPickup pu {
                .entity   = enemy.weaponEntity,
                .weaponId = enemy.weaponId,
                .ammo     = GetWeaponDef(enemy.weaponId).magSize,
                .position = enemy.behavior.position + JPH::Vec3(0, 1.0f, 0),
                .velocity = JPH::Vec3(0, 3.0f, 0),
                .spin     = JPH::Vec3(4.0f, 2.0f, 1.0f)
            };
            g_State.pickups.push_back(pu);
            enemy.behavior.weaponDropped = true;
            enemy.weaponEntity           = NullEntity;
        }

        ++it;
    }

    // Maintain wave threshold in Horde Mode
    uint32_t targetHostiles = g_State.hordeMode ? g_State.hordeTarget : (3 + std::min(6u, g_State.wave));
    if (g_State.enemies.size() < targetHostiles) {
        g_State.waveTimer -= dt;
        if (g_State.waveTimer <= 0.0f) {
            g_State.waveTimer  = g_State.hordeMode ? 0.2f : 2.4f;
            float    randAngle = ((std::rand() % 100) / 100.0f) * 6.283f;
            WeaponId wId       = (((std::rand() % 100) / 100.0f) < 0.35f) ? WeaponId::Shotgun : WeaponId::Rifle;
            SpawnEnemy(engine, JPH::Vec3(std::cos(randAngle) * 22.0f, 0.0f, std::sin(randAngle) * 22.0f), wId);
        }
    }
}

void StartGame(Engine* engine) {
    auto& reg = engine->GetRegistry();
    auto& pc  = engine->GetPhysicsContext();
    auto& rc  = engine->GetRenderContext();

    ZHLN::Log("[Blacksite] Initializing FPS Tactical Sandbox...");

    auto settingsEntities = reg.GetEntitiesWith<Components::GlobalSettingsTagComponent>();
    if (!settingsEntities.empty()) {
        if (auto* pp = reg.Get<Components::PostProcessSettingsComponent>(settingsEntities[0])) {
            pp->giMode            = 1;
            pp->ambientExposure   = 12.0f;
            pp->enableSSR         = 1;
            pp->enableRTR         = 0;
            pp->vignetteIntensity = 1.15f;
            pp->vignettePower     = 1.6f;
        }
    }

    g_State.concreteMat = HashAssetID("concrete_mat_asset");
    g_State.metalMat    = HashAssetID("metal_mat_asset");
    g_State.barrierMat  = HashAssetID("barrier_mat_asset");
    g_State.crateMat    = HashAssetID("crate_mat_asset");
    g_State.sandbagMat  = HashAssetID("sandbag_mat_asset");
    g_State.enemyMat    = HashAssetID("enemy_mat_asset");

    rc.RegisterGPUMaterial(g_State.concreteMat, CreativeWorksFactory::CreateBasicMaterial(rc).value());
    rc.RegisterGPUMaterial(g_State.metalMat, CreativeWorksFactory::CreateBasicMaterial(rc).value());
    rc.RegisterGPUMaterial(g_State.barrierMat, CreativeWorksFactory::CreateBasicMaterial(rc).value());
    rc.RegisterGPUMaterial(g_State.crateMat, CreativeWorksFactory::CreateBasicMaterial(rc).value());
    rc.RegisterGPUMaterial(g_State.sandbagMat, CreativeWorksFactory::CreateBasicMaterial(rc).value());

    auto enemyMaterial               = CreativeWorksFactory::CreateBasicMaterial(rc).value();
    enemyMaterial.baseColorFactor[0] = 0.28f;
    enemyMaterial.baseColorFactor[1] = 0.33f;
    enemyMaterial.baseColorFactor[2] = 0.26f; // Tactical Olive
    rc.RegisterGPUMaterial(g_State.enemyMat, enemyMaterial);

    g_State.tracerMat   = CreativeWorksFactory::CreateBasicMaterial(rc, true, true, true).value();
    g_State.particleMat = CreativeWorksFactory::CreateBasicMaterial(rc, true, true, true).value();

    AssetID groundMeshAsset = HashAssetID("ground_floor_mesh");
    if (!rc.GetGPUMesh(groundMeshAsset).has_value()) {
        Mesh groundBox = CreativeWorksFactory::CreateBox(rc, JPH::Vec3(100.0f, 0.1f, 100.0f)); // 200m x 0.2m x 200m
        rc.RegisterGPUMesh(groundMeshAsset, groundBox);
    }

    auto groundShape   = Physics::GetOrCreateShape(pc, Physics::ShapeType::Plane, 0.0f, 1.0f, 0.0f, 0.0f);
    g_State.floorPlane = reg.Create();
    reg.Add(g_State.floorPlane, Components::TransformComponent {.position = JPH::Vec3(0.0f, -0.1f, 0.0f)});
    reg.Add(
        g_State.floorPlane,
        Components::PhysicsComponent {Physics::CreateRigidBody(pc, groundShape, JPH::RVec3::sZero(), JPH::Quat::sIdentity(), JPH::EMotionType::Static, 0)}
    );
    reg.Add(g_State.floorPlane, Components::MeshComponent {.meshAsset = groundMeshAsset, .materialAsset = g_State.concreteMat, .cullRadius = 250.0f});
    reg.Add(g_State.floorPlane, Components::PBRComponent {.roughness = 0.92f, .metallic = 0.05f});

    g_State.sunLight = reg.Create();
    reg.Add(g_State.sunLight, Components::TransformComponent {.rotation = JPH::Quat::sRotation(JPH::Vec3::sAxisX(), -0.78f)});
    reg.Add(
        g_State.sunLight, Components::LightComponent {
                              .type        = LightType::Sun,
                              .color       = JPH::Vec3(1.0f, 0.98f, 0.95f),
                              .intensity   = 150.0f,
                              .radius      = 0.5f,
                              .direction   = JPH::Vec3(0.4f, 0.8f, 0.4f).Normalized(), // Positive Y = Sun in the sky
                              .range       = 500.0f,
                              .shadowLayer = -1
                          }
    );

    AddBox(engine, JPH::Vec3(0.0f, 0.0f, -6.0f), JPH::Vec3(14.0f, 5.0f, 10.0f), g_State.concreteMat);
    AddBox(engine, JPH::Vec3(-3.0f, 5.0f, -6.0f), JPH::Vec3(8.0f, 0.4f, 10.0f), g_State.barrierMat, true);
    AddBox(engine, JPH::Vec3(9.0f, 0.0f, -10.0f), JPH::Vec3(4.0f, 2.6f, 4.0f), g_State.concreteMat);

    AddBox(engine, JPH::Vec3(-16.0f, 0.0f, 4.0f), JPH::Vec3(6.0f, 2.6f, 2.5f), g_State.metalMat);
    AddBox(engine, JPH::Vec3(-16.0f, 2.6f, 4.0f), JPH::Vec3(6.0f, 2.6f, 2.5f), g_State.metalMat, true);
    AddBox(engine, JPH::Vec3(-9.0f, 0.0f, 12.0f), JPH::Vec3(2.5f, 2.6f, 6.0f), g_State.metalMat);
    AddBox(engine, JPH::Vec3(14.0f, 0.0f, 8.0f), JPH::Vec3(6.0f, 2.6f, 2.5f), g_State.metalMat);
    AddBox(engine, JPH::Vec3(20.0f, 0.0f, -4.0f), JPH::Vec3(2.5f, 2.6f, 6.0f), g_State.metalMat);
    AddBox(engine, JPH::Vec3(-22.0f, 0.0f, -12.0f), JPH::Vec3(6.0f, 2.6f, 2.5f), g_State.metalMat);

    JPH::Vec3 spawnPos(0.0f, 1.5f, 24.0f);
    g_State.playerEnt = reg.Create();
    reg.Add(g_State.playerEnt, Components::PlayerTagComponent {});
    reg.Add(g_State.playerEnt, Components::TransformComponent {.position = spawnPos});
    reg.Add(g_State.playerEnt, Components::MovementComponent {});
    reg.Add(g_State.playerEnt, Components::InputComponent {});
    reg.Add(g_State.playerEnt, Components::PhysicsComponent {Physics::CreateCharacter(pc, JPH::RVec3(spawnPos))});
    reg.Add(g_State.playerEnt, Components::PhysicsStateComponent {.currPosition = spawnPos, .prevPosition = spawnPos});

    auto& p     = reg.Add(g_State.playerEnt, GameplayComponents::PlayerController {});
    p.baseYaw   = -90.0f;
    p.basePitch = 0.0f;

    g_State.weaponEntity = CreateWeaponModel(engine, p.currentWeapon, g_State.metalMat, g_State.crateMat);

    auto camEnts = reg.GetEntitiesWith<Components::MainCameraTagComponent>();
    if (!camEnts.empty()) {
        Entity camEnt    = camEnts[0];
        auto*  targetCam = reg.Get<Components::TargetCameraComponent>(camEnt);
        if (!targetCam) {
            targetCam = &reg.Add(camEnt, Components::TargetCameraComponent {});
        }
        targetCam->target          = g_State.playerEnt;
        targetCam->distance        = 0.0f; // 0.0f for First-Person
        targetCam->targetDistance  = 0.0f;
        targetCam->yaw             = -90.0f;
        targetCam->pitch           = 0.0f;
        targetCam->stiffness       = 0.0f;
        targetCam->targetOffset    = JPH::Vec3(0.0f, PLAYER_EYE_OFFSET_Y, 0.0f);
        targetCam->smoothTargetPos = spawnPos;
    }

    for (int i = 0; i < 5; ++i) {
        float    randAngle = (static_cast<float>(i) / 5.0f) * 6.283f;
        WeaponId wId       = (i % 3 == 0) ? WeaponId::Shotgun : WeaponId::Rifle;
        SpawnEnemy(engine, JPH::Vec3(std::cos(randAngle) * 18.0f, 0.0f, std::sin(randAngle) * 18.0f), wId);
    }

    uint32_t fontIdx = 0;
    for (Entity uiEnt: reg.GetEntitiesWith<Components::UISettingsComponent>()) {
        if (auto* uiSettings = reg.Get<Components::UISettingsComponent>(uiEnt)) {
            fontIdx = uiSettings->defaultFontAtlasIdx;
            break;
        }
    }

    g_State.hudVitalsBg   = reg.Create();
    auto& bgRect          = reg.Add(g_State.hudVitalsBg, Components::UIRectComponent {});
    bgRect.anchorMinX     = 0.0f;
    bgRect.anchorMaxX     = 0.0f;
    bgRect.anchorMinY     = 1.0f;
    bgRect.anchorMaxY     = 1.0f;
    bgRect.x              = 24.0f;
    bgRect.y              = -80.0f;
    bgRect.width          = 200.0f;
    bgRect.height         = 14.0f;
    bgRect.hierarchyDepth = 10;

    auto& bgPanel = reg.Add(g_State.hudVitalsBg, Components::UIPanelComponent {});
    bgPanel.color = JPH::Vec4(0.12f, 0.12f, 0.16f, 0.65f);

    g_State.hudVitalsBar   = reg.Create();
    auto& barRect          = reg.Add(g_State.hudVitalsBar, Components::UIRectComponent {});
    barRect.parentEntity   = g_State.hudVitalsBg;
    barRect.x              = 2.0f;
    barRect.y              = 2.0f;
    barRect.width          = 196.0f;
    barRect.height         = 10.0f;
    barRect.hierarchyDepth = 11;

    auto& barPanel = reg.Add(g_State.hudVitalsBar, Components::UIPanelComponent {});
    barPanel.color = JPH::Vec4(0.35f, 0.95f, 0.45f, 0.95f);

    g_State.hudAmmoText     = reg.Create();
    auto& ammoRect          = reg.Add(g_State.hudAmmoText, Components::UIRectComponent {});
    ammoRect.anchorMinX     = 1.0f;
    ammoRect.anchorMaxX     = 1.0f;
    ammoRect.anchorMinY     = 1.0f;
    ammoRect.anchorMaxY     = 1.0f;
    ammoRect.x              = -240.0f;
    ammoRect.y              = -85.0f;
    ammoRect.width          = 200.0f;
    ammoRect.height         = 40.0f;
    ammoRect.hierarchyDepth = 10;

    auto& ammoText = reg.Add(g_State.hudAmmoText, Components::TextComponent {});
    ammoText.text.assign("30 / 210");
    ammoText.scale     = 1.25f;
    ammoText.fontIndex = fontIdx;
    ammoText.color     = JPH::Vec4(0.95f, 0.95f, 0.95f, 0.95f);

    g_State.hudCrosshair  = reg.Create();
    auto& chRect          = reg.Add(g_State.hudCrosshair, Components::UIRectComponent {});
    chRect.anchorMinX     = 0.5f;
    chRect.anchorMaxX     = 0.5f;
    chRect.anchorMinY     = 0.5f;
    chRect.anchorMaxY     = 0.5f;
    chRect.x              = -6.0f;
    chRect.y              = -8.0f;
    chRect.width          = 20.0f;
    chRect.height         = 20.0f;
    chRect.hierarchyDepth = 15;

    auto& chText = reg.Add(g_State.hudCrosshair, Components::TextComponent {});
    chText.text.assign("+");
    chText.scale     = 1.5f;
    chText.fontIndex = fontIdx;
    chText.color     = JPH::Vec4(0.43f, 1.00f, 0.70f, 0.85f);

    g_State.hudWaveText     = reg.Create();
    auto& waveRect          = reg.Add(g_State.hudWaveText, Components::UIRectComponent {});
    waveRect.anchorMinX     = 0.0f;
    waveRect.anchorMaxX     = 0.0f;
    waveRect.anchorMinY     = 0.0f;
    waveRect.anchorMaxY     = 0.0f;
    waveRect.x              = 24.0f;
    waveRect.y              = 20.0f;
    waveRect.width          = 250.0f;
    waveRect.height         = 80.0f;
    waveRect.hierarchyDepth = 10;

    auto& waveText = reg.Add(g_State.hudWaveText, Components::TextComponent {});
    waveText.text.assign("WAVE 01");
    waveText.scale     = 1.0f;
    waveText.fontIndex = fontIdx;
    waveText.color     = JPH::Vec4(0.55f, 0.82f, 1.00f, 0.85f);

    g_State.gameStarted = true;
}

// ============================================================================
// SYSTEM UPDATE TICK RUNNERS
// ============================================================================

void PlayerInputSystem(Engine* engine, [[maybe_unused]] float dt) {
    auto&       reg   = engine->GetRegistry();
    const auto& input = engine->GetInput();

    if (g_State.playerEnt == NullEntity || !reg.IsAlive(g_State.playerEnt)) {
        return;
    }

    auto* p    = reg.Get<GameplayComponents::PlayerController>(g_State.playerEnt);
    auto* move = reg.Get<Components::MovementComponent>(g_State.playerEnt);
    if (!p || !move) {
        return;
    }

    // 1. Process Mouse Look
    if (!g_State.mainMenu.IsActive()) {
        const float sensitivity = 0.15f;
        float       mouseDeltaX = input.GetMouse().deltaX;
        float       mouseDeltaY = input.GetMouse().deltaY;

        p->baseYaw += mouseDeltaX * sensitivity;
        p->basePitch = std::clamp(p->basePitch - (mouseDeltaY * sensitivity), -89.0f, 89.0f);
    }

    // 2. Process WASD Movement
    float yawRad   = JPH::DegreesToRadians(p->baseYaw);
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
    if (len > 0.01f) {
        move->inputX = moveX / len;
        move->inputZ = moveZ / len;
    } else {
        move->inputX = 0.0f;
        move->inputZ = 0.0f;
    }

    move->isSprinting = input.IsKeyDown(KeyCode::LShift) && (len > 0.01f);
    if (input.IsKeyDown(KeyCode::Space)) {
        move->jumpRequested = true;
    }

    // Weapon Switching Inputs
    if (input.IsKeyDown(KeyCode::E) || input.IsKeyDown(KeyCode::R)) {
        // Interhandled via direct key triggers in update tick
    }
}

void PlayerUpdateTick(Engine* engine, float dt) {
    auto&       reg   = engine->GetRegistry();
    const auto& input = engine->GetInput();
    auto&       cam   = engine->GetCamera();

    if (g_State.playerEnt == NullEntity) {
        return;
    }

    auto* p    = reg.Get<GameplayComponents::PlayerController>(g_State.playerEnt);
    auto* move = reg.Get<Components::MovementComponent>(g_State.playerEnt);
    if (!p || !move) {
        return;
    }

    p->totalTime += dt;
    const auto& def = GetWeaponDef(p->currentWeapon);

    // 1. Weapon Swap Logic
    if (p->swapT > 0.0f) {
        float prev = p->swapT;
        p->swapT -= dt;
        if (prev > 0.275f && p->swapT <= 0.275f) {
            p->currentWeapon = p->pendingWeapon;
            if (g_State.weaponEntity != NullEntity && reg.IsAlive(g_State.weaponEntity)) {
                reg.Destroy(g_State.weaponEntity);
            }
            g_State.weaponEntity = CreateWeaponModel(engine, p->currentWeapon, g_State.metalMat, g_State.crateMat);
        }
        if (p->swapT <= 0.0f)
            p->swapT = 0.0f;
    }

    // 2. Weapon Selection Keys (1, 2, 3)
    if (input.IsKeyDown(KeyCode::R)) { // Standard reload trigger
        if (p->reloading <= 0.0f && p->swapT <= 0.0f) {
            auto& ammoState = p->ammo[static_cast<size_t>(p->currentWeapon)];
            if (ammoState.mag < def.magSize && ammoState.reserve > 0) {
                if (def.shellReload > 0.0f) {
                    p->shellsToLoad = std::min(def.magSize - ammoState.mag, ammoState.reserve);
                    p->shellTimer   = 0.35f;
                    p->reloading    = 0.35f + p->shellsToLoad * def.shellReload + 0.3f;
                } else {
                    p->reloading = def.reloadTime;
                }
                PlaySound_Reload(engine);
            }
        }
    }

    // 3. Minigun Barrel Spin-Up
    if (def.spinUp > 0.0f) {
        bool  wantSpin = input.IsMouseButtonDown(KeyCode::LButton) || input.IsMouseButtonDown(KeyCode::RButton);
        float rate     = wantSpin ? dt / def.spinUp : -dt / (def.spinUp * 1.5f);
        p->spin        = MathUtils::Clamp(p->spin + rate, 0.0f, 1.0f);
        p->barrelAngle += p->spin * 46.0f * dt;
    } else {
        p->spin = 0.0f;
    }

    // 4. Reloading Countdown
    p->fireCd = std::max(0.0f, p->fireCd - dt);
    if (p->reloading > 0.0f) {
        p->reloading -= dt;
        auto& ammoState = p->ammo[static_cast<size_t>(p->currentWeapon)];

        if (def.shellReload > 0.0f) {
            p->shellTimer -= dt;
            if (p->shellTimer <= 0.0f && p->shellsToLoad > 0 && ammoState.reserve > 0 && ammoState.mag < def.magSize) {
                ammoState.mag++;
                if (!p->infiniteAmmo)
                    ammoState.reserve--;
                p->shellsToLoad--;
                p->shellTimer = def.shellReload;
                PlaySound_Step(engine); // Reload click cue
            }
        } else if (p->reloading <= 0.0f) {
            int32_t need     = def.magSize - ammoState.mag;
            int32_t transfer = std::min(need, ammoState.reserve);
            ammoState.mag += transfer;
            if (!p->infiniteAmmo) {
                ammoState.reserve -= transfer;
            }
        }
    }

    p->hurtFlash = std::max(0.0f, p->hurtFlash - dt * 1.4f);

    p->pitchRecoil.Update(dt);
    p->yawRecoil.Update(dt);
    p->kickSpring.Update(dt);

    bool canAds = !def.noAds && p->reloading <= 0.0f && p->swapT <= 0.0f;
    p->ads      = MathUtils::Damp(p->ads, (canAds && input.IsMouseButtonDown(KeyCode::RButton)) ? 1.0f : 0.0f, 14.0f, dt);

    float speedSq     = move->inputX * move->inputX + move->inputZ * move->inputZ;
    float planarSpeed = (speedSq > 0.01f) ? (move->isSprinting ? 7.4f : 5.1f) : 0.0f;
    p->bobAmt         = MathUtils::Damp(p->bobAmt, move->isGrounded ? planarSpeed / 6.0f : 0.0f, 8.0f, dt);
    p->bobPhase += planarSpeed * dt * (move->isSprinting ? 2.1f : 1.7f);

    p->stepPhase += planarSpeed * dt * 0.55f;
    if (p->stepPhase > 1.0f) {
        p->stepPhase -= 1.0f;
        if (move->isGrounded) {
            PlaySound_Step(engine);
        }
    }

    p->landVel += (-160.0f * p->landDip - 18.0f * p->landVel) * dt;
    p->landDip += p->landVel * dt;

    float height = reg.Get<Components::TransformComponent>(g_State.playerEnt)->position.GetY();
    if (move->isGrounded && p->lastHeight - height > 1.5f) {
        p->landVel = -(p->lastHeight - height) * 3.5f;
    }
    p->lastHeight = height;

    float bobScale = 1.0f - p->ads * 0.9f;
    float bobY     = std::sin(p->bobPhase * 2.0f * JPH::JPH_PI) * 0.035f * p->bobAmt * bobScale;
    float bobX     = std::cos(p->bobPhase * JPH::JPH_PI) * 0.045f * p->bobAmt * bobScale;

    cam.yaw   = p->baseYaw + p->yawRecoil.value * 0.35f;
    cam.pitch = std::clamp(p->basePitch + p->pitchRecoil.value, -89.0f, 89.0f);
    cam.fov   = MathUtils::Lerp(HIP_FOV, def.adsFov, p->ads);

    JPH::Vec3 playerPos = JPH::Vec3::sZero();
    if (auto* trans = reg.Get<Components::TransformComponent>(g_State.playerEnt)) {
        playerPos = trans->position;
    }

    JPH::Vec3 mutablePos = playerPos;
    mutablePos.SetY(mutablePos.GetY() + PLAYER_EYE_OFFSET_Y + bobY - p->landDip);
    mutablePos.SetX(mutablePos.GetX() + bobX * 0.4f);
    cam.position = mutablePos;

    auto camEnts = reg.GetEntitiesWith<Components::MainCameraTagComponent>();
    if (!camEnts.empty()) {
        if (auto* targetCam = reg.Get<Components::TargetCameraComponent>(camEnts[0])) {
            targetCam->distance        = 0.0f;
            targetCam->targetDistance  = 0.0f;
            targetCam->yaw             = cam.yaw;
            targetCam->pitch           = cam.pitch;
            targetCam->targetOffset    = JPH::Vec3(0.0f, PLAYER_EYE_OFFSET_Y + bobY - p->landDip, 0.0f);
            targetCam->smoothTargetPos = playerPos;
        }
    }

    if (g_State.weaponEntity != NullEntity && reg.IsAlive(g_State.weaponEntity)) {
        if (auto* wTrans = reg.Get<Components::TransformComponent>(g_State.weaponEntity)) {
            float free   = 1.0f - p->ads;
            float freeSq = free * free;

            float mouseSwayX = input.GetMouse().deltaX;
            float mouseSwayY = input.GetMouse().deltaY;

            p->sway.Update(dt, mouseSwayX * 0.002f, mouseSwayY * 0.002f);

            // 1. Compute Forward View Vector
            float pitchRad = JPH::DegreesToRadians(cam.pitch);
            float yawRad   = JPH::DegreesToRadians(cam.yaw);

            JPH::Vec3 forward(std::cos(yawRad) * std::cos(pitchRad), std::sin(pitchRad), std::sin(yawRad) * std::cos(pitchRad));
            forward = forward.Normalized();

            // 2. Build Pure Right-Handed Basis Vectors (Zero Roll)
            JPH::Vec3 worldUp(0.0f, 1.0f, 0.0f);
            JPH::Vec3 right    = worldUp.Cross(forward).Normalized();
            JPH::Vec3 actualUp = forward.Cross(right).Normalized();

            // 3. Base Offset Interpolation (Hip vs ADS)
            JPH::Vec3 hipBase(-0.14f, 0.05f, 0.38f); // +0.05f raises the gun UP on screen
            JPH::Vec3 adsBase(0.0f, -SIGHT_HEIGHT, 0.285f);
            JPH::Vec3 base = MathUtils::Lerp(hipBase, adsBase, p->ads);

            float offsetX = base.GetX() + (p->sway.currentSwayX + std::sin(p->bobPhase * 2.0f * JPH::JPH_PI) * 0.012f * p->bobAmt) * freeSq;
            float offsetY = base.GetY() + (p->sway.currentSwayY + std::abs(std::cos(p->bobPhase * JPH::JPH_PI)) * 0.012f * p->bobAmt) * freeSq;
            float offsetZ = base.GetZ() - p->kickSpring.value * 0.04f;

            if (p->swapT > 0.0f) {
                float swapDip = std::sin(MathUtils::Clamp(1.0f - std::abs(p->swapT - 0.275f) / 0.275f, 0.0f, 1.0f) * (JPH::JPH_PI * 0.5f));
                offsetY -= swapDip * 0.34f;
                offsetZ -= swapDip * 0.10f;
            }

            // 4. Build Right-Handed Basis Matrix (det = +1.0)
            JPH::Mat44 basis = JPH::Mat44::sIdentity();
            basis.SetColumn3(0, right);
            basis.SetColumn3(1, actualUp);
            basis.SetColumn3(2, forward);

            JPH::Quat camRot  = basis.GetQuaternion().Normalized();
            JPH::Quat kickRot = MathUtils::EulerYXZ(-p->kickSpring.value * 0.55f, p->kickSpring.value * 0.12f, p->kickSpring.value * 0.2f);

            // 5. Position & Orient Weapon
            wTrans->position = cam.position + right * offsetX + actualUp * offsetY + forward * offsetZ;
            wTrans->rotation = (camRot * kickRot).Normalized();
        }
    }

    if (input.IsMouseButtonDown(KeyCode::LButton)) {
        ProcessPlayerWeaponFire(engine, *p);
    } else {
        p->shotsFired = std::max(0, static_cast<int>(p->shotsFired - dt * 6.0f));
    }

    // Kinetic Blast Trigger
    if (input.IsKeyDown(KeyCode::E)) { // F/E key blast activation
        ProcessKineticBlast(engine, *p);
    }

    UpdatePickupsSystem(engine, *p, dt);

    // Update HUD Interfaces
    auto& ammoState = p->ammo[static_cast<size_t>(p->currentWeapon)];

    if (g_State.hudVitalsBar != NullEntity && reg.IsAlive(g_State.hudVitalsBar)) {
        if (auto* barRect = reg.Get<Components::UIRectComponent>(g_State.hudVitalsBar)) {
            float hpPct    = std::max(0.0f, p->health / p->maxHealth);
            barRect->width = 196.0f * hpPct;
        }
        if (auto* barPanel = reg.Get<Components::UIPanelComponent>(g_State.hudVitalsBar)) {
            if (p->health < 35.0f) {
                barPanel->color = JPH::Vec4(0.92f, 0.15f, 0.15f, 0.95f);
            } else {
                barPanel->color = JPH::Vec4(0.35f, 0.95f, 0.45f, 0.95f);
            }
        }
    }

    if (g_State.hudAmmoText != NullEntity && reg.IsAlive(g_State.hudAmmoText)) {
        if (auto* ammoText = reg.Get<Components::TextComponent>(g_State.hudAmmoText)) {
            ammoText->text.assign(std::format("{:02d} / {}", ammoState.mag, ammoState.reserve));
        }
    }

    if (g_State.hudCrosshair != NullEntity && reg.IsAlive(g_State.hudCrosshair)) {
        if (auto* chText = reg.Get<Components::TextComponent>(g_State.hudCrosshair)) {
            float alpha = std::clamp(1.0f - p->ads, 0.0f, 1.0f);

            if (p->totalTime - p->hitMarkerTime < 0.18f) {
                chText->text.assign("x");
                if (p->totalTime - p->headMarkerTime < 0.25f) {
                    chText->color = JPH::Vec4(1.00f, 0.30f, 0.30f, 0.95f);
                } else if (p->totalTime - p->pierceMarkerTime < 0.28f) {
                    chText->color = JPH::Vec4(0.20f, 0.85f, 1.00f, 0.95f);
                } else {
                    chText->color = JPH::Vec4(1.00f, 1.00f, 1.00f, 0.95f);
                }
            } else {
                chText->text.assign("+");
                chText->color = JPH::Vec4(0.43f, 1.00f, 0.70f, alpha * 0.85f);
            }
        }
    }

    if (g_State.hudWaveText != NullEntity && reg.IsAlive(g_State.hudWaveText)) {
        if (auto* waveText = reg.Get<Components::TextComponent>(g_State.hudWaveText)) {
            waveText->text.assign(std::format("WAVE {:02d} | HOSTILES: {}", g_State.wave, g_State.enemies.size()));
        }
    }
}

void ProcessRenderTick(Engine* engine, float dt) {
    auto& rc  = engine->GetRenderContext();
    auto& cam = engine->GetCamera();

    AssetID unitBoxAsset = HashAssetID("unit_box");
    if (!rc.GetGPUMesh(unitBoxAsset).has_value()) {
        rc.RegisterGPUMesh(unitBoxAsset, CreativeWorksFactory::CreateBox(rc, JPH::Vec3(0.5f, 0.5f, 0.5f)));
    }

    // 1. Process CPU Tracer Lines
    for (auto it = g_State.tracers.begin(); it != g_State.tracers.end();) {
        it->traveled += it->speed * dt;
        if (it->traveled - it->length > it->totalDistance) {
            it = g_State.tracers.erase(it);
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

                Renderer::Draw(rc, g_State.tracerMat, *rc.GetGPUMesh(unitBoxAsset), params);
            }
            ++it;
        }
    }

    // 2. Process Kinetic Shockwave Rings
    AssetID planeMesh = HashAssetID("unit_plane");
    if (!rc.GetGPUMesh(planeMesh).has_value()) {
        rc.RegisterGPUMesh(planeMesh, CreativeWorksFactory::CreatePlane(rc, 1.0f));
    }

    for (auto it = g_State.shockwaves.begin(); it != g_State.shockwaves.end();) {
        it->life -= dt;
        if (it->life <= 0.0f) {
            it = g_State.shockwaves.erase(it);
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

            Renderer::Draw(rc, g_State.tracerMat, *rc.GetGPUMesh(planeMesh), params);
            ++it;
        }
    }

    // 3. Process Billboard Particles
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

    for (auto it = g_State.particles.begin(); it != g_State.particles.end();) {
        it->life -= dt;
        if (it->life <= 0.0f) {
            it = g_State.particles.erase(it);
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

            Renderer::Draw(rc, g_State.particleMat, *rc.GetGPUMesh(particleMesh), params);
            ++it;
        }
    }
}

} // namespace Game

GAMEPLAY_API ZHLN::GameplayStatus NativeGameplayUpdate(ZHLN::Engine* engine, float dt) {
    if (!engine) {
        return ZHLN::GameplayStatus::Error;
    }

    ZHLN_PROFILE_SCOPE("ECS System: Native Gameplay Update");

    static bool wasTabDown = false;
    bool        isTabDown  = engine->GetInput().IsKeyDown(ZHLN::KeyCode::Tab) || engine->GetInput().IsKeyDown(ZHLN::KeyCode::Escape);

    if (!Game::g_State.gameStarted) {
        ZHLN::MenuConfig cfg;
        cfg.titleLogoPrefab = "";
        cfg.themeMusicPath  = "";
        cfg.cameraPosition  = JPH::Vec3(0.0f, 1.5f, 12.0f);
        cfg.cameraYaw       = -90.0f;
        cfg.cameraPitch     = 0.0f;

        cfg.buttons.push_back(
            {.text = "DEPLOY",
             .onClick =
                 [](ZHLN::Engine* eng) {
                     eng->GetWindow().CaptureMouse(true);
                     Game::StartGame(eng);
                     Game::g_State.mainMenu.Destroy(eng);
                 },
             .textX = 55.0f,
             .textY = 25.0f}
        );

        cfg.buttons.push_back({.text = "QUIT", .onClick = [](ZHLN::Engine* eng) { eng->GetWindow().Close(); }, .textX = 80.0f, .textY = 25.0f});

        Game::g_State.mainMenu.Build(engine, cfg);
    } else if (isTabDown && !wasTabDown) {
        if (Game::g_State.mainMenu.IsActive()) {
            engine->GetWindow().CaptureMouse(true);
            Game::g_State.mainMenu.Destroy(engine);
        } else {
            engine->GetWindow().CaptureMouse(false);
            ZHLN::MenuConfig cfg;
            cfg.cameraPosition = engine->GetCamera().position;
            cfg.cameraYaw      = engine->GetCamera().yaw;
            cfg.cameraPitch    = engine->GetCamera().pitch;

            cfg.buttons.push_back(
                {.text = "RESUME",
                 .onClick =
                     [](ZHLN::Engine* eng) {
                         eng->GetWindow().CaptureMouse(true);
                         Game::g_State.mainMenu.Destroy(eng);
                     },
                 .textX = 55.0f,
                 .textY = 25.0f}
            );

            cfg.buttons.push_back({.text = "QUIT", .onClick = [](ZHLN::Engine* eng) { eng->GetWindow().Close(); }, .textX = 80.0f, .textY = 25.0f});

            Game::g_State.mainMenu.Build(engine, cfg);
        }
    }
    wasTabDown = isTabDown;

    if (Game::g_State.mainMenu.IsActive()) {
        Game::g_State.mainMenu.Update(engine, dt);
        return ZHLN::GameplayStatus::OK;
    }

    if (Game::g_State.gameStarted) {
        Game::PlayerInputSystem(engine, dt);
        Game::PlayerUpdateTick(engine, dt);
        Game::EnemyAISystem(engine, dt);
        Game::ProcessRenderTick(engine, dt);
    }

    return ZHLN::GameplayStatus::OK;
}
