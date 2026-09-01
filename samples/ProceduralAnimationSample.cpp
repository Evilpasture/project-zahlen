// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Camera.hpp>
#include <Zahlen/Clock.hpp>
#include <Zahlen/CommandLine.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/DefaultPreset.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Window.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <glTF/GLTFImporter.hpp>

// Optional extras/toolkit modules
import ZHLN.Locomotion;
import ZHLN.ProceduralAnimation;

// Jolt Physics
#include <Jolt/Jolt.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <numbers>
#include <utility>
#include <vector>

namespace {

inline constexpr float kAmbientExposure = 10.0f;
inline constexpr float kSunIntensity    = 28.0f;
inline const JPH::Vec3 kSunPosition {25.0f, 60.0f, 25.0f};
inline const JPH::Vec3 kSunColor {1.00f, 0.96f, 0.90f};
inline const JPH::Vec4 kSkyZenith {0.25f, 0.55f, 0.95f, 1.0f};
inline const JPH::Vec4 kSkyHorizon {0.70f, 0.85f, 1.00f, 1.0f};
inline const JPH::Vec4 kSkyGround {0.20f, 0.28f, 0.20f, 1.0f};

struct HiddenHeadMesh {
    ZHLN::Entity    entity;
    ZHLN::DrawFlags originalFlags = ZHLN::DrawFlags::None;
};

struct FirstPersonViewState {
    bool                                    enabled                 = false;
    bool                                    toggleKeyWasDown        = false;
    bool                                    thirdPersonSaved        = false;
    ZHLN::Entity                            cameraEntity            = ZHLN::Entity::Null();
    float                                   thirdPersonNearZ        = 0.1f;
    float                                   lookYawOffset           = 0.0f;
    float                                   lookPitchOffset         = 0.0f;
    float                                   eyeUpOffset             = 0.07f;
    float                                   eyeForwardOffset        = 0.08f;
    float                                   thirdPersonLookAtWeight = 0.85f;
    bool                                    lookAtWeightSaved       = false;
    ZHLN::Components::TargetCameraComponent thirdPersonCamera {};
    std::vector<HiddenHeadMesh>             headMeshes;
};

[[nodiscard]] auto CanonicalNodeName(std::string_view name) -> std::array<char, 96> {
    std::array<char, 96> result {};
    size_t               write = 0;
    for (char c: name) {
        if (write + 1 >= result.size()) {
            break;
        }
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            result[write++] = c;
        }
    }
    return result;
}

[[nodiscard]] bool IsHeadVisualName(std::string_view name) {
    const auto             canonical = CanonicalNodeName(name);
    const std::string_view value(canonical.data());
    return value == "head" || value == "defhead" || value == "orghead" || value.find("head") != std::string_view::npos ||
           value.find("face") != std::string_view::npos || value.find("visor") != std::string_view::npos || value.find("eye") != std::string_view::npos ||
           value.find("hair") != std::string_view::npos || value.find("ponytail") != std::string_view::npos || value.find("braid") != std::string_view::npos ||
           value.find("hat") != std::string_view::npos || value.find("beanie") != std::string_view::npos || value.find("helmet") != std::string_view::npos ||
           value == "cap" || value.find("glasses") != std::string_view::npos || value.find("goggles") != std::string_view::npos;
}

[[nodiscard]] bool IsNodeUnder(const ZHLN::ModelPrefab& prefab, int32_t node, int32_t ancestor) {
    for (size_t depth = 0; depth < prefab.nodes.size() && node >= 0 && node < static_cast<int32_t>(prefab.nodes.size()); ++depth) {
        if (node == ancestor) {
            return true;
        }
        node = prefab.nodes[static_cast<size_t>(node)].parentIndex;
    }
    return false;
}

[[nodiscard]] JPH::Mat44 GetPrefabNodeModelTransform(const ZHLN::ModelPrefab& prefab, int32_t node) {
    if (node < 0 || node >= static_cast<int32_t>(prefab.nodes.size())) {
        return JPH::Mat44::sIdentity();
    }
    JPH::Mat44 transform = prefab.nodes[static_cast<size_t>(node)].localTransform;
    int32_t    parent    = prefab.nodes[static_cast<size_t>(node)].parentIndex;
    for (size_t depth = 0; depth < prefab.nodes.size() && parent >= 0 && parent < static_cast<int32_t>(prefab.nodes.size()); ++depth) {
        transform = prefab.nodes[static_cast<size_t>(parent)].localTransform * transform;
        parent    = prefab.nodes[static_cast<size_t>(parent)].parentIndex;
    }
    return transform;
}

[[nodiscard]] bool HasHeadVisualAncestor(const ZHLN::ModelPrefab& prefab, int32_t node) {
    for (size_t depth = 0; depth < prefab.nodes.size() && node >= 0 && node < static_cast<int32_t>(prefab.nodes.size()); ++depth) {
        if (IsHeadVisualName(std::string_view(prefab.nodes[static_cast<size_t>(node)].name))) {
            return true;
        }
        node = prefab.nodes[static_cast<size_t>(node)].parentIndex;
    }
    return false;
}

[[nodiscard]] bool IsHeadDominatedSkin(const ZHLN::ModelPrefab& prefab, int32_t skeletonIndex) {
    if (skeletonIndex < 0 || skeletonIndex >= static_cast<int32_t>(prefab.skeletons.size())) {
        return false;
    }
    const auto& joints = prefab.skeletons[static_cast<size_t>(skeletonIndex)].joints;
    if (joints.empty()) {
        return false;
    }
    size_t headJoints  = 0;
    size_t bodyAnchors = 0;
    for (const ZHLN::Joint& joint: joints) {
        headJoints += IsHeadVisualName(std::string_view(joint.name)) ? 1u : 0u;
        const auto             canonical = CanonicalNodeName(std::string_view(joint.name));
        const std::string_view name(canonical.data());
        bodyAnchors += name.find("hips") != std::string_view::npos || name.find("chest") != std::string_view::npos ||
                               name.find("thigh") != std::string_view::npos || name.find("shin") != std::string_view::npos ||
                               name.find("foot") != std::string_view::npos || name.find("upperarm") != std::string_view::npos ||
                               name.find("forearm") != std::string_view::npos ?
                           1u :
                           0u;
    }
    return headJoints > 0 && bodyAnchors == 0;
}

