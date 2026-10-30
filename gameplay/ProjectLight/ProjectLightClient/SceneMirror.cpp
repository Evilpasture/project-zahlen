// gameplay/ProjectLight/ProjectLightClient/SceneMirror.cpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The passes that turn mirrored records into a scene. Moved here from
// ClientSession, which keeps the socket, the packet buffers and the session
// state; nothing in this file opens a socket, reads a packet or owns a session
// member. See SceneMirror.hpp for the split and the per-frame ordering.

#include <ProjectLightClient/SceneMirror.hpp>

#include <Camera/TargetCamera.hpp>
#include <CharacterController/CharacterComponents.hpp>
#include <Replication/ClassVocabulary.hpp>
#include <Zahlen/Audio/Audio.hpp>
#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math/Math3D.hpp>
#include <Zahlen/Engine/PrefabFactory.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/Physics/Physics.hpp>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <string_view>
#include <utility>
#include <vector>

namespace ZHLN::ProjectLight {
namespace {

// Bias toward responsiveness: keep a short remote playout delay to smooth
// 60 Hz arrivals without making moving props feel as far behind as 40 ms.
constexpr auto kNetworkSmoothDelay = std::chrono::milliseconds(20);
constexpr float kLocalJumpScale = 0.40435f; // sqrt(32 / project-light's 196.2 studs/s^2)

auto SameVec3(JPH::Vec3Arg lhs, JPH::Vec3Arg rhs) noexcept -> bool {
    return lhs.GetX() == rhs.GetX() && lhs.GetY() == rhs.GetY() && lhs.GetZ() == rhs.GetZ();
}
struct ResolvedDirectionalLight {
    JPH::Vec3 direction = JPH::Vec3(0.0f, 1.0f, 0.0f); // toward the source, as Zahlen expects
    JPH::Vec3 color = JPH::Vec3::sReplicate(1.0f);
    float intensity = 1.0f;
};
auto ProjectLightSunDirectionToSource(float timeOfDay, float latitude) noexcept -> JPH::Vec3 {
    // Match LightingService.GetSunDirection(yUp=true): X=east, Y=up, Z=north.
    constexpr float kDegreesToRadians = 0.01745329251994329577f;
    constexpr float kSolarDeclination = 23.44f * kDegreesToRadians;
    if (!std::isfinite(timeOfDay) || !std::isfinite(latitude)) return JPH::Vec3(0.0f, 1.0f, 0.0f);

    const float lat = latitude * kDegreesToRadians;
    const float hourAngle = (timeOfDay - 12.0f) * 15.0f * kDegreesToRadians;
    const float sinLat = std::sin(lat);
    const float cosLat = std::cos(lat);
    const float sinDec = std::sin(kSolarDeclination);
    const float cosDec = std::cos(kSolarDeclination);
    const float sinHour = std::sin(hourAngle);
    const float cosHour = std::cos(hourAngle);

    const JPH::Vec3 towardSource(
        -cosDec * sinHour,
        sinLat * sinDec + cosLat * cosDec * cosHour,
        cosLat * sinDec - sinLat * cosDec * cosHour
    );
    return towardSource.LengthSq() > 1.0e-8f ? towardSource.Normalized() : JPH::Vec3(0.0f, 1.0f, 0.0f);
}
auto ResolveDirectionalLight(const ReplicatedObject& light, const ReplicatedObject* lighting) noexcept -> ResolvedDirectionalLight {
    // ProjectLight's shaders use L = -sunDirection because that property is
    // the direction light rays travel. Zahlen's LightComponent.direction is L
    // itself: the direction from a surface toward the light source.
    JPH::Vec3 travelDirection = light.direction;
    if (light.isMain && lighting != nullptr) {
        travelDirection = -ProjectLightSunDirectionToSource(lighting->timeOfDay, lighting->latitude);
    }
    if (travelDirection.LengthSq() <= 1.0e-8f) travelDirection = JPH::Vec3(0.0f, -1.0f, 0.0f);
    travelDirection = travelDirection.Normalized();

    JPH::Vec3 color = light.color;
    float intensity = light.intensity;
    // Match ProjectLight's sun-to-moon handoff when the rays would point above
    // the horizon. The moon is antipodal, so convert the final travel vector
    // to the renderer's toward-source convention only after this switch.
    if (travelDirection.GetY() > 0.0f) {
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
auto CharacterOffsetForName(std::string_view name) noexcept -> JPH::Vec3 {
    if (name == "Head") return JPH::Vec3(0.0f, 1.5f, 0.0f);
    if (name == "Left Arm") return JPH::Vec3(-1.5f, 0.0f, 0.0f);
    if (name == "Right Arm") return JPH::Vec3(1.5f, 0.0f, 0.0f);
    if (name == "Left Leg") return JPH::Vec3(-0.5f, -2.0f, 0.0f);
    if (name == "Right Leg") return JPH::Vec3(0.5f, -2.0f, 0.0f);
    return JPH::Vec3::sZero();
}
auto IsCharacterLimb(std::string_view name) noexcept -> bool {
    return name == "Right Arm" || name == "Left Arm" || name == "Right Leg" || name == "Left Leg";
}
auto LimbPoseAngle(std::string_view partName, HumanoidState state, float time, float walkSpeed) noexcept -> float {
    if (partName == "Head" || partName == "Torso") return 0.0f;
    if (state == HumanoidState::Jumping || state == HumanoidState::Freefall) {
        return (partName == "Right Arm" || partName == "Left Arm") ? -JPH::JPH_PI : 0.0f;
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
void WriteTransform(Engine& engine, ReplicatedObject& object, Entity entity, JPH::Vec3 position, JPH::Quat rotation) {
    object.position = position;
    object.rotationQuat = rotation.Normalized();
    object.renderPosition = object.position;
    object.renderRotation = object.rotationQuat;
    object.rotationEulerDeg = Math::QuatToEulerDegrees(object.rotationQuat);
    object.transformDirty = true;
    if (entity == Entity::Null()) return;
    auto& registry = engine.GetRegistry();
    if (auto transform = registry.Get<Components::TransformComponent>(entity)) {
        transform->position = object.position;
        transform->rotation = object.rotationQuat;
    }
}

void WriteInterpolatedTransform(Engine& engine, ReplicatedObject& object, Entity entity, JPH::Vec3 position, JPH::Quat rotation) {
    // Keep the authoritative network pose intact; only the render pose is
    // delayed/interpolated. Physics snapshots use object.position/rotationQuat.
    object.renderPosition = position;
    object.renderRotation = rotation.Normalized();
    object.transformDirty = true;
    if (entity == Entity::Null()) return;
    auto& registry = engine.GetRegistry();
    if (auto transform = registry.Get<Components::TransformComponent>(entity)) {
        transform->position = object.renderPosition;
        transform->rotation = object.renderRotation;
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
void SpawnOrUpdatePartEntity(Engine& engine, SceneMirrorState& state, ReplicatedObject& object) {
    auto& registry = engine.GetRegistry();
    const JPH::Vec3 size = JPH::Vec3(
        std::clamp(std::abs(object.size.GetX()), 0.001f, 100000.0f),
        std::clamp(std::abs(object.size.GetY()), 0.001f, 100000.0f),
        std::clamp(std::abs(object.size.GetZ()), 0.001f, 100000.0f)
    );
    const bool createPhysics = ShouldCreatePhysics(state, object);
    const bool blendMaterial = object.transparency > 0.001f;
    Entity     entity        = state.index.Find(object.uid);
    const bool liveEntity    = entity != Entity::Null() && registry.IsAlive(entity)
                            && !registry.Get<Components::PendingDestroy>(entity).has_value();
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
        params.position         = JPH::RVec3(createPhysics ? object.position : object.renderPosition);
        params.rotation         = (createPhysics ? object.rotationQuat : object.renderRotation).Normalized();
        params.scale            = size;
        params.createPhysics    = createPhysics;
        params.isStaticPhysics  = object.anchored;
        params.physicsMask      = object.canCollide ? 0xFFFFFFFFu : 0u;
        params.color            = JPH::Vec4(1.0f, 1.0f, 1.0f, 1.0f);
        params.roughness        = object.roughness;
        params.metallic         = object.metallic;

        auto material = engine.GetRenderContext().CreateMaterial(blendMaterial ? ZHLN::MaterialDesc::Transparent() : ZHLN::MaterialDesc::Basic());
        if (material) {
            material->baseColorFactor = {1.0f, 1.0f, 1.0f, 1.0f};
            material->roughnessFactor = object.roughness;
            material->metallicFactor  = object.metallic;
            params.materialOverride   = *material;
        } else {
            Log("[ProjectLight] Could not create material for '{}': {}", object.name, material.error());
        }

        const JPH::Vec3 halfExtents = JPH::Vec3::sReplicate(0.5f);
        switch (object.shape) {
            case PartShape::Sphere:
            case PartShape::Head:
                entity = PrefabFactory::CreateSphere(engine, 0.5f, params);
                break;
            case PartShape::Cylinder:
                entity = PrefabFactory::CreateCylinder(engine, 0.5f, 1.0f, params);
                break;
            case PartShape::Cone:
                entity = PrefabFactory::CreateCone(engine, 0.5f, 1.0f, params);
                break;
            case PartShape::Cube:
            case PartShape::Wedge:
            case PartShape::CornerWedge:
            case PartShape::Capsule:
            default:
                // Wedge, CornerWedge, Capsule and MeshPart asset substitution are
                // intentionally represented by the nearest built-in primitive.
                entity = PrefabFactory::CreateBox(engine, halfExtents, params);
                break;
        }

        if (entity == Entity::Null() || !registry.IsAlive(entity)) {
            object.materialId = InvalidMaterialID;
            object.hasPhysicsBody = false;
            return;
        }

        // Bind, then write the replication components. From here on the entity
        // answers "which instance am I" and "who owns me" on its own; the index
        // is the only place the reverse question is answered.
        state.index.Bind(object.uid, entity);
        registry.Add(
            entity,
            ProjectLightInstanceComponent {.uid = object.uid},
            ProjectLightNetworkOwnerComponent {.ownerUid = object.networkOwnerUid},
            ProjectLightReplicaTag {}
        );

        registry.Patch<Components::NameComponent>(entity, [&](auto& name) { name.name = String64(object.name); });
        if (auto transform = registry.Get<Components::TransformComponent>(entity)) {
            transform->position = object.renderPosition;
            transform->rotation = object.renderRotation.Normalized();
            transform->scale    = size;
        }
        if (auto world = registry.Get<Components::WorldTransformComponent>(entity)) {
            const JPH::Mat44 matrix = Math::CreateTransform(object.renderPosition, object.renderRotation.Normalized(), size);
            world->world    = matrix;
            world->previous = matrix;
        }
        if (auto mesh = registry.Get<Components::MeshComponent>(entity)) object.materialId = mesh->materialAsset;
        object.hasPhysicsBody  = registry.Get<Components::PhysicsComponent>(entity).has_value();
        object.spawnedSize     = size;
        object.spawnedShape    = object.shape;
        object.spawnedAnchored = object.anchored;
        object.spawnedCanCollide = object.canCollide;
        object.spawnedOwnerUid = object.networkOwnerUid;
        object.spawnedBlend    = blendMaterial;
        object.visualDirty     = true;
        object.transformDirty  = false;
    }

    if (entity == Entity::Null() || !registry.IsAlive(entity)) return;

    // Ownership moves without a rebuild in the mirror's view (the rebuild above
    // covers the geometry case), so the component is refreshed on every pass.
    if (auto owner = registry.Get<ProjectLightNetworkOwnerComponent>(entity)) {
        owner->ownerUid = object.networkOwnerUid;
    }

    if (object.transformDirty) {
        if (auto transform = registry.Get<Components::TransformComponent>(entity)) {
            transform->position = object.renderPosition;
            transform->rotation = object.renderRotation.Normalized();
            transform->scale    = size;
        }
        object.transformDirty = false;
    }

    if (object.visualDirty && object.materialId != InvalidMaterialID) {
        if (auto material = engine.GetRenderContext().GetGPUMaterial(object.materialId)) {
            material->baseColorFactor = {object.color.GetX(), object.color.GetY(), object.color.GetZ(), 1.0f - object.transparency};
            material->emissiveFactor  = {object.color.GetX() * object.emission, object.color.GetY() * object.emission,
                                         object.color.GetZ() * object.emission, 1.0f};
            material->roughnessFactor = object.roughness;
            material->metallicFactor  = object.metallic;
            engine.GetRenderContext().RegisterGPUMaterial(object.materialId, *material);
        }
        registry.Patch<Components::PBRComponent>(entity, [&](auto& pbr) {
            pbr.roughness = object.roughness;
            pbr.metallic  = object.metallic;
        });
        registry.Patch<Components::NameComponent>(entity, [&](auto& name) { name.name = String64(object.name); });
        object.visualDirty = false;
    }
}
void SpawnOrUpdateLightEntity(Engine& engine, SceneMirrorState& state, ReplicatedObject& object) {
    auto& registry = engine.GetRegistry();
    const bool directional = object.className == "DirectionalLight";
    const ReplicatedObject* lighting = directional ? FindObject(state, state.lightingUid) : nullptr;
    const ResolvedDirectionalLight resolved = directional
        ? ResolveDirectionalLight(object, lighting)
        : ResolvedDirectionalLight {.direction = object.direction, .color = object.color, .intensity = object.intensity};
    const float rendererIntensity = object.enabled ? resolved.intensity * (directional ? 180.0f : 1.0f) : 0.0f;
    Entity      entity            = state.index.Find(object.uid);
    if (entity == Entity::Null() || !registry.IsAlive(entity) || registry.Get<Components::PendingDestroy>(entity).has_value()) {
        entity = registry.Create();
        const JPH::Mat44 matrix = Math::CreateTransform(object.position, object.rotationQuat, JPH::Vec3::sReplicate(1.0f));
        registry.Add(
            entity,
            ProjectLightInstanceComponent {.uid = object.uid},
            ProjectLightNetworkOwnerComponent {.ownerUid = object.networkOwnerUid},
            ProjectLightReplicaTag {},
            Components::NameComponent {.name = String64(object.name)},
            Components::TransformComponent {.position = object.position, .rotation = object.rotationQuat, .scale = JPH::Vec3::sReplicate(1.0f)},
            Components::WorldTransformComponent {.world = matrix, .previous = matrix},
            Components::LightComponent {
                .type = directional ? LightType::Sun : LightType::Point,
                .color = resolved.color,
                .intensity = rendererIntensity,
                .radius = object.sourceRadius,
                .direction = resolved.direction,
                .range = object.range
            },
            Components::SceneLightTagComponent {}
        );
        state.index.Bind(object.uid, entity);
        object.visualDirty = true;
    }

    if (directional && object.isMain && !registry.Get<Components::SunTagComponent>(entity)) {
        registry.Add(entity, Components::SunTagComponent {});
    } else if (directional && !object.isMain && registry.Get<Components::SunTagComponent>(entity)) {
        registry.Remove<Components::SunTagComponent>(entity);
    }

    if (auto name = registry.Get<Components::NameComponent>(entity)) name->name = String64(object.name);
    if (auto transform = registry.Get<Components::TransformComponent>(entity)) {
        transform->position = object.position;
        transform->rotation = object.rotationQuat;
    }
    if (auto light = registry.Get<Components::LightComponent>(entity)) {
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
void SyncEnvironment(Engine& engine, SceneMirrorState& state) {
    const ReplicatedObject* lighting = FindObject(state, state.lightingUid);
    if (lighting == nullptr) return;
    auto& registry = engine.GetRegistry();

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
    // pairs with exactly those numbers (Scene.hpp SceneEnvironment::exposure,
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

    for (Entity settings: registry.GetEntitiesWith<Components::GlobalSettingsTagComponent>()) {
        registry.Patch<Components::PostProcessSettingsComponent>(settings, [&](auto& post) {
            post.ambientExposure = ambientExposure;
            post.exposure        = kProjectLightExposure;
            if (lighting->proceduralSky) {
                post.skyZenith  = JPH::Vec4(0.003f * kProjectLightSkyScale, 0.008f * kProjectLightSkyScale, 0.020f * kProjectLightSkyScale, 1.0f);
                post.skyHorizon = JPH::Vec4(0.015f * kProjectLightSkyScale, 0.035f * kProjectLightSkyScale, 0.080f * kProjectLightSkyScale, 1.0f);
                post.skyGround  = JPH::Vec4(0.001f * kProjectLightSkyScale, 0.001f * kProjectLightSkyScale, 0.003f * kProjectLightSkyScale, 1.0f);
            } else {
                post.skyZenith  = JPH::Vec4(0.0f, 0.0f, 0.0f, 1.0f);
                post.skyHorizon = JPH::Vec4(0.0f, 0.0f, 0.0f, 1.0f);
                post.skyGround  = JPH::Vec4(0.0f, 0.0f, 0.0f, 1.0f);
            }
        });
    }
}
void UpdateRemoteCharacterParts(Engine& engine, SceneMirrorState& state, float dt) {
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

        JPH::Vec3 rootPosition {};
        JPH::Quat rootRotation {};
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
            const JPH::Vec3 offset = CharacterOffsetForName(object.name);
            const JPH::Vec3 position = rootPosition + rootRotation * offset;
            const float poseAngle = IsCharacterLimb(object.name) ? LimbPoseAngle(object.name, stateValue, state.time, walkSpeed) : 0.0f;
            const JPH::Quat limbRotation = poseAngle == 0.0f ? rootRotation : rootRotation * JPH::Quat::sRotation(JPH::Vec3::sAxisX(), poseAngle);
            WriteInterpolatedTransform(engine, object, state.index.Find(uid), position, limbRotation);
        }
    }
}
void UpdateSoundVoices(Engine& engine, SceneMirrorState& state) {
    for (auto& [uid, object]: state.objects) {
        (void)uid;
        if (object.className != "Sound") continue;
        auto& audio = engine.GetAudioContext();
        if (object.playing) {
            if (object.audioHandle == AudioHandle::Invalid || !audio.IsVoiceValid(object.audioHandle)) {
                if (object.soundId.empty()) continue;
                object.audioHandle = audio.CreateVoice(object.soundId, true, object.loops, object.volume);
                if (object.audioHandle == AudioHandle::Invalid) {
                    continue; // asset may not be present in the local Zahlen installation
                }
                audio.PlayVoice(object.audioHandle);
            } else {
                audio.SetVoiceVolume(object.audioHandle, object.volume);
                audio.SetVoiceLooping(object.audioHandle, object.loops);
            }
            const ReplicatedObject* parent = FindObject(state, object.parentUid);
            if (parent != nullptr) audio.SetVoicePosition(object.audioHandle, parent->position);
        } else if (object.audioHandle != AudioHandle::Invalid) {
            audio.StopVoice(object.audioHandle);
            object.audioHandle = AudioHandle::Invalid;
        }
    }
}
} // namespace

void SceneMirror::SyncSceneEntities(Engine& engine, SceneMirrorState& state) {
    auto& registry = engine.GetRegistry();

    for (uint64_t uid: state.pendingRemovals) {
        auto found = state.objects.find(uid);
        if (found == state.objects.end()) continue;
        ReplicatedObject& object = found->second;
        if (object.audioHandle != AudioHandle::Invalid) {
            engine.GetAudioContext().StopVoice(object.audioHandle);
            object.audioHandle = AudioHandle::Invalid;
        }
        const Entity entity = state.index.Take(uid);
        if (entity != Entity::Null() && registry.IsAlive(entity)) {
            // The instance is gone as of this frame, so say so on the entity
            // before it dies: anything running later in the frame (and the
            // socket-free mirror test) can read the removal off the registry
            // instead of asking the session what it just did.
            registry.Add<ProjectLightRemovalPending>(entity);
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
            (void)object.physicsHistory.Sample(renderTime, object.renderPosition, object.renderRotation);
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
                if (registry.IsAlive(bound)) ++liveEntityCount;
            }
        }
        Log("[ProjectLight][scene] initial sync after snapshot: objects={} parts/meshParts={} lights={} boundEntities={} liveEntities={} workspaceUid={} localPlayerUid={} localRootPartUid={}",
            state.objects.size(), partCount, lightCount, boundEntityCount, liveEntityCount, state.workspaceUid, state.localPlayerUid, state.localRootPartUid);
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
        Log("[ProjectLight][scene] parsed ClassName distribution: {}", classSummary.empty() ? "<none>" : classSummary);
        if (partCount == 0 && lightCount == 0) {
            LogWarning("[ProjectLight][scene] snapshot completed but contains no renderable Part/MeshPart or light objects.");
        } else if (liveEntityCount == 0) {
            LogWarning("[ProjectLight][scene] replicated renderables exist, but no live ECS render entities were created.");
        }
        state.summaryLogged = true;
    }
}
void SceneMirror::UpdateLocalController(Engine& engine, SceneMirrorState& state) {
    auto& registry = engine.GetRegistry();
    auto bindCamera = [&](Entity target) -> void {
        if (target == Entity::Null()) return;
        for (Entity camera: registry.GetEntitiesWith<Components::MainCameraTagComponent>()) {
            registry.Patch<CameraRig::TargetCameraComponent>(camera, [&](auto& rig) {
                if (rig.target != target) {
                    rig.target = target;
                    rig.hasInitSmoothTarget = 0;
                }
            });
        }
    };
    auto destroyController = [&]() -> void {
        if (state.localControllerBody != Physics::BodyHandle::Null()) {
            engine.GetPhysicsContext().DestroyBody(state.localControllerBody);
            state.localControllerBody = Physics::BodyHandle::Null();
        }
        if (state.localController != Entity::Null() && registry.IsAlive(state.localController)) {
            DespawnEntity(engine, state.localController);
        }
        state.localController = Entity::Null();
        state.controllerForRootUid = 0;
    };

    if (state.serverAuthority) {
        if (state.localController != Entity::Null() || state.localControllerBody != Physics::BodyHandle::Null()) destroyController();
        // The server drives the character, so the camera follows the root part's
        // entity; the mirror is only consulted for the uid, never for the entity.
        const Entity rootEntity = state.index.Find(state.localRootPartUid);
        if (rootEntity != Entity::Null() && registry.IsAlive(rootEntity)) bindCamera(rootEntity);
        return;
    }
    if (state.localRootPartUid == 0) {
        if (state.localController != Entity::Null() || state.localControllerBody != Physics::BodyHandle::Null()) destroyController();
        return;
    }
    if (state.controllerForRootUid == state.localRootPartUid && state.localController != Entity::Null()
        && registry.IsAlive(state.localController) && state.localControllerBody != Physics::BodyHandle::Null()) {
        const ReplicatedObject* humanoid = FindObject(state, state.localHumanoidUid);
        if (auto move = registry.Get<Character::MovementComponent>(state.localController); move && humanoid != nullptr) {
            move->speed = std::max(0.0f, humanoid->walkSpeed);
            move->jumpForce = std::max(0.0f, humanoid->jumpPower) * kLocalJumpScale;
        }
        bindCamera(state.localController);
        return;
    }
    destroyController();

    const ReplicatedObject* rootPart = FindObject(state, state.localRootPartUid);
    if (rootPart == nullptr) return;

    auto& physics = engine.GetPhysicsContext();
    Physics::CharacterParams params;
    params.shape = physics.GetOrCreateShape(Physics::ShapeType::Capsule, 2.0f, 0.5f);
    params.maxSlopeAngle = JPH::DegreesToRadians(45.0f);
    state.localControllerBody = physics.CreateCharacter(JPH::RVec3(rootPart->position), params);
    if (state.localControllerBody == Physics::BodyHandle::Null()) {
        Log("[ProjectLight] Could not create the local player CharacterVirtual body.");
        return;
    }

    registry.RegisterComponent<Character::MovementComponent>();
    registry.RegisterComponent<Character::InputComponent>();
    const Entity controller = registry.Create();
    const JPH::Mat44 matrix = Math::CreateTransform(rootPart->position, rootPart->rotationQuat, JPH::Vec3::sReplicate(1.0f));
    registry.Add(
        controller,
        Components::NameComponent {.name = String64("ProjectLightLocalController")},
        Components::TransformComponent {.position = rootPart->position, .rotation = rootPart->rotationQuat, .scale = JPH::Vec3::sReplicate(1.0f)},
        Components::WorldTransformComponent {.world = matrix, .previous = matrix},
        Components::PhysicsComponent {.physicsHandle = state.localControllerBody, .isStatic = false},
        Character::MovementComponent {.orientation = rootPart->rotationQuat, .prevOrientation = rootPart->rotationQuat},
        Character::InputComponent {}
    );
    state.localController = controller;
    state.controllerForRootUid = state.localRootPartUid;

    const ReplicatedObject* humanoid = FindObject(state, state.localHumanoidUid);
    if (auto move = registry.Get<Character::MovementComponent>(controller); move && humanoid != nullptr) {
        move->speed = std::max(0.0f, humanoid->walkSpeed);
        move->jumpForce = std::max(0.0f, humanoid->jumpPower) * kLocalJumpScale;
    }

    bindCamera(controller);
}
void SceneMirror::UpdateLocalCharacterParts(Engine& engine, SceneMirrorState& state, float dt) {
    if (state.localController == Entity::Null() || state.localControllerBody == Physics::BodyHandle::Null()) return;
    auto& registry = engine.GetRegistry();
    auto movement = registry.Get<Character::MovementComponent>(state.localController);
    if (!movement) return;

    // `bodyState` rather than `state`: in this file `state` is the session view.
    auto& physics = engine.GetPhysicsContext();
    const auto bodyStateResult = physics.BodyState(state.localControllerBody);
    if (!bodyStateResult || !bodyStateResult->valid) return;
    const auto& bodyState = *bodyStateResult;
    const JPH::Vec3 rootPosition = bodyState.currentPosition;
    const JPH::Quat rootRotation = movement->orientation.Normalized();
    const JPH::Vec3 velocity = physics.GetCharacterVelocity(state.localControllerBody);
    (void)dt;

    if (auto input = registry.GetSingleton<Components::InputStateComponent>()) {
        const bool jumpDown = input->IsKeyDown(static_cast<uint8_t>(KeyCode::Space));
        if (jumpDown && !state.wasJumpDown && movement->isGrounded) state.jumpRemaining = 0.20f;
        state.wasJumpDown = jumpDown;
    }
    state.jumpRemaining = std::max(0.0f, state.jumpRemaining - std::max(0.0f, dt));
    HumanoidState stateValue = HumanoidState::Idle;
    if (!movement->isGrounded) stateValue = state.jumpRemaining > 0.0f ? HumanoidState::Jumping : HumanoidState::Freefall;
    else if (std::abs(movement->inputX) + std::abs(movement->inputZ) > 0.05f) stateValue = HumanoidState::Running;

    const ReplicatedObject* humanoid = FindObject(state, state.localHumanoidUid);
    const float walkSpeed = humanoid ? humanoid->walkSpeed : movement->speed;
    for (auto& [uid, object]: state.objects) {
        if (!IsPartClass(object.className) || !IsPartOfCharacter(state.objects, uid, state.localCharacterUid)) continue;
        const JPH::Vec3 position = rootPosition + rootRotation * CharacterOffsetForName(object.name);
        const float poseAngle = IsCharacterLimb(object.name) ? LimbPoseAngle(object.name, stateValue, state.time, walkSpeed) : 0.0f;
        const JPH::Quat limbRotation = poseAngle == 0.0f ? rootRotation : rootRotation * JPH::Quat::sRotation(JPH::Vec3::sAxisX(), poseAngle);
        object.linearVelocity = velocity;
        object.humanoidState = stateValue;
        WriteTransform(engine, object, state.index.Find(uid), position, limbRotation);
    }
}

} // namespace ZHLN::ProjectLight
