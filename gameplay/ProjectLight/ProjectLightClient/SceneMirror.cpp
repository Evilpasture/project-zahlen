// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include <ProjectLightClient/SceneMirror.hpp>

#include <Camera/TargetCamera.hpp>
#include <CharacterController/CharacterController.hpp>
#include <Replication/ClassVocabulary.hpp>
#include <Zahlen/Audio/AudioValues.hpp>
#include <Zahlen/COM/IActionContext.hpp>
#include <Zahlen/COM/IAssetLibrary.hpp>
#include <Zahlen/COM/IAudioDevice.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/COM/ISimulation.hpp>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/ECS/ComponentPolicy.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/Engine/SpawnParams.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math/Angles.hpp>
#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Math/Transform.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Physics/PhysicsValues.hpp>
#include <Zahlen/Render/GraphicsSettings.hpp>
#include <Zahlen/COM/ILogger.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>
#include <cstdint>
#include <format>
#include <string_view>
#include <utility>
#include <vector>
#include <Zahlen/COM/LoggerView.hpp>


namespace ZHLN::ProjectLight {
namespace {

// Bias toward responsiveness: keep a short remote playout delay to smooth
// 60 Hz arrivals without making moving props feel as far behind as 40 ms.
constexpr auto kNetworkSmoothDelay = std::chrono::milliseconds(20);
constexpr float kLocalJumpScale = 0.40435f; // sqrt(32 / project-light's 196.2 studs/s^2)

auto SameVec3(FVec3 lhs, FVec3 rhs) noexcept -> bool {
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
}
struct ResolvedDirectionalLight {
    FVec3 direction = {0.0f, 1.0f, 0.0f}; // toward the source, as Zahlen expects
    FVec3 color = FVec3::Splat(1.0f);
    float intensity = 1.0f;
};
auto ProjectLightSunDirectionToSource(float timeOfDay, float latitude) noexcept -> FVec3 {
    // Match LightingService.GetSunDirection(yUp=true): X=east, Y=up, Z=north.
    constexpr auto kSolarDeclination = DegF{23.44f}.ToRadians();
    if (!std::isfinite(timeOfDay) || !std::isfinite(latitude)) return FVec3{0.0f, 1.0f, 0.0f};

    const auto lat = DegF{latitude}.ToRadians();
    const auto hourAngle = DegF{(timeOfDay - 12.0f) * 15.0f}.ToRadians();
    const float sinLat = Sin(lat);
    const float cosLat = Cos(lat);
    const float sinDec = Sin(kSolarDeclination);
    const float cosDec = Cos(kSolarDeclination);
    const float sinHour = Sin(hourAngle);
    const float cosHour = Cos(hourAngle);

    const FVec3 towardSource{
        -cosDec * sinHour,
        sinLat * sinDec + cosLat * cosDec * cosHour,
        cosLat * sinDec - sinLat * cosDec * cosHour
    };
    return towardSource.LengthSquared() > 1.0e-8f ? towardSource.Normalized() : FVec3{0.0f, 1.0f, 0.0f};
}
auto ResolveDirectionalLight(const ReplicatedObject& light, const ReplicatedObject* lighting) noexcept -> ResolvedDirectionalLight {
    // ProjectLight's shaders use L = -sunDirection because that property is
    // the direction light rays travel. Zahlen's LightComponent.direction is L
    // itself: the direction from a surface toward the light source.
    FVec3 travelDirection = light.direction;
    if (light.isMain && lighting != nullptr) {
        travelDirection = -ProjectLightSunDirectionToSource(lighting->timeOfDay, lighting->latitude);
    }
    if (travelDirection.LengthSquared() <= 1.0e-8f) travelDirection = {0.0f, -1.0f, 0.0f};
    travelDirection = travelDirection.Normalized();

    FVec3 color = light.color;
    float intensity = light.intensity;
    // Match ProjectLight's sun-to-moon handoff when the rays would point above
    // the horizon. The moon is antipodal, so convert the final travel vector
    // to the renderer's toward-source convention only after this switch.
    if (travelDirection.y > 0.0f) {
        travelDirection = -travelDirection;
        color = light.nightColor;
        intensity = light.nightIntensity;
    }

    return {
        .direction = -travelDirection,
        .color = color,
        .intensity = std::max(intensity, 0.0f),
    };
}
auto CharacterOffsetForName(std::string_view name) noexcept -> FVec3 {
    if (name == "Head") return FVec3{0.0f, 1.5f, 0.0f};
    if (name == "Left Arm") return FVec3{-1.5f, 0.0f, 0.0f};
    if (name == "Right Arm") return FVec3{1.5f, 0.0f, 0.0f};
    if (name == "Left Leg") return FVec3{-0.5f, -2.0f, 0.0f};
    if (name == "Right Leg") return FVec3{0.5f, -2.0f, 0.0f};
    return FVec3::Zero();
}
auto IsCharacterLimb(std::string_view name) noexcept -> bool {
    return name == "Right Arm" || name == "Left Arm" || name == "Right Leg" || name == "Left Leg";
}
auto LimbPoseAngle(std::string_view partName, HumanoidState state, float time, float walkSpeed) noexcept -> float {
    if (partName == "Head" || partName == "Torso") return 0.0f;
    if (state == HumanoidState::Jumping || state == HumanoidState::Freefall) {
        return (partName == "Right Arm" || partName == "Left Arm") ? -std::numbers::pi_v<float> : 0.0f;
    }
    if (state == HumanoidState::FallingDown) {
        return (partName == "Right Arm" || partName == "Left Arm") ? -2.2f : 0.0f;
    }
    if (state == HumanoidState::GettingUp) {
        return (partName == "Right Arm" || partName == "Left Arm") ? -0.5f : 0.0f;
    }
    const float swing = state == HumanoidState::Running ? std::sin(time * (walkSpeed * 0.6f)) * 0.6f : std::sin(time * 0.75f) * 0.1f;
    if (partName == "Right Arm" || partName == "Left Leg") return swing;
    if (partName == "Left Arm" || partName == "Right Leg") return -swing;
    return 0.0f;
}

// Who owns a replicated pose
// --------------------------
// The split is per body, not per object, and it is decided by ShouldCreatePhysics:
//
// * An instance the server drives (a remote player's parts, a prop we do not
//   own) is static-bodied or bodiless here, and nothing samples a static body,
//   so the engine's interpolation never looks at it. The mirror writes
//   TransformComponent and the 20 ms playout in SyncSceneEntities is the only
//   interpolation that happens. These helpers are the write.
// * An instance this client owns is the other side: it gets a real Jolt body,
//   VisualInterpolationSystem interpolates it from body state every frame, and
//   the mirror's job for it is to read that state back and send it. Writing a
//   render pose for one of those would be overwritten a phase later.
// * A local character part is neither: the body lives on the controller entity,
//   not on the limb, so the limb is written here from controller state.
//
// One pose, one writer per entity. That is the invariant to keep when adding a
// pass: if it has a body it belongs to the engine, and if it does not it belongs
// to the mirror.
void DespawnEntity(COM::IEngine& engine, Entity entity) noexcept {
    if (auto* entities = engine.Entities(); entities != nullptr && entity != Entity::Null() && entities->IsAlive(entity)) {
        entities->Despawn(entity);
    }
}

void WriteTransform(COM::IEngine& engine, ReplicatedObject& object, Entity entity, FVec3 position, Quat rotation) {
    object.position = position;
    object.rotationQuat = rotation.Normalized();
    object.renderPosition = object.position;
    object.renderRotation = object.rotationQuat;
    object.rotationEulerDeg = object.rotationQuat.EulerAnglesDegrees();
    object.transformDirty   = true;
    if (entity == Entity::Null())
        return;
    if (auto* transform = engine.Entities()->Get<Components::TransformComponent>(entity)) {
        transform->position = object.position;
        transform->rotation = object.rotationQuat;
    }
}

void WriteInterpolatedTransform(COM::IEngine& engine, ReplicatedObject& object, Entity entity, FVec3 position, Quat rotation) {
    // Keep the authoritative network pose intact; only the render pose is
    // delayed/interpolated. Physics snapshots use object.position/rotationQuat.
    object.renderPosition = position;
    object.renderRotation = rotation.Normalized();
    object.transformDirty = true;
    if (entity == Entity::Null()) return;
    if (auto* transform = engine.Entities()->Get<Components::TransformComponent>(entity)) {
        transform->position   = object.renderPosition;
        transform->rotation   = object.renderRotation;
        object.transformDirty = false;
    }
}

auto FindObject(const SceneMirrorState& state, uint64_t uid) noexcept -> const ReplicatedObject* {
    const auto found = state.objects.find(uid);
    return found == state.objects.end() ? nullptr : &found->second;
}

auto ShouldCreatePhysics(const SceneMirrorState& state, const ReplicatedObject& object) noexcept -> bool {
    if (!IsPartClass(object.className) || IsLocalCharacterPart(state.objects, object.uid, state.localCharacterUid)) return false;
    if (object.anchored) return object.canCollide;
    return !state.serverAuthority && state.localPlayerUid != 0 && object.networkOwnerUid == state.localPlayerUid;
}
void SpawnOrUpdatePartEntity(COM::IEngine& engine, SceneMirrorState& state, ReplicatedObject& object) {
    auto& entities = *engine.Entities();
    const FVec3 size = {std::clamp(std::abs(object.size.x), 0.001f, 100000.0f),
    std::clamp(std::abs(object.size.y), 0.001f, 100000.0f),
    std::clamp(std::abs(object.size.z), 0.001f, 100000.0f)};
    const bool createPhysics = ShouldCreatePhysics(state, object);
    const bool blendMaterial = object.transparency > 0.001f;
    Entity     entity        = state.index.Find(object.uid);
    const bool liveEntity    = entity != Entity::Null() && entities.IsAlive(entity)
                            && !entities.Has<Components::PendingDestroy>(entity);
    // A dead or half-destroyed entity is stale bookkeeping, not a scene object:
    // drop the binding so the mirror cannot hand out an entity the registry has
    // already retired.
    if (!liveEntity && entity != Entity::Null()) {
        state.index.Unbind(object.uid);
        entity = Entity::Null();
    }

    const bool rebuild = !liveEntity || !SameVec3(object.spawnedSize, size) || object.spawnedShape != object.shape
                         || object.spawnedAnchored != object.anchored || object.spawnedCanCollide != object.canCollide
                         || object.spawnedOwnerUid != object.networkOwnerUid || object.hasPhysicsBody != createPhysics
                         || object.spawnedBlend != blendMaterial;
    if (rebuild) {
        if (liveEntity) {
            DespawnEntity(engine, entity);
            state.index.Unbind(object.uid);
            entity = Entity::Null();
        }

        PrefabFactory::SpawnParams params;
        // Physics bodies must start from the latest authoritative pose, not a
        // delayed render sample. This matters when NetworkOwner changes.
        params.position = (createPhysics ? object.position : object.renderPosition).Cast<double>();
        params.rotation = (createPhysics ? object.rotationQuat : object.renderRotation).Normalized();
        params.scale            = size;
        params.createPhysics    = createPhysics;
        params.isStaticPhysics  = object.anchored;
        params.physicsMask      = object.canCollide ? 0xFFFFFFFFu : 0u;
        params.color            = {1.0f, 1.0f, 1.0f, 1.0f};
        params.roughness        = object.roughness;
        params.metallic         = object.metallic;

        auto material = engine.Graphics()->CreateMaterial(blendMaterial ? ZHLN::MaterialDesc::Transparent() : ZHLN::MaterialDesc::Basic());
        if (material.HasValue()) {
            material.value.baseColorFactor = {1.0f, 1.0f, 1.0f, 1.0f};
            material.value.roughnessFactor = object.roughness;
            material.value.metallicFactor  = object.metallic;
            params.materialOverride        = material.value;
        } else {
            ZHLN::Log<ZHLN::LogSeverity::Warning>("Could not create material for '{}': {}", object.name, material.error);
        }

        const FVec3 halfExtents = FVec3::Splat(0.5f);
        auto* assets = engine.Assets();
        switch (object.shape) {
            case PartShape::Sphere:
            case PartShape::Head:
                entity = assets->SpawnSphere(0.5f, params);
                break;
            case PartShape::Cylinder:
                entity = assets->SpawnCylinder(0.5f, 1.0f, params);
                break;
            case PartShape::Cone:
                entity = assets->SpawnCone(0.5f, 1.0f, params);
                break;
            case PartShape::Cube:
            case PartShape::Wedge:
            case PartShape::CornerWedge:
            case PartShape::Capsule:
            default:
                entity = assets->SpawnBox(halfExtents, params);
                break;
        }

        if (entity == Entity::Null() || !entities.IsAlive(entity)) {
            object.materialId = InvalidMaterialID;
            object.hasPhysicsBody = false;
            return;
        }

        // Bind, then write the replication components. From here on the entity
        // answers "which instance am I" and "who owns me" on its own; the index
        // is the only place the reverse question is answered.
        state.index.Bind(object.uid, entity);
        entities.Attach(entity, ProjectLightInstanceComponent {.uid = object.uid});
        entities.Attach(entity, ProjectLightNetworkOwnerComponent {.ownerUid = object.networkOwnerUid});
        entities.Attach(entity, ProjectLightReplicaTag {});

        if (auto* name = entities.Get<Components::NameComponent>(entity)) name->name = String64(object.name);
        if (auto* transform = entities.Get<Components::TransformComponent>(entity)) {
            transform->position = object.renderPosition;
            transform->rotation = object.renderRotation.Normalized();
            transform->scale    = size;
        }
        if (auto* world = entities.Get<Components::WorldTransformComponent>(entity)) {
            const auto matrix = ComposeTransform(object.renderPosition, object.renderRotation.Normalized(), size);
            world->world    = matrix;
            world->previous = matrix;
        }
        if (const auto* mesh = entities.Get<Components::MeshComponent>(entity)) object.materialId = mesh->materialAsset;
        object.hasPhysicsBody = HasPhysicsBodyComponent(entities, entity);
        object.spawnedSize     = size;
        object.spawnedShape    = object.shape;
        object.spawnedAnchored = object.anchored;
        object.spawnedCanCollide = object.canCollide;
        object.spawnedOwnerUid = object.networkOwnerUid;
        object.spawnedBlend    = blendMaterial;
        object.visualDirty     = true;
        object.transformDirty  = false;
    }

    if (entity == Entity::Null() || !entities.IsAlive(entity)) return;

    // Ownership moves without a rebuild in the mirror's view (the rebuild above
    // covers the geometry case), so the component is refreshed on every pass.
    if (auto* owner = entities.Get<ProjectLightNetworkOwnerComponent>(entity)) {
        owner->ownerUid = object.networkOwnerUid;
    }

    if (object.transformDirty) {
        if (auto* transform = entities.Get<Components::TransformComponent>(entity)) {
            transform->position = object.renderPosition;
            transform->rotation = object.renderRotation.Normalized();
            transform->scale    = size;
        }
        object.transformDirty = false;
    }

    if (object.visualDirty && object.materialId != InvalidMaterialID) {
        if (auto material = engine.Graphics()->FindMaterial(object.materialId); material.HasValue()) {
            material.value.baseColorFactor = {object.color.x, object.color.y, object.color.z, 1.0f - object.transparency};
            material.value.emissiveFactor  = {object.color.x * object.emission, object.color.y * object.emission,
                                              object.color.z * object.emission, 1.0f};
            material.value.roughnessFactor = object.roughness;
            material.value.metallicFactor  = object.metallic;
            engine.Graphics()->RegisterMaterial(object.materialId, material.value);
        }
        if (auto* pbr = entities.Get<Components::PBRComponent>(entity)) {
            pbr->roughness = object.roughness;
            pbr->metallic  = object.metallic;
        }
        if (auto* name = entities.Get<Components::NameComponent>(entity)) name->name = String64(object.name);
        object.visualDirty = false;
    }
}
void SpawnOrUpdateLightEntity(COM::IEngine& engine, SceneMirrorState& state, ReplicatedObject& object) {
    auto& entities = *engine.Entities();
    const bool directional = object.className == "DirectionalLight";
    const ReplicatedObject* lighting = directional ? FindObject(state, state.lightingUid) : nullptr;
    const ResolvedDirectionalLight resolved = directional
        ? ResolveDirectionalLight(object, lighting)
        : ResolvedDirectionalLight {.direction = object.direction, .color = object.color, .intensity = object.intensity};
    const float rendererIntensity = object.enabled ? resolved.intensity * (directional ? 180.0f : 1.0f) : 0.0f;
    Entity      entity            = state.index.Find(object.uid);
    if (entity == Entity::Null() || !entities.IsAlive(entity) || entities.Has<Components::PendingDestroy>(entity)) {
        entity            = entities.CreateEntity();
        const auto matrix = ComposeTransform(object.position, object.rotationQuat, {1.0f, 1.0f, 1.0f});
        entities.Attach(entity, ProjectLightInstanceComponent {.uid = object.uid});
        entities.Attach(entity, ProjectLightNetworkOwnerComponent {.ownerUid = object.networkOwnerUid});
        entities.Attach(entity, ProjectLightReplicaTag {});
        entities.Attach(entity, Components::NameComponent {.name = String64(object.name)});
        entities.Attach(entity, Components::TransformComponent {.position = object.position, .rotation = object.rotationQuat, .scale = FVec3::Splat(1.0f)});
        entities.Attach(entity, Components::WorldTransformComponent {.world = matrix, .previous = matrix});
        entities.Attach(entity, Components::LightComponent {
            .type      = directional ? LightType::Sun : LightType::Point,
            .color     = resolved.color,
            .intensity = rendererIntensity,
            .radius    = object.sourceRadius,
            .direction = resolved.direction,
            .range     = object.range,
        });
        entities.Attach(entity, Components::SceneLightTagComponent {});
        state.index.Bind(object.uid, entity);
        object.visualDirty = true;
    }

    if (directional && object.isMain && !entities.Get<Components::SunTagComponent>(entity)) {
        entities.Attach(entity, Components::SunTagComponent {});
    } else if (directional && !object.isMain && entities.Get<Components::SunTagComponent>(entity)) {
        entities.Detach<Components::SunTagComponent>(entity);
    }

    if (auto* name = entities.Get<Components::NameComponent>(entity)) name->name = String64(object.name);
    if (auto* transform = entities.Get<Components::TransformComponent>(entity)) {
        transform->position = object.position;
        transform->rotation = object.rotationQuat;
    }
    if (auto* light = entities.Get<Components::LightComponent>(entity)) {
        light->type      = directional ? LightType::Sun : LightType::Point;
        light->color     = resolved.color;
        light->intensity = rendererIntensity;
        light->radius    = object.sourceRadius;
        light->direction = resolved.direction;
        light->range     = object.range;
    }
    object.transformDirty = false;
    object.visualDirty = false;
}
void SyncEnvironment(COM::IEngine& engine, SceneMirrorState& state) {
    const ReplicatedObject* lighting = FindObject(state, state.lightingUid);
    if (lighting == nullptr) return;

    // ProjectLight Ambient is a normalized scalar (default 0.01). Zahlen uses
    // ambientExposure as the environment-light multiplier (default 25), so
    // preserve both defaults and carry server-side relative changes across.
    constexpr float kProjectLightDefaultAmbient = 0.01f;
    constexpr float kZahlenDefaultAmbientExposure = 25.0f;
    const float ambient = std::isfinite(lighting->ambient) ? std::max(lighting->ambient, 0.0f) : kProjectLightDefaultAmbient;
    const float ambientExposure = kZahlenDefaultAmbientExposure * (ambient / kProjectLightDefaultAmbient);

    // This client hands the renderer Zahlen's absolute-radiance light units: a
    // directional light enters at intensity * 180 (see SpawnOrUpdateLightEntity)
    // and the environment at `ambientExposure` above. `exposure` is the display
    // half of that convention -- blit.slang multiplies the whole HDR buffer by
    // it before tone mapping -- and 0.015 is the value Zahlen's own scene preset
    // pairs with exactly those numbers (Scene.hpp Environment::exposure,
    // next to a 180-intensity fallback sun). Left at the component default of
    // 1.0, a lit white surface drives ~57 into the tone curve and both ACES and
    // PBR Neutral saturate it: the whole frame reads as pastel white.
    constexpr float kProjectLightExposure = 0.015f;

    // The procedural-sky constants below are Zahlen's night preset (0.003..0.08
    // radiance). They are also the SH the shadow fill is baked from, and both
    // reach the image as `skyRadiance * ambientExposure * exposure`, so a daylit
    // ProjectLight sky wants them scaled well past 1/exposure. This factor is the
    // sky/shade dial: ~10 is a deep sky with dark shadows, 20 puts the horizon
    // near sRGB 130, ~30 reproduces the (clipped) sky the scene gets today at the
    // component default exposure of 1.0. The sun -- and therefore the
    // highlight-to-shade separation -- is still `exposure`, not this.
    constexpr float kProjectLightSkyScale = 20.0f;

    GraphicsSettings gfx            = engine.Graphics()->Settings();
    gfx.environment.ambientExposure = ambientExposure;
    gfx.post.exposure               = kProjectLightExposure;
    if (lighting->proceduralSky) {
        gfx.environment.skyZenith  = {0.003f * kProjectLightSkyScale, 0.008f * kProjectLightSkyScale, 0.020f * kProjectLightSkyScale, 1.0f};
        gfx.environment.skyHorizon = {0.015f * kProjectLightSkyScale, 0.035f * kProjectLightSkyScale, 0.080f * kProjectLightSkyScale, 1.0f};
        gfx.environment.skyGround  = {0.001f * kProjectLightSkyScale, 0.001f * kProjectLightSkyScale, 0.003f * kProjectLightSkyScale, 1.0f};
    } else {
        gfx.environment.skyZenith  = {0.0f, 0.0f, 0.0f, 1.0f};
        gfx.environment.skyHorizon = {0.0f, 0.0f, 0.0f, 1.0f};
        gfx.environment.skyGround  = {0.0f, 0.0f, 0.0f, 1.0f};
    }
    engine.Graphics()->ApplySettings(gfx);
}
void UpdateRemoteCharacterParts(COM::IEngine& engine, SceneMirrorState& state, float dt) {
    (void)dt;
    const auto renderTime = std::chrono::steady_clock::now() - kNetworkSmoothDelay;
    for (const auto& [userId, history]: state.controllerStates) {
        if (userId == state.localUserId || history.Empty()) continue;
        const ReplicatedObject* player = nullptr;
        for (const auto& [uid, candidate]: state.objects) {
            (void)uid;
            if (candidate.className == "Player" && candidate.userId == userId) { player = &candidate; break; }
        }
        if (player == nullptr || player->characterUid == 0) continue;

        FVec3 rootPosition {};
        Quat rootRotation {};
        if (!history.Sample(renderTime, rootPosition, rootRotation)) continue;
        const ReplicatedObject* humanoid = nullptr;
        for (const auto& [uid, candidate]: state.objects) {
            (void)uid;
            if (candidate.parentUid == player->characterUid && candidate.className == "Humanoid") { humanoid = &candidate; break; }
        }
        const HumanoidState stateValue = humanoid ? humanoid->humanoidState : HumanoidState::Idle;
        const float walkSpeed = humanoid ? humanoid->walkSpeed : 16.0f;

        for (auto& [uid, object]: state.objects) {
            if (!IsPartClass(object.className) || !IsPartOfCharacter(state.objects, uid, player->characterUid)) continue;
            const FVec3 offset = CharacterOffsetForName(object.name);
            const FVec3 position = rootPosition + rootRotation * offset;
            const float poseAngle = IsCharacterLimb(object.name) ? LimbPoseAngle(object.name, stateValue, state.time, walkSpeed) : 0.0f;
            const Quat limbRotation = poseAngle == 0.0f ? rootRotation : rootRotation * Quat::FromAxisAngle(FVec3::UnitX(), ZHLN::RadF{poseAngle});
            WriteInterpolatedTransform(engine, object, state.index.Find(uid), position, limbRotation);
        }
    }
}
// Reads a whole virtual-layer asset (disk or pak) into memory for CreateVoice.
[[nodiscard]] static auto ReadVirtualAsset(COM::IEngine& engine, const std::string& path) -> std::vector<uint8_t> {
    auto* assets = engine.Assets();
    if (assets == nullptr) {
        return {};
    }
    auto opened = assets->OpenVirtualFile(COM::StringView(path));
    if (!opened) {
        return {};
    }
    const auto bytes = (*opened)->Bytes();
    const auto* raw  = reinterpret_cast<const uint8_t*>(bytes.data);
    return std::vector<uint8_t>(raw, raw + bytes.size);
}

void UpdateSoundVoices(COM::IEngine& engine, SceneMirrorState& state) {
    for (auto& [uid, object]: state.objects) {
        (void)uid;
        if (object.className != "Sound") continue;
        auto& audio = *engine.Audio();
        if (object.playing) {
            if (object.audioID == VoiceID::Invalid || !audio.IsVoiceValid(object.audioID)) {
                if (object.soundId.empty()) continue;
                if (auto bytes = ReadVirtualAsset(engine, object.soundId); !bytes.empty()) {
                    object.audioID = audio.CreateVoice(COM::Span<const uint8_t>(bytes.data(), bytes.size()), true, object.loops, object.volume);
                }
                if (object.audioID == VoiceID::Invalid) {
                    continue; // asset may not be present in the local Zahlen installation
                }
                audio.PlayVoice(object.audioID);
            } else {
                audio.SetVoiceVolume(object.audioID, object.volume);
                audio.SetVoiceLooping(object.audioID, object.loops);
            }
            const ReplicatedObject* parent = FindObject(state, object.parentUid);
            if (parent != nullptr)
                audio.SetVoicePosition(object.audioID, parent->position);
        } else if (object.audioID != VoiceID::Invalid) {
            audio.StopVoice(object.audioID, 0.0f);
            object.audioID = VoiceID::Invalid;
        }
    }
}
} // namespace

void SceneMirror::SyncSceneEntities(COM::IEngine& engine, SceneMirrorState& state) {
    auto& entities = *engine.Entities();

    for (uint64_t uid: state.pendingRemovals) {
        auto found = state.objects.find(uid);
        if (found == state.objects.end()) continue;
        ReplicatedObject& object = found->second;
        if (object.audioID != VoiceID::Invalid) {
            engine.Audio()->StopVoice(object.audioID, 0.0f);
            object.audioID = VoiceID::Invalid;
        }
        const Entity entity = state.index.Take(uid);
        if (entity != Entity::Null() && entities.IsAlive(entity)) {
            // The instance is gone as of this frame, so say so on the entity
            // before it dies: anything running later in the frame (and the
            // socket-free mirror test) can read the removal off the registry
            // instead of asking the session what it just did.
            entities.Attach(entity, ProjectLightRemovalPending {});
            DespawnEntity(engine, entity);
        }
        state.controllerStates.erase(object.userId);
        state.objects.erase(found);
    }
    state.pendingRemovals.clear();

    const auto renderTime = std::chrono::steady_clock::now() - kNetworkSmoothDelay;
    for (auto& [uid, object]: state.objects) {
        (void)uid;
        object.renderPosition = object.position;
        object.renderRotation = object.rotationQuat;
        const bool locallyOwned = state.localPlayerUid != 0 && object.networkOwnerUid == state.localPlayerUid;
        if (locallyOwned) {
            object.physicsHistory.Clear();
            object.hasPhysicsSample = false;
        } else if (object.hasPhysicsSample) {
            object.physicsHistory.Sample(renderTime, object.renderPosition, object.renderRotation);
        }
        if (object.hasPhysicsSample) object.transformDirty = true;
    }
    for (auto& [uid, object]: state.objects) {
        (void)uid;
        if (object.removalPending) continue;
        if (IsPartClass(object.className)) SpawnOrUpdatePartEntity(engine, state, object);
        else if (IsLightClass(object.className)) SpawnOrUpdateLightEntity(engine, state, object);
    }

    UpdateRemoteCharacterParts(engine, state, 0.0f);
    SyncEnvironment(engine, state);
    UpdateSoundVoices(engine, state);

    if (state.snapshotComplete && !state.summaryLogged) {
        size_t partCount = 0;
        size_t lightCount = 0;
        size_t boundEntityCount = 0;
        size_t liveEntityCount = 0;
        for (const auto& [uid, object]: state.objects) {
            if (IsPartClass(object.className)) ++partCount;
            if (IsLightClass(object.className)) ++lightCount;
            const Entity bound = state.index.Find(uid);
            if (bound != Entity::Null()) {
                ++boundEntityCount;
                if (entities.IsAlive(bound)) ++liveEntityCount;
            }
        }
        ZHLN::COM::Log(engine.Logger(), "initial sync after snapshot: objects={} parts/meshParts={} lights={} boundEntities={} liveEntities={} workspaceUid={} localPlayerUid={} localRootPartUid={}", state.objects.size(), partCount, lightCount, boundEntityCount, liveEntityCount, state.workspaceUid, state.localPlayerUid, state.localRootPartUid);
        std::unordered_map<std::string, size_t> classCounts;
        for (const auto& [uid, object]: state.objects) {
            (void)uid;
            ++classCounts[object.className.empty() ? "<empty>" : object.className];
        }
        std::vector<std::pair<std::string, size_t>> distribution(classCounts.begin(), classCounts.end());
        std::sort(distribution.begin(), distribution.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.second != rhs.second ? lhs.second > rhs.second : lhs.first < rhs.first;
        });
        std::string classSummary;
        for (size_t i = 0; i < std::min<size_t>(distribution.size(), 12u); ++i) {
            if (!classSummary.empty()) classSummary += ", ";
            classSummary += std::format("{}={}", distribution[i].first, distribution[i].second);
        }
        ZHLN::COM::LoggerView{engine.Logger()}.Warn("parsed ClassName distribution: {}", classSummary.empty() ? "<none>" : classSummary);
        if (partCount == 0 && lightCount == 0) {
            ZHLN::COM::LoggerView{engine.Logger()}.Warn("snapshot completed but contains no renderable Part/MeshPart or light objects.");
        } else if (liveEntityCount == 0) {
            ZHLN::COM::LoggerView{engine.Logger()}.Warn("replicated renderables exist, but no live ECS render entities were created.");
        }
        state.summaryLogged = true;
    }
}
void SceneMirror::UpdateLocalController(COM::IEngine& engine, SceneMirrorState& state) {
    auto& entities = *engine.Entities();
    auto bindCamera = [&](Entity target) -> void {
        if (target == Entity::Null()) return;
        for (Entity camera: entities.EntitiesWith<Components::MainCameraTagComponent>()) {
            if (auto* rig = entities.Get<CameraRig::TargetComponent>(camera)) {
                if (rig->target != target) {
                    rig->target              = target;
                    rig->hasInitSmoothTarget = 0;
                }
            }
        }
    };
    auto destroyController = [&]() -> void {
        if (state.localControllerBody != Physics::CharacterID::Null()) {
            engine.Simulation()->DestroyBody(state.localControllerBody);
            state.localControllerBody = Physics::CharacterID::Null();
        }
        if (state.localController != Entity::Null() && entities.IsAlive(state.localController)) {
            DespawnEntity(engine, state.localController);
        }
        state.localController = Entity::Null();
        state.controllerForRootUid = 0;
    };

    if (state.serverAuthority) {
        if (state.localController != Entity::Null() || state.localControllerBody != Physics::CharacterID::Null()) destroyController();
        // The server drives the character, so the camera follows the root part's
        // entity; the mirror is only consulted for the uid, never for the entity.
        const Entity rootEntity = state.index.Find(state.localRootPartUid);
        if (rootEntity != Entity::Null() && entities.IsAlive(rootEntity)) bindCamera(rootEntity);
        return;
    }
    if (state.localRootPartUid == 0) {
        if (state.localController != Entity::Null() || state.localControllerBody != Physics::CharacterID::Null()) destroyController();
        return;
    }
    if (state.controllerForRootUid == state.localRootPartUid && state.localController != Entity::Null()
        && entities.IsAlive(state.localController) && state.localControllerBody != Physics::CharacterID::Null()) {
        const ReplicatedObject* humanoid = FindObject(state, state.localHumanoidUid);
        if (humanoid != nullptr) {
            if (auto* move = entities.Get<Character::MovementComponent>(state.localController)) {
                move->speed     = std::max(0.0f, humanoid->walkSpeed);
                move->jumpForce = std::max(0.0f, humanoid->jumpPower) * kLocalJumpScale;
            }
        }
        bindCamera(state.localController);
        return;
    }
    destroyController();

    const ReplicatedObject* rootPart = FindObject(state, state.localRootPartUid);
    if (rootPart == nullptr) return;

    auto&                        sim = *engine.Simulation();
    const Physics::Capsule       capsule {.halfHeight = 2.0f, .radius = 0.5f};
    const Physics::CharacterBody charRole {
        .position      = rootPart->position.Cast<double>(),
        .maxSlopeAngle = RadF {std::numbers::pi_v<float> / 4.0f},
    };
    const auto charRes        = sim.CreateBody(capsule, charRole);
    state.localControllerBody = charRes.HasValue() ? charRes.value : Physics::CharacterID::Null();
    if (state.localControllerBody == Physics::CharacterID::Null()) {
        ZHLN::COM::LoggerView{engine.Logger()}.Warn("Could not create the local player CharacterVirtual body.");
        return;
    }

    const Entity controller = entities.CreateEntity();
    const auto   matrix     = ComposeTransform(rootPart->position, rootPart->rotationQuat, {1.0f, 1.0f, 1.0f});
    entities.Attach(controller, Components::NameComponent {.name = String64("ProjectLightLocalController")});
    entities.Attach(controller, Components::TransformComponent {.position = rootPart->position, .rotation = rootPart->rotationQuat, .scale = FVec3::Splat(1.0f)});
    entities.Attach(controller, Components::WorldTransformComponent {.world = matrix, .previous = matrix});
    entities.Attach(controller, Components::CharacterBodyComponent {.bodyID = state.localControllerBody});
    Character::MovementComponent moveComp {.orientation = rootPart->rotationQuat, .prevOrientation = rootPart->rotationQuat};
    if (const ReplicatedObject* humanoid = FindObject(state, state.localHumanoidUid); humanoid != nullptr) {
        moveComp.speed     = std::max(0.0f, humanoid->walkSpeed);
        moveComp.jumpForce = std::max(0.0f, humanoid->jumpPower) * kLocalJumpScale;
    }
    Character::Attach(entities, controller, moveComp, Character::ControllerComponent {});
    state.localController      = controller;
    state.controllerForRootUid = state.localRootPartUid;

    bindCamera(controller);
}
void SceneMirror::UpdateLocalCharacterParts(COM::IEngine& engine, SceneMirrorState& state, float dt) {
    if (state.localController == Entity::Null() || state.localControllerBody == Physics::CharacterID::Null()) return;
    auto& entities = *engine.Entities();
    auto movement = entities.Get<Character::MovementComponent>(state.localController);
    if (!movement) return;

    // `bodyState` rather than `state`: in this file `state` is the session view.
    auto& sim = *engine.Simulation();
    const auto bodyStateResult = sim.BodyState(state.localControllerBody);
    if (!bodyStateResult.HasValue() || !bodyStateResult.value.valid) return;
    const auto& bodyState    = bodyStateResult.value;
    const FVec3 rootPosition = bodyState.currentPosition.Cast<float>();
    const Quat  rootRotation = movement->orientation.Normalized();
    const FVec3 velocity     = sim.CharacterVelocity(state.localControllerBody);
    (void)dt;

    const auto* actions  = engine.Actions();
    const bool  jumpDown = actions != nullptr && actions->KeyDown(KeyCode::Space, kPrimarySeat);
    if (jumpDown && !state.wasJumpDown && movement->isGrounded) state.jumpRemaining = 0.20f;
    state.wasJumpDown = jumpDown;
    state.jumpRemaining = std::max(0.0f, state.jumpRemaining - std::max(0.0f, dt));
    HumanoidState stateValue = HumanoidState::Idle;
    if (!movement->isGrounded) stateValue = state.jumpRemaining > 0.0f ? HumanoidState::Jumping : HumanoidState::Freefall;
    else if (std::abs(movement->inputX) + std::abs(movement->inputZ) > 0.05f) stateValue = HumanoidState::Running;

    const ReplicatedObject* humanoid = FindObject(state, state.localHumanoidUid);
    const float walkSpeed = humanoid ? humanoid->walkSpeed : movement->speed;
    for (auto& [uid, object]: state.objects) {
        if (!IsPartClass(object.className) || !IsPartOfCharacter(state.objects, uid, state.localCharacterUid)) continue;
        const FVec3 position = rootPosition + rootRotation * CharacterOffsetForName(object.name);
        const float poseAngle = IsCharacterLimb(object.name) ? LimbPoseAngle(object.name, stateValue, state.time, walkSpeed) : 0.0f;
        const Quat limbRotation = poseAngle == 0.0f ? rootRotation : rootRotation * Quat::FromAxisAngle(FVec3::UnitX(), ZHLN::RadF{poseAngle});
        object.linearVelocity = velocity;
        object.humanoidState = stateValue;
        WriteTransform(engine, object, state.index.Find(uid), position, limbRotation);
    }
}

} // namespace ZHLN::ProjectLight