void CollectFirstPersonHeadMeshes(
    ZHLN::ECS::Registry&          registry,
    const ZHLN::ModelPrefab&      prefab,
    std::span<const ZHLN::Entity> visualParts,
    FirstPersonViewState&         state
) {
    int32_t headNode = -1;
    int32_t neckNode = -1;
    for (size_t node = 0; node < prefab.nodes.size(); ++node) {
        const auto             canonical = CanonicalNodeName(std::string_view(prefab.nodes[node].name));
        const std::string_view name(canonical.data());
        if (headNode < 0 && (name == "head" || name == "defhead" || name == "orghead")) {
            headNode = static_cast<int32_t>(node);
        }
        if (neckNode < 0 && (name == "neck" || name == "defneck" || name == "orgneck")) {
            neckNode = static_cast<int32_t>(node);
        }
    }
    const JPH::Vec3 headPosition = GetPrefabNodeModelTransform(prefab, headNode).GetTranslation();
    const JPH::Vec3 neckPosition = GetPrefabNodeModelTransform(prefab, neckNode).GetTranslation();
    const float     headScale    = headNode >= 0 && neckNode >= 0 ? std::max((headPosition - neckPosition).Length(), 0.06f) : 0.12f;

    state.headMeshes.clear();
    for (ZHLN::Entity entity: visualParts) {
        auto* mesh = registry.Get<ZHLN::Components::MeshComponent>(entity);
        if (mesh == nullptr || mesh->nodeIndex < 0 || mesh->nodeIndex >= static_cast<int32_t>(prefab.nodes.size())) {
            continue;
        }
        const bool  underHead       = headNode >= 0 && IsNodeUnder(prefab, mesh->nodeIndex, headNode);
        const auto* skeletalMesh    = registry.Get<ZHLN::Components::SkeletalMeshComponent>(entity);
        const bool  headSkin        = skeletalMesh != nullptr && IsHeadDominatedSkin(prefab, skeletalMesh->skeletonIndex);
        bool        compactNearHead = false;
        for (const ZHLN::ModelPart& part: prefab.parts) {
            if (part.nodeIndex != mesh->nodeIndex) {
                continue;
            }
            const JPH::Mat44 model = GetPrefabNodeModelTransform(prefab, part.nodeIndex) * part.localTransform;
            const JPH::Vec3  localMin(part.localMin[0], part.localMin[1], part.localMin[2]);
            const JPH::Vec3  localMax(part.localMax[0], part.localMax[1], part.localMax[2]);
            const JPH::Vec3  localCenter     = (localMin + localMax) * 0.5f;
            const JPH::Vec3  halfExtent      = (localMax - localMin) * 0.5f;
            const JPH::Vec3  center          = model.Multiply3x3(localCenter) + model.GetTranslation();
            const float      radius          = model.Multiply3x3(halfExtent).Length();
            const float      surfaceDistance = std::max((center - headPosition).Length() - radius, 0.0f);
            compactNearHead                  = headNode >= 0 && radius <= headScale * 2.8f && surfaceDistance <= headScale * 1.8f &&
                                               center.GetY() >= neckPosition.GetY() - headScale * 0.15f;
            if (compactNearHead) {
                break;
            }
        }
        if (underHead || HasHeadVisualAncestor(prefab, mesh->nodeIndex) || headSkin || compactNearHead) {
            state.headMeshes.push_back({.entity = entity, .originalFlags = mesh->flags});
        }
    }
    ZHLN::Log("[Sample] First-person head hide set contains {} mesh parts.", state.headMeshes.size());
}

void SetFirstPersonMode(ZHLN::Engine& engine, ZHLN::Entity player, FirstPersonViewState& state, bool enabled) {
    auto& registry       = engine.GetRegistry();
    auto  cameraEntities = registry.GetEntitiesWith<ZHLN::Components::MainCameraTagComponent>();
    if (cameraEntities.empty()) {
        return;
    }
    state.cameraEntity = cameraEntities[0];

    if (enabled) {
        auto* targetCamera = registry.Get<ZHLN::Components::TargetCameraComponent>(state.cameraEntity);
        if (targetCamera == nullptr) {
            return;
        }
        state.thirdPersonCamera = *targetCamera;
        state.thirdPersonNearZ  = engine.GetCamera().nearZ;
        state.thirdPersonSaved  = true;
        state.lookYawOffset     = 0.0f;
        state.lookPitchOffset   = 0.0f;
        registry.Remove<ZHLN::Components::FreeCamTagComponent>(state.cameraEntity);
        registry.Remove<ZHLN::Components::TargetCameraComponent>(state.cameraEntity);
        if (auto* lookAt = registry.Get<ZHLN::ProceduralLookAtComponent>(player)) {
            state.thirdPersonLookAtWeight = lookAt->weight;
            state.lookAtWeightSaved       = true;
            lookAt->weight                = 0.0f;
        }
        engine.GetCamera().fov   = 75.0f;
        engine.GetCamera().nearZ = 0.03f;
    } else if (state.thirdPersonSaved) {
        ZHLN::Components::TargetCameraComponent restored = state.thirdPersonCamera;
        restored.hasInitSmoothTarget                     = 0;
        if (auto* targetCamera = registry.Get<ZHLN::Components::TargetCameraComponent>(state.cameraEntity)) {
            *targetCamera = restored;
        } else {
            registry.Add(state.cameraEntity, std::move(restored));
        }
        engine.GetCamera().yaw   = state.thirdPersonCamera.yaw;
        engine.GetCamera().pitch = state.thirdPersonCamera.pitch;
        engine.GetCamera().fov   = state.thirdPersonCamera.fov;
        engine.GetCamera().nearZ = state.thirdPersonNearZ;
        if (state.lookAtWeightSaved) {
            if (auto* lookAt = registry.Get<ZHLN::ProceduralLookAtComponent>(player)) {
                lookAt->weight = state.thirdPersonLookAtWeight;
            }
            state.lookAtWeightSaved = false;
        }
        state.thirdPersonSaved = false;
    }

    registry.Patch<ZHLN::FirstPersonVisibilityComponent>(player, [&](auto& visibility) -> auto {
        visibility.eyeOffsetModel    = JPH::Vec3(0.0f, state.eyeUpOffset, state.eyeForwardOffset);
        visibility.lookYawDegrees    = state.lookYawOffset;
        visibility.lookPitchDegrees  = state.lookPitchOffset;
        visibility.enabled           = enabled;
        visibility.hideHead          = true;
        visibility.hideHair          = true;
        visibility.cameraInitialized = false;
    });
    for (const HiddenHeadMesh& hidden: state.headMeshes) {
        if (auto* mesh = registry.Get<ZHLN::Components::MeshComponent>(hidden.entity)) {
            mesh->flags = enabled ? hidden.originalFlags | ZHLN::DrawFlags::Hidden : hidden.originalFlags;
        }
    }
    state.enabled = enabled;
    ZHLN::Log("[Sample] Camera mode: {}.", enabled ? "FIRST PERSON / HEAD-CONSTRAINED FULL BODY" : "THIRD PERSON / ORBIT");
}

[[nodiscard]] bool EnvironmentFlag(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return false;
    }
    const std::string_view text(value);
    return text != "0" && text != "false" && text != "FALSE" && text != "off" && text != "OFF";
}

[[nodiscard]] float EnvironmentFloat(const char* name, float fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    char*       end    = nullptr;
    const float parsed = std::strtof(value, &end);
    return end != value ? parsed : fallback;
}

auto BuildProceduralArena(ZHLN::Engine& engine) -> void {
    auto& reg = engine.GetRegistry();

    // 1. Configure Post-Processing Atmosphere via ECS Patch
    for (ZHLN::Entity e: reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>()) {
        reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(e, [](auto& pp) -> auto {
            pp.ambientExposure = kAmbientExposure;
            pp.skyZenith       = kSkyZenith;
            pp.skyHorizon      = kSkyHorizon;
            pp.skyGround       = kSkyGround;
        });
    }

    // 2. Terrain (220m procedural rolling landscape)
    ZHLN::CreativeWorksFactory::CreateTerrain(
        engine, 128, 220.0f, 12.0f, ZHLN::CreativeWorksFactory::TerrainType::Default,
        ZHLN::CreativeWorksFactory::SpawnParams {.position = {0.0, 0.0, 0.0}, .createPhysics = true, .isStaticPhysics = true, .roughness = 0.80f}
    );

    // 3. Center Platform
    ZHLN::CreativeWorksFactory::CreateBox(
        engine, JPH::Vec3(10.0f, 0.50f, 10.0f),
        ZHLN::CreativeWorksFactory::SpawnParams {
            .position = {0.0, 0.50, 0.0}, .createPhysics = true, .isStaticPhysics = true, .roughness = 0.50f, .color = {0.32f, 0.34f, 0.38f, 1.0f}
        }
    );

    // 4. 30-Degree Grounding Test Ramp
    ZHLN::CreativeWorksFactory::CreateBox(
        engine, JPH::Vec3(4.5f, 0.18f, 2.2f),
        ZHLN::CreativeWorksFactory::SpawnParams {
            .position        = {-14.0, 2.75, 5.0},
            .rotation        = JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), JPH::DegreesToRadians(30.0f)),
            .createPhysics   = true,
            .isStaticPhysics = true,
            .roughness       = 0.55f,
            .color           = {0.25f, 0.50f, 0.72f, 1.0f}
        }
    );

    // 5. Stepping Stones
    for (int i = 0; i < 6; ++i) {
        float stepHeight = 0.20f + (static_cast<float>(i) * 0.10f);
        ZHLN::CreativeWorksFactory::CreateBox(
            engine, JPH::Vec3(1.0f, stepHeight * 0.5f, 1.0f),
            ZHLN::CreativeWorksFactory::SpawnParams {
                .position        = {12.0 + (static_cast<double>(i) * 2.5), static_cast<double>(stepHeight * 0.5f), -4.0},
                .createPhysics   = true,
                .isStaticPhysics = true,
                .roughness       = 0.45f,
                .color           = {0.65f, 0.40f, 0.20f, 1.0f}
            }
        );
    }

    // 6. Obstacle Pillars
    struct PillarDesc {
        float     x, z, width, height;
        JPH::Vec4 color;
    };
    const std::array<PillarDesc, 4> pillars = {
        {{.x = -14.0f, .z = -8.0f, .width = 1.5f, .height = 5.0f, .color = {0.35f, 0.45f, 0.60f, 1.0f}},
         {.x = -18.0f, .z = 4.0f, .width = 2.0f, .height = 7.5f, .color = {0.30f, 0.40f, 0.55f, 1.0f}},
         {.x = 18.0f, .z = 12.0f, .width = 2.2f, .height = 9.0f, .color = {0.25f, 0.35f, 0.50f, 1.0f}},
         {.x = 14.0f, .z = -16.0f, .width = 2.0f, .height = 8.0f, .color = {0.28f, 0.38f, 0.52f, 1.0f}}}
    };
    for (const auto& p: pillars) {
        ZHLN::CreativeWorksFactory::CreateBox(
            engine, JPH::Vec3(p.width * 0.5f, p.height * 0.5f, p.width * 0.5f),
            ZHLN::CreativeWorksFactory::SpawnParams {
                .position        = {static_cast<double>(p.x), static_cast<double>(p.height * 0.5f), static_cast<double>(p.z)},
                .createPhysics   = true,
                .isStaticPhysics = true,
                .color           = p.color
            }
        );
    }

    // 7. Directional Sunlight via Variadic Create
    reg.Create(
        ZHLN::Components::NameComponent {.name = ZHLN::String64("SunLight")}, ZHLN::Components::TransformComponent {.position = kSunPosition},
        ZHLN::Components::LightComponent {
            .type = ZHLN::LightType::Sun, .color = kSunColor, .intensity = kSunIntensity, .direction = JPH::Vec3(0.45f, 1.00f, 0.30f).Normalized()
        }
    );
}

void SetHandgunVisibility(ZHLN::ECS::Registry& registry, ZHLN::Entity handgunRoot, bool visible) {
    for (ZHLN::Entity e: registry.GetEntitiesWith<ZHLN::Components::MeshComponent>()) {
        const auto* hier = registry.Get<ZHLN::Components::HierarchyComponent>(e);
        if (hier != nullptr && hier->parent == handgunRoot) {
            registry.Patch<ZHLN::Components::MeshComponent>(e, [visible](auto& mesh) {
                if (visible) {
                    mesh.flags &= ~ZHLN::DrawFlags::Hidden;
                } else {
                    mesh.flags |= ZHLN::DrawFlags::Hidden;
                }
            });
        }
    }
}

auto CreateTestHandgun(ZHLN::Engine& engine, ZHLN::Entity player, float itemScale) -> ZHLN::Entity {
    auto&              reg     = engine.GetRegistry();
    const float        scale   = std::clamp(itemScale, 0.10f, 4.0f);
    const ZHLN::Entity handgun = reg.Create(
        ZHLN::Components::NameComponent {.name = ZHLN::String64("ProceduralTestHandgun")}, ZHLN::Components::TransformComponent {},
        ZHLN::Components::WorldTransformComponent {}, ZHLN::Components::HierarchyComponent {.parent = player}
    );

    auto addPart = [&](std::string_view name, JPH::Vec3Arg halfExtents, JPH::Vec3Arg localPosition, JPH::QuatArg localRotation, JPH::Vec4Arg color,
                       float metallic) {
        ZHLN::CreativeWorksFactory::MaterialDesc materialDesc;
        materialDesc.metallic         = metallic;
        materialDesc.roughness        = 0.32f;
        materialDesc.baseColor        = {color.GetX(), color.GetY(), color.GetZ(), color.GetW()};
        const ZHLN::Material material = ZHLN::CreativeWorksFactory::CreateMaterial(engine.GetRenderContext(), materialDesc).value_or(ZHLN::Material {});
        const ZHLN::Entity   part     = ZHLN::CreativeWorksFactory::CreateBox(
            engine, JPH::Vec3(halfExtents) * scale,
            ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.0, 0.0), .createPhysics = false, .materialOverride = material}
        );
        reg.Add(part, ZHLN::Components::HierarchyComponent {.parent = handgun});
        reg.Patch<ZHLN::Components::TransformComponent>(part, [&](auto& transform) -> auto {
            transform.position = JPH::Vec3(localPosition) * scale;
            transform.rotation = localRotation;
        });
        reg.Patch<ZHLN::Components::NameComponent>(part, [&](auto& component) -> auto { component.name = ZHLN::String64(name); });
    };

    const JPH::Vec4 darkMetal(0.10f, 0.12f, 0.15f, 1.0f);
    const JPH::Vec4 gripColor(0.20f, 0.12f, 0.08f, 1.0f);
    addPart("Handgun_Slide", JPH::Vec3(0.075f, 0.055f, 0.22f), JPH::Vec3(0.0f, 0.025f, 0.08f), JPH::Quat::sIdentity(), darkMetal, 0.90f);
    addPart(
        "Handgun_Barrel", JPH::Vec3(0.035f, 0.035f, 0.24f), JPH::Vec3(0.0f, 0.015f, 0.11f), JPH::Quat::sIdentity(), JPH::Vec4(0.05f, 0.06f, 0.07f, 1.0f), 1.0f
    );
    addPart(
        "Handgun_Grip", JPH::Vec3(0.060f, 0.135f, 0.055f), JPH::Vec3(0.0f, -0.145f, -0.065f),
        JPH::Quat::sRotation(JPH::Vec3::sAxisX(), JPH::DegreesToRadians(-10.0f)), gripColor, 0.35f
    );
    addPart("Handgun_TriggerGuard", JPH::Vec3(0.045f, 0.035f, 0.055f), JPH::Vec3(0.0f, -0.070f, 0.015f), JPH::Quat::sIdentity(), darkMetal, 0.80f);
    addPart(
        "Handgun_Sight", JPH::Vec3(0.018f, 0.018f, 0.045f), JPH::Vec3(0.0f, 0.095f, 0.02f), JPH::Quat::sIdentity(), JPH::Vec4(0.75f, 0.18f, 0.08f, 1.0f), 0.20f
    );
    return handgun;
}

[[nodiscard]] auto BuildTestHandgunHandling(ZHLN::Entity handgun, float itemScale) -> ZHLN::Animation::ItemHandlingComponent {
    ZHLN::Animation::ItemHandlingComponent handling;
    const float                            scale  = std::clamp(itemScale, 0.10f, 4.0f);
    const auto                             scaled = [scale](JPH::Vec3Arg value) { return JPH::Vec3(value) * scale; };
    handling.driverMode                           = ZHLN::Animation::ItemDriverMode::BodyMounted;
    handling.itemEntity                           = handgun;
    handling.hipLocalOffset                       = JPH::Mat44::sTranslation(scaled(JPH::Vec3(-0.24f, -0.32f, 0.08f)));
    handling.aimLocalOffset                       = JPH::Mat44::sTranslation(scaled(JPH::Vec3(0.0f, -0.20f, 0.30f)));
    handling.aimProgress                          = 1.0f;
    handling.sway.massKg                          = 1.1f;
    handling.sway.stiffness                       = 115.0f;
    handling.sway.damping                         = 0.90f;
    handling.avoidance                            = {.probeDistance = 0.55f * scale, .probeRadius = 0.08f * scale, .pushbackScale = 0.80f, .tiltScale = 0.35f};
    handling.grips[0]                             = {
        .assignedLimb      = ZHLN::CharacterBone::HandR,
        .orientationMode   = ZHLN::Animation::GripOrientationMode::AutomaticHanded,
        .localTransform    = JPH::Mat44::sTranslation(scaled(JPH::Vec3(-0.055f, -0.13f, -0.055f))),
        .ikWeight          = 0.0f,
        .evaluatedIKWeight = 0.0f,
        .rotationWeight    = 0.90f,
        .grasp             = {
            .shape        = ZHLN::Animation::GraspShape::TriggerGrip,
            .curlAxisMode = ZHLN::Animation::FingerCurlAxisMode::AutomaticPalm,
            .gripRadius   = 0.022f * scale,
            .tightness    = 0.90f,
            .triggerCurl  = 0.32f
        },
    };
    handling.grips[1] = {
        .assignedLimb      = ZHLN::CharacterBone::HandL,
        .orientationMode   = ZHLN::Animation::GripOrientationMode::AutomaticHanded,
        .localTransform    = JPH::Mat44::sTranslation(scaled(JPH::Vec3(0.060f, -0.070f, 0.015f))),
        .ikWeight          = 0.0f,
        .evaluatedIKWeight = 0.0f,
        .rotationWeight    = 0.80f,
        .grasp             = {
            .shape        = ZHLN::Animation::GraspShape::Cylinder,
            .curlAxisMode = ZHLN::Animation::FingerCurlAxisMode::AutomaticPalm,
            .gripRadius   = 0.030f * scale,
            .tightness    = 0.82f
        },
    };
    handling.gripCount          = 2;
    handling.shoulderLeadWeight = 0.18f;
    handling.torsoReachWeight   = 0.18f;
    return handling;
}

/**
 * @brief Attaches a loaded GLB model or generates a procedural fallback rig.
 */
auto AttachCharacterRig(
    ZHLN::Engine&                          engine,
    ZHLN::Entity                           player,
    std::string_view                       glbPath,
    ZHLN::ModelPrefab*                     prefab,
    ZHLN::Animation::ItemHandlingComponent itemHandling,
    FirstPersonViewState&                  viewState
) -> void {
    auto& reg = engine.GetRegistry();

    ZHLN::ProceduralLocomotionComponent locomotion {
        .strideLength = 1.40f,
        .stepHeight   = 0.22f,
        .legReach     = 0.83f,
    };
    ZHLN::HairStrandsComponent      hair {};
    ZHLN::ProceduralLookAtComponent lookAt {
        .targetWorldPos = JPH::Vec3(0.0f, 2.0f, 4.0f),
        .weight         = 0.85f,
        .maxAngleDeg    = 70.0f,
    };
    const char*                              interpolationValue = std::getenv("ZHLN_POSE_INTERPOLATION");
    const bool                               useBicubic         = interpolationValue != nullptr && std::string_view(interpolationValue) == "bicubic";
    ZHLN::ProceduralAnimationConfigComponent animationConfig {
        .poseInterpolation       = useBicubic ? ZHLN::PoseInterpolationMode::Bicubic : ZHLN::PoseInterpolationMode::SpringDamper,
        .springStiffness         = EnvironmentFloat("ZHLN_SPRING_STIFFNESS", 2500.0f),
        .springDampingFactor     = EnvironmentFloat("ZHLN_SPRING_DAMPING_FACTOR", 0.90f),
        .bicubicTension          = EnvironmentFloat("ZHLN_BICUBIC_TENSION", 0.0f),
        .legIKWeight             = EnvironmentFloat("ZHLN_LEG_IK_WEIGHT", 0.65f),
        .pelvisDropWeight        = EnvironmentFloat("ZHLN_PELVIS_DROP_WEIGHT", 1.0f),
        .maxFootHeightCorrection = EnvironmentFloat("ZHLN_MAX_FOOT_HEIGHT_CORRECTION", 0.18f),
        .maxLegExtension         = EnvironmentFloat("ZHLN_MAX_LEG_EXTENSION", 0.98f),
        .maxIKBodyTiltDegrees    = EnvironmentFloat("ZHLN_MAX_IK_BODY_TILT_DEGREES", 10.0f),
        .maxAnkleSidewaysDegrees = EnvironmentFloat("ZHLN_MAX_ANKLE_SIDEWAYS_DEGREES", 15.0f),
        .maxAnkleForwardDegrees  = EnvironmentFloat("ZHLN_MAX_ANKLE_FORWARD_DEGREES", 35.0f),
        .enableGait              = !EnvironmentFlag("ZHLN_DISABLE_GAIT"),
        .enableGravityBounce     = !EnvironmentFlag("ZHLN_DISABLE_GRAVITY_BOUNCE"),
        .enableLegIK             = !EnvironmentFlag("ZHLN_DISABLE_IK"),
        .worldLockFeet           = EnvironmentFlag("ZHLN_WORLD_LOCK_FEET"),
        .enableAccelerationTilt  = !EnvironmentFlag("ZHLN_DISABLE_ACCELERATION_TILT"),
        .enableUpperBody         = !EnvironmentFlag("ZHLN_DISABLE_UPPER_BODY"),
        .enableSecondaryMotion   = !EnvironmentFlag("ZHLN_DISABLE_SECONDARY_MOTION"),
        .enforceFootAttachments  = false,
        .authoredPoseOnly        = EnvironmentFlag("ZHLN_AUTHORED_POSE_ONLY") || EnvironmentFlag("ZHLN_KEYFRAME_ONLY"),
    };
    ZHLN::Log(
        "[Sample] Pose interpolation: {} (stiffness={}, damping factor={}, bicubic tension={}).", useBicubic ? "bicubic" : "spring-damper",
        animationConfig.springStiffness, animationConfig.springDampingFactor, animationConfig.bicubicTension
    );
    ZHLN::Log(
        "[Sample] Procedural layers: gait={}, gravity bounce={}, leg IK={}, acceleration tilt={}, upper body={}, secondary motion={}.",
        animationConfig.enableGait, animationConfig.enableGravityBounce, animationConfig.enableLegIK, animationConfig.enableAccelerationTilt,
        animationConfig.enableUpperBody, animationConfig.enableSecondaryMotion
    );
    ZHLN::Log(
        "[Sample] Leg IK weight={}; authored X/Z=true; world lock={}; max height correction={}; pelvis-drop weight={}.",
        animationConfig.enableLegIK ? animationConfig.legIKWeight : 0.0f, animationConfig.worldLockFeet, animationConfig.maxFootHeightCorrection,
        animationConfig.pelvisDropWeight
    );
    ZHLN::Log(
        "[Sample] IK limits: extension={}, body tilt={} deg, ankle sideways={} deg, ankle forward={} deg.", animationConfig.maxLegExtension,
        animationConfig.maxIKBodyTiltDegrees, animationConfig.maxAnkleSidewaysDegrees, animationConfig.maxAnkleForwardDegrees
    );
    if (animationConfig.authoredPoseOnly) {
        ZHLN::Log("[Sample] Authored-pose-only isolation enabled; all procedural layers are bypassed.");
    } else if (!animationConfig.enableLegIK) {
        ZHLN::Log("[Sample] Leg IK disabled; gait/keyframe layers remain active.");
    }

    if (prefab != nullptr) {
        ZHLN::Log("[Sample] GLB model '{}' loaded successfully. Instantiating visual parts...", glbPath);

        int32_t idleTrack = ZHLN::FindAnimationTrack(*prefab, "idle");
        if (idleTrack < 0 && !prefab->animations.empty()) {
            idleTrack = 0;
        }
        const int32_t walkTrack = ZHLN::FindAnimationTrack(*prefab, "walk");
        const int32_t runTrack  = ZHLN::FindAnimationTrack(*prefab, "run");
        if (idleTrack >= 0) {
            const auto& clip = prefab->animations[static_cast<size_t>(idleTrack)];
            ZHLN::Log("[Sample] Selected idle track {}: '{}' (duration={}, channels={}).", idleTrack, clip.name, clip.duration, clip.channels.size());
            ZHLN::Log("[Sample] Locomotion tracks: idle={}, walk={}, run={}.", idleTrack, walkTrack, runTrack);
        } else {
            ZHLN::Log("[Sample] WARNING: '{}' contains no authored animation track; bind pose will be shown.", glbPath);
        }

        // Instantiate the visual hierarchy without physics colliders. A prefab
        // can emit one root, one entity per part, and at most one emissive VPL
        // per part. Size the output dynamically: InstantiatePrefab returns the
        // total number spawned even when the caller's output span is smaller.
        const size_t              outputCapacity = 1 + prefab->parts.size() * 2;
        std::vector<ZHLN::Entity> parts(outputCapacity);
        const uint32_t            count = ZHLN::CreativeWorksFactory::InstantiatePrefab(
            engine, *prefab, ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 1.20, 0.0), .createPhysics = false, .isAnimated = true},
            parts.data(), static_cast<uint32_t>(parts.size())
        );
        const uint32_t writtenCount = std::min(count, static_cast<uint32_t>(parts.size()));

        // Re-parent every returned visual mesh part directly under the player
        // CharacterVirtual entity. Emissive VPLs have no hierarchy and are
        // safely ignored by Patch.
        for (uint32_t i = 1; i < writtenCount; ++i) {
            reg.Patch<ZHLN::Components::HierarchyComponent>(parts[i], [&](auto& hier) -> auto { hier.parent = player; });
        }
        if (writtenCount > 1) {
            CollectFirstPersonHeadMeshes(reg, *prefab, std::span<const ZHLN::Entity>(parts.data() + 1, writtenCount - 1), viewState);
        }

        // Clean up the redundant prefab container root (parts[0]).
        if (writtenCount > 0) {
            reg.Destroy(parts[0]);
        }

        // Variadic component registration: AnimatorComponent triggers RigBoneMap discovery
        reg.Add(
            player,
            ZHLN::Components::AnimatorComponent {
                .currentTrackIdx = idleTrack,
                .currentLoop     = true,
                .prefab          = prefab,
            },
            ZHLN::ProceduralLocomotionTracksComponent {
                .idleTrack = idleTrack,
                .walkTrack = walkTrack,
                .runTrack  = runTrack,
            },
            ZHLN::Components::KinematicPoseOverrideComponent {}, ZHLN::RigBoneMap {}, // Initialized lazily on frame 0 by the optional subsystem
            std::move(locomotion), std::move(hair), std::move(lookAt), animationConfig, ZHLN::FirstPersonVisibilityComponent {}, std::move(itemHandling)
        );
    } else {
        ZHLN::Log("[Sample] Notice: '{}' not found. Falling back to in-memory procedural rig.", glbPath);

        ZHLN::RigBoneMap proceduralRig {};
        ZHLN::BuildStandardProceduralRig(proceduralRig);

        reg.Add(
            player, ZHLN::Components::KinematicPoseOverrideComponent {}, std::move(proceduralRig), std::move(locomotion), std::move(hair), std::move(lookAt),
            animationConfig, ZHLN::FirstPersonVisibilityComponent {}, std::move(itemHandling)
        );
    }
}

} // namespace

auto main(int argc, char* argv[]) -> int {
    auto optionsRes = ZHLN::HandleCommandLine(std::span(argv, static_cast<size_t>(argc)));
    if (!optionsRes) {
        return EXIT_FAILURE;
    }
    const auto& options = optionsRes.value();

    if (options.helpRequested || options.versionRequested || options.printGraphRequested) {
        return EXIT_SUCCESS;
    }

    ZHLN::SetLogLevel(options.logLevel);
    ZHLN::SetupSignalHandler();
    ZHLN::TaskSystem::Init();
    ZHLN::DefaultPreset::SetDisabled(true);

    auto engineRes = ZHLN::Engine::Create(
        {.physics = {.maxBodies = 2048, .maxBodyPairs = 4096, .maxContactConstraints = 4096},
         .render  = {.appName = "Zahlen :: Procedural Locomotion Sample", .vsync = options.vsync, .fullscreen = options.fullscreen}}
    );

    if (!engineRes) {
        ZHLN::Log("FATAL: Failed to initialize Engine: {}", engineRes.error().Message());
        return EXIT_FAILURE;
    }

    auto engine = std::move(engineRes.value());
    engine->GetWindow().Focus();

    engine->InitializeDefaultScene();
    ZHLN::ProceduralAnimation::Register(*engine);
    BuildProceduralArena(*engine);

    // 1. Load the visual first so the CharacterVirtual hull can be fitted to
    // every transformed mesh-part bound instead of assuming a fixed human size.
    const char*            rigOverride = std::getenv("ZHLN_PROCEDURAL_RIG");
    const std::string_view rigPath     = rigOverride != nullptr && rigOverride[0] != '\0' ? std::string_view(rigOverride) :
                                                                                            std::string_view("ProceduralAnimationBaseRig.glb");
    ZHLN::Log("[Sample] Using procedural rig '{}'. Set ZHLN_PROCEDURAL_RIG to override.", rigPath);
    // Reading a .glb is an extra, so the sample asks the importer directly. It
    // caches the prefab, after which CreativeWorksFactory::LoadModelPrefab(rigPath)
    // -- the path Scene::ShapeKind::Prefab and the scripting bindings use -- finds
    // it without core knowing a parser exists.
    ZHLN::GLTF::InstallDeviceLostHandler(*engine);
    ZHLN::ModelPrefab* const prefab = ZHLN::GLTF::LoadGLBPrefab(engine->GetRenderContext(), engine->GetCreativeWorksManager(), rigPath);

    const ZHLN::Locomotion::CharacterBoundsEstimate bounds          = prefab != nullptr ? ZHLN::Locomotion::EstimateCharacterBounds(*prefab) :
                                                                                          ZHLN::Locomotion::CharacterBoundsEstimate {};
    const ZHLN::Physics::DualShapeConfig            dualShapeConfig = ZHLN::Locomotion::FitDualShapeToBounds(bounds);
    if (bounds.valid) {
        const JPH::Vec3 size = bounds.Size();
        ZHLN::Log(
            "[Sample] Estimated GLB bounds min=({}, {}, {}), max=({}, {}, {}), size=({}, {}, {}).", bounds.min.GetX(), bounds.min.GetY(), bounds.min.GetZ(),
            bounds.max.GetX(), bounds.max.GetY(), bounds.max.GetZ(), size.GetX(), size.GetY(), size.GetZ()
        );
    }
    ZHLN::Log(
        "[Sample] Character hull: lifter radius={}, bumper radius XZ={}, bumper radius Y={}, top={}.", dualShapeConfig.lifterRadius,
        dualShapeConfig.bumperRadiusXZ, dualShapeConfig.bumperRadiusY, dualShapeConfig.GetBumperOffsetY() + dualShapeConfig.bumperRadiusY
    );

    constexpr float handgunAuthoringHeight = 1.75f;
    const float     characterHeight        = bounds.valid ? std::max(bounds.Size().GetY(), 0.01f) : handgunAuthoringHeight;
    const float     handgunScale           = std::clamp(characterHeight / handgunAuthoringHeight, 0.35f, 1.75f);
    ZHLN::Log("[Sample] Test handgun scale={} from estimated character height={}.", handgunScale, characterHeight);

    constexpr float    kWalkSpeed = 2.40f;
    constexpr float    kJumpForce = 7.00f;
    const ZHLN::Entity player     = ZHLN::Locomotion::SpawnCharacter(*engine, JPH::Vec3(0.0f, 1.20f, 0.0f), dualShapeConfig, kWalkSpeed, kJumpForce);
    const ZHLN::Entity handgun    = CreateTestHandgun(*engine, player, handgunScale);

    // 2. Attach the already-loaded visual and the same proportionally-scaled
    // test handgun setup to either the reference or imported production rig.
    FirstPersonViewState viewState;
    viewState.eyeUpOffset      = std::clamp(characterHeight * 0.045f, 0.04f, 0.11f);
    viewState.eyeForwardOffset = std::clamp(characterHeight * 0.040f, 0.04f, 0.10f);
    AttachCharacterRig(*engine, player, rigPath, prefab, BuildTestHandgunHandling(handgun, handgunScale), viewState);

    SetHandgunVisibility(engine->GetRegistry(), handgun, false);

    ZHLN::Clock clock;
    float       sampleTime      = 0.0f;
    bool        handgunEquipped = false;
    bool        equipKeyWasDown = false;
    bool        slowMotion      = false;
    bool        tabKeyWasDown   = false;
    ZHLN::Log(
        "[ProceduralAnimationSample] Ready. WASD move, LSHIFT sprint, SPACE jump, E equip/rest handgun, V first/third person, TAB slow motion, Right-Click look. "
        "First person keeps the full body and arms but hides head visuals."
    );

    while (engine->IsRunning()) {
        const auto dt = std::min(clock.GetDeltaTime(), 0.05f);
        engine->ProcessEvents();

        auto& registry = engine->GetRegistry();

        // 1. Mouse Look and edge-triggered handgun equip input.
        bool equipKeyDown = false;
        bool viewKeyDown  = false;
        bool tabKeyDown   = false;
        for (ZHLN::Entity e: registry.GetEntitiesWith<ZHLN::Components::InputStateComponent>()) {
            registry.Patch<ZHLN::Components::InputStateComponent>(e, [&](auto& st) -> auto {
                equipKeyDown = equipKeyDown || st.IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::E));
                viewKeyDown  = viewKeyDown || st.IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::V));
                tabKeyDown   = tabKeyDown || st.IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::Tab));
                if (viewState.enabled) {
                    st.mouseWheel = 0.0f;
                }
                if (st.needsResize) {
                    engine->GetRenderContext().SetResolution(st.newSize);
                    st.needsResize = false;
                }
                if (st.IsMouseButtonDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::RButton))) {
                    if (viewState.enabled) {
                        viewState.lookYawOffset   = std::clamp(viewState.lookYawOffset + st.GetMouseDeltaX() * 0.15f, -80.0f, 80.0f);
                        viewState.lookPitchOffset = std::clamp(viewState.lookPitchOffset - st.GetMouseDeltaY() * 0.15f, -70.0f, 70.0f);
                    } else {
                        engine->GetCamera().yaw += st.GetMouseDeltaX() * 0.15f;
                        engine->GetCamera().pitch = std::clamp(engine->GetCamera().pitch - (st.GetMouseDeltaY() * 0.15f), -85.0f, 85.0f);
                    }
                }
            });
        }
        if (equipKeyDown && !equipKeyWasDown) {
            handgunEquipped = !handgunEquipped;
            registry.Patch<ZHLN::Animation::ItemHandlingComponent>(player, [&](auto& handling) -> auto {
                handling.driverMode    = handgunEquipped ? ZHLN::Animation::ItemDriverMode::AimGuided : ZHLN::Animation::ItemDriverMode::BodyMounted;
                handling.aimProgress   = handgunEquipped ? 1.0f : 0.0f;
                const size_t gripCount = std::min(handling.gripCount, handling.grips.size());
                for (size_t gripIndex = 0; gripIndex < gripCount; ++gripIndex) {
                    handling.grips[gripIndex].ikWeight = handgunEquipped ? 1.0f : 0.0f;
                }
            });
            SetHandgunVisibility(registry, handgun, handgunEquipped);
            ZHLN::Log("[Sample] Test handgun: {}.", handgunEquipped ? "EQUIPPED / AIM-GUIDED" : "RESTING / BODY-MOUNTED");
        }
        equipKeyWasDown = equipKeyDown;
        if (viewKeyDown && !viewState.toggleKeyWasDown) {
            SetFirstPersonMode(*engine, player, viewState, !viewState.enabled);
        }
        viewState.toggleKeyWasDown = viewKeyDown;
        if (tabKeyDown && !tabKeyWasDown) {
            slowMotion = !slowMotion;
            ZHLN::Log("[Sample] Slow motion: {}.", slowMotion ? "ON (0.25x)" : "OFF (1.0x)");
        }
        tabKeyWasDown = tabKeyDown;
        if (viewState.enabled) {
            registry.Patch<ZHLN::FirstPersonVisibilityComponent>(player, [&](auto& visibility) -> auto {
                visibility.lookYawDegrees   = viewState.lookYawOffset;
                visibility.lookPitchDegrees = viewState.lookPitchOffset;
            });
        }

        // 2. Update Procedural Look-At Orbit via Multi-Component Patch
        sampleTime += dt;
        registry.Patch<ZHLN::Components::TransformComponent, ZHLN::ProceduralLookAtComponent>(player, [&](const auto& trans, auto& lookAt) -> auto {
            lookAt.targetWorldPos = trans.position + trans.rotation * JPH::Vec3(std::sin(sampleTime * 0.65f) * 2.2f, 1.65f, 4.0f);
        });

        // 3. Synchronized Engine Tick (apply slow motion time scale)
        const float scaledDt = slowMotion ? dt * 0.25f : dt;
        const auto status = engine->Tick(scaledDt, ZHLN::GameplayDriver::Cpp);
        if (status == ZHLN::GameplayStatus::RequestQuit) {
            engine->GetWindow().Close();
            break;
        }

        // 4. Render diagnostic hull/skeleton only in third person; first person
        // keeps the actual full body and arms without debug geometry at the face.
        if (!viewState.enabled) {
            ZHLN::Locomotion::RenderDebugRig(*engine, player, dualShapeConfig);
            registry.Patch<ZHLN::Components::TransformComponent, ZHLN::RigBoneMap>(player, [&](const auto& trans, const auto& rig) -> auto {
                const auto* gait = registry.Get<ZHLN::ProceduralLocomotionComponent>(player);
                ZHLN::ProceduralAnimation::DrawDebugRig(engine->GetRenderContext(), trans.position, trans.rotation, rig, gait);
            });
        }
    }

    ZHLN::TaskSystem::Shutdown();
    return EXIT_SUCCESS;
}
