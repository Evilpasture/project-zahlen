// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Animation/Context.hpp>
#include <print>
#include <cstdio>
#include <Animation/Components.hpp>
#include <Animation/PrefabRig.hpp>
#include <Animation/ProceduralMath.hpp>
#include <Camera/TargetCamera.hpp>
#include <CharacterController/CharacterController.hpp>
#include <Signal/CrashHandler.hpp>
#include <Terrain/TerrainGeometry.hpp>
#include <Terrain/Terrain.hpp>
#include <Zahlen/COM/EngineFactory.hpp>
#include <Zahlen/COM/IActionContext.hpp>
#include <Zahlen/COM/IAssetLibrary.hpp>
#include <Zahlen/COM/IDebugDraw.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/ISimulation.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/ConfigCli.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/Engine/Clock.hpp>
#include <Zahlen/Engine/SpawnParams.hpp>
#include <Zahlen/FileSystem/Paths.hpp>
#include <Zahlen/Input/Input.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math/Angles.hpp>
#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Physics/PhysicsValues.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/COM/ILogger.hpp>
#include <glTF/GLTFAssets.hpp>
#include <glTF/GLTFDocument.hpp>

import ZHLN.Locomotion;
import ZHLN.ProceduralAnimation;

using ZHLN::FVec3;
using ZHLN::FVec4;
using ZHLN::Mat4x4;
using ZHLN::Quat;

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <numbers>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>
#include <Zahlen/COM/LoggerView.hpp>


namespace {

inline constexpr float kAmbientExposure = 10.0f;
inline constexpr float kSunIntensity    = 28.0f;
inline const FVec3 kSunPosition {25.0f, 60.0f, 25.0f};
inline const FVec3 kSunColor {1.00f, 0.96f, 0.90f};
inline const FVec4 kSkyZenith {0.25f, 0.55f, 0.95f, 1.0f};
inline const FVec4 kSkyHorizon {0.70f, 0.85f, 1.00f, 1.0f};
inline const FVec4 kSkyGround {0.20f, 0.28f, 0.20f, 1.0f};

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
    ZHLN::CameraRig::TargetComponent  thirdPersonCamera {};
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

[[nodiscard]] bool IsNodeUnder(const ZHLN::GLTF::Document& prefab, int32_t node, int32_t ancestor) {
    for (size_t depth = 0; depth < prefab.nodes.size() && node >= 0 && node < static_cast<int32_t>(prefab.nodes.size()); ++depth) {
        if (node == ancestor) {
            return true;
        }
        node = prefab.nodes[static_cast<size_t>(node)].parentIndex;
    }
    return false;
}

[[nodiscard]] Mat4x4 GetPrefabNodeModelTransform(const ZHLN::GLTF::Document& prefab, int32_t node) {
    if (node < 0 || node >= static_cast<int32_t>(prefab.nodes.size())) {
        return Mat4x4::Identity();
    }
    Mat4x4 transform = (prefab.nodes[static_cast<size_t>(node)].localTransform);
    int32_t    parent    = prefab.nodes[static_cast<size_t>(node)].parentIndex;
    for (size_t depth = 0; depth < prefab.nodes.size() && parent >= 0 && parent < static_cast<int32_t>(prefab.nodes.size()); ++depth) {
        transform = (prefab.nodes[static_cast<size_t>(parent)].localTransform) * transform;
        parent    = prefab.nodes[static_cast<size_t>(parent)].parentIndex;
    }
    return transform;
}

[[nodiscard]] bool HasHeadVisualAncestor(const ZHLN::GLTF::Document& prefab, int32_t node) {
    for (size_t depth = 0; depth < prefab.nodes.size() && node >= 0 && node < static_cast<int32_t>(prefab.nodes.size()); ++depth) {
        if (IsHeadVisualName(std::string_view(prefab.nodes[static_cast<size_t>(node)].name))) {
            return true;
        }
        node = prefab.nodes[static_cast<size_t>(node)].parentIndex;
    }
    return false;
}

[[nodiscard]] bool IsHeadDominatedSkin(const ZHLN::GLTF::Document& prefab, int32_t skeletonIndex) {
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

[[maybe_unused]] void CollectFirstPersonHeadMeshes(
    ZHLN::COM::IEntities&         entities,
    const ZHLN::GLTF::Document&   prefab,
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
    const FVec3 headPosition = ZHLN::ProceduralMath::Translation(GetPrefabNodeModelTransform(prefab, headNode));
    const FVec3 neckPosition = ZHLN::ProceduralMath::Translation(GetPrefabNodeModelTransform(prefab, neckNode));
    const float     headScale    = headNode >= 0 && neckNode >= 0 ? std::max((headPosition - neckPosition).Length(), 0.06f) : 0.12f;

    state.headMeshes.clear();
    for (ZHLN::Entity entity: visualParts) {
        auto mesh = entities.Get<ZHLN::Components::MeshComponent>(entity);
        if (!mesh || mesh->nodeIndex < 0 || mesh->nodeIndex >= static_cast<int32_t>(prefab.nodes.size())) {
            continue;
        }
        const bool underHead       = headNode >= 0 && IsNodeUnder(prefab, mesh->nodeIndex, headNode);
        const auto skeletalMesh    = entities.Get<ZHLN::Components::SkeletalMeshComponent>(entity);
        const bool headSkin        = skeletalMesh && IsHeadDominatedSkin(prefab, skeletalMesh->skeletonIndex);
        bool        compactNearHead = false;
        for (const ZHLN::GLTF::PartDocument& part: prefab.parts) {
            if (part.nodeIndex != mesh->nodeIndex || part.primitive >= prefab.primitives.size()) {
                continue;
            }
            const auto&  prim     = prefab.primitives[part.primitive];
            const Mat4x4 model    = GetPrefabNodeModelTransform(prefab, part.nodeIndex);
            const FVec3  localMin = prim.localMin;
            const FVec3  localMax = prim.localMax;
            const FVec3  localCenter     = (localMin + localMax) * 0.5f;
            const FVec3  halfExtent      = (localMax - localMin) * 0.5f;
            const FVec3  center          = ZHLN::ProceduralMath::Direction(model, localCenter) + ZHLN::ProceduralMath::Translation(model);
            const float      radius          = ZHLN::ProceduralMath::Direction(model, halfExtent).Length();
            const float      surfaceDistance = std::max((center - headPosition).Length() - radius, 0.0f);
            compactNearHead                  = headNode >= 0 && radius <= headScale * 2.8f && surfaceDistance <= headScale * 1.8f &&
                                               center.y >= neckPosition.y - headScale * 0.15f;
            if (compactNearHead) {
                break;
            }
        }
        if (underHead || HasHeadVisualAncestor(prefab, mesh->nodeIndex) || headSkin || compactNearHead) {
            state.headMeshes.push_back({.entity = entity, .originalFlags = mesh->flags});
        }
    }
}

void SetFirstPersonMode(ZHLN::COM::IEngine& engine, ZHLN::Entity player, FirstPersonViewState& state, bool enabled) {
    auto& entities      = *engine.Entities();
    auto cameraEntities = entities.EntitiesWith<ZHLN::Components::MainCameraTagComponent>();
    if (cameraEntities.empty()) {
        return;
    }
    state.cameraEntity = cameraEntities[0];

    if (enabled) {
        auto targetCamera = entities.Get<ZHLN::CameraRig::TargetComponent>(state.cameraEntity);
        if (!targetCamera) {
            return;
        }
        auto cameraComp = entities.GetSingleton<ZHLN::Components::CameraComponent>();
        if (!cameraComp) {
            return; // Nothing to save and nothing to pose for third person.
        }
        state.thirdPersonCamera = *targetCamera;
        state.thirdPersonNearZ  = cameraComp->camera.nearZ;
        state.thirdPersonSaved  = true;
        state.lookYawOffset     = 0.0f;
        state.lookPitchOffset   = 0.0f;
        entities.Detach<ZHLN::CameraRig::TargetComponent>(state.cameraEntity);
        entities.Patch<ZHLN::ProceduralLookAtComponent>(player, [&](auto& lookAt) {
            state.thirdPersonLookAtWeight = lookAt.weight;
            state.lookAtWeightSaved       = true;
            lookAt.weight                 = 0.0f;
        });
        entities.PatchSingleton<ZHLN::Components::CameraComponent>([](auto& cc) {
            cc.camera.fov   = ZHLN::DegF{75.0f};
            cc.camera.nearZ = 0.03f;
        });
    } else if (state.thirdPersonSaved) {
        ZHLN::CameraRig::TargetComponent restored = state.thirdPersonCamera;
        restored.hasInitSmoothTarget                     = 0;
        if (!entities.Patch<ZHLN::CameraRig::TargetComponent>(state.cameraEntity, [&](auto& targetCamera) { targetCamera = restored; })) {
            entities.AttachMany(state.cameraEntity, std::move(restored));
        }
        entities.PatchSingleton<ZHLN::Components::CameraComponent>([&](auto& cameraComp) {
            cameraComp.camera.yaw   = ZHLN::DegF{state.thirdPersonCamera.yaw};
            cameraComp.camera.pitch = ZHLN::DegF{state.thirdPersonCamera.pitch};
            cameraComp.camera.fov   = ZHLN::DegF{state.thirdPersonCamera.fov};
            cameraComp.camera.nearZ = state.thirdPersonNearZ;
        });
        if (state.lookAtWeightSaved) {
            entities.Patch<ZHLN::ProceduralLookAtComponent>(player, [&](auto& lookAt) {
                lookAt.weight = state.thirdPersonLookAtWeight;
            });
            state.lookAtWeightSaved = false;
        }
        state.thirdPersonSaved = false;
    }

    entities.Patch<ZHLN::FirstPersonVisibilityComponent>(player, [&](auto& visibility) -> auto {
        visibility.eyeOffsetModel = {0.0f, state.eyeUpOffset, state.eyeForwardOffset};
        visibility.lookYawDegrees    = state.lookYawOffset;
        visibility.lookPitchDegrees  = state.lookPitchOffset;
        visibility.enabled           = enabled;
        visibility.hideHead          = true;
        visibility.hideHair          = true;
        visibility.cameraInitialized = false;
    });
    for (const HiddenHeadMesh& hidden: state.headMeshes) {
        entities.Patch<ZHLN::Components::MeshComponent>(hidden.entity, [&](auto& mesh) {
            mesh.flags = enabled ? hidden.originalFlags | ZHLN::DrawFlags::Hidden : hidden.originalFlags;
        });
    }
    state.enabled = enabled;
    ZHLN::COM::Log(engine.Logger(), "Camera mode: {}.", enabled ? "FIRST PERSON / HEAD-CONSTRAINED FULL BODY" : "THIRD PERSON / ORBIT");
}

[[maybe_unused]] bool EnvironmentFlag(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return false;
    }
    const std::string_view text(value);
    return text != "0" && text != "false" && text != "FALSE" && text != "off" && text != "OFF";
}

[[maybe_unused]] float EnvironmentFloat(const char* name, float fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    char*       end    = nullptr;
    const float parsed = std::strtof(value, &end);
    return end != value ? parsed : fallback;
}

auto BuildProceduralArena(ZHLN::COM::IEngine& engine, ZHLN::Terrain::System& terrain) -> void {
    auto& entities = *engine.Entities();

    ZHLN::GraphicsSettings gfx      = engine.Graphics()->Settings();
    gfx.environment.ambientExposure = kAmbientExposure;
    gfx.environment.skyZenith       = kSkyZenith;
    gfx.environment.skyHorizon      = kSkyHorizon;
    gfx.environment.skyGround       = kSkyGround;
    engine.Graphics()->ApplySettings(gfx);

    if (auto geom = ZHLN::Terrain::Generate({.sampleCount = 128, .worldSize = 220.0f, .maxHeight = 12.0f, .type = ZHLN::Terrain::Type::Default})) {
        (void)terrain.Create(std::move(*geom), {.position = {0.0f, 0.0f, 0.0f}, .roughness = 0.80f, .createPhysics = true});
    }

    // 3. Center Platform
    engine.Assets()->SpawnBox({10.0f, 0.50f, 10.0f},
        ZHLN::PrefabFactory::SpawnParams {
            .position = {0.0, 0.50, 0.0}, .createPhysics = true, .isStaticPhysics = true, .roughness = 0.50f, .color = {0.32f, 0.34f, 0.38f, 1.0f}
        }
    );

    // 4. 30-Degree Grounding Test Ramp
    engine.Assets()->SpawnBox({4.5f, 0.18f, 2.2f},
        ZHLN::PrefabFactory::SpawnParams {
            .position        = {-14.0, 2.75, 5.0},
            .rotation        = Quat::FromAxisAngle(FVec3::UnitZ(), ZHLN::DegF{30.0f}.ToRadians()),
            .createPhysics   = true,
            .isStaticPhysics = true,
            .roughness       = 0.55f,
            .color           = {0.25f, 0.50f, 0.72f, 1.0f}
        }
    );

    // 5. Stepping Stones
    for (int i = 0; i < 6; ++i) {
        float stepHeight = 0.20f + (static_cast<float>(i) * 0.10f);
        engine.Assets()->SpawnBox({1.0f, stepHeight * 0.5f, 1.0f},
            ZHLN::PrefabFactory::SpawnParams {
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
        FVec4 color;
    };
    const std::array<PillarDesc, 4> pillars = {
        {{.x = -14.0f, .z = -8.0f, .width = 1.5f, .height = 5.0f, .color = {0.35f, 0.45f, 0.60f, 1.0f}},
         {.x = -18.0f, .z = 4.0f, .width = 2.0f, .height = 7.5f, .color = {0.30f, 0.40f, 0.55f, 1.0f}},
         {.x = 18.0f, .z = 12.0f, .width = 2.2f, .height = 9.0f, .color = {0.25f, 0.35f, 0.50f, 1.0f}},
         {.x = 14.0f, .z = -16.0f, .width = 2.0f, .height = 8.0f, .color = {0.28f, 0.38f, 0.52f, 1.0f}}}
    };
    for (const auto& p: pillars) {
        engine.Assets()->SpawnBox({p.width * 0.5f, p.height * 0.5f, p.width * 0.5f},
            ZHLN::PrefabFactory::SpawnParams {
                .position        = {static_cast<double>(p.x), static_cast<double>(p.height * 0.5f), static_cast<double>(p.z)},
                .createPhysics   = true,
                .isStaticPhysics = true,
                .color           = p.color
            }
        );
    }

    // 7. Directional Sunlight via Variadic Create
    entities.Spawn(
        ZHLN::Components::NameComponent {.name = ZHLN::String64("SunLight")}, ZHLN::Components::TransformComponent {.position = kSunPosition},
        ZHLN::Components::LightComponent {
            .type = ZHLN::LightType::Sun, .color = kSunColor, .intensity = kSunIntensity, .direction = FVec3{0.45f, 1.00f, 0.30f}.Normalized()
        }
    );
}

void SetHandgunVisibility(ZHLN::COM::IEntities& entities, ZHLN::Entity handgunRoot, bool visible) {
    for (ZHLN::Entity e: entities.EntitiesWith<ZHLN::Components::MeshComponent>()) {
        const auto hier = entities.Get<ZHLN::Components::HierarchyComponent>(e);
        if (hier && hier->parent == handgunRoot) {
            entities.Patch<ZHLN::Components::MeshComponent>(e, [visible](auto& mesh) {
                if (visible) {
                    mesh.flags &= ~ZHLN::DrawFlags::Hidden;
                } else {
                    mesh.flags |= ZHLN::DrawFlags::Hidden;
                }
            });
        }
    }
}

auto CreateTestHandgun(ZHLN::COM::IEngine& engine, ZHLN::Entity player, float itemScale) -> ZHLN::Entity {
    auto&              entities = *engine.Entities();
    const float        scale    = std::clamp(itemScale, 0.10f, 4.0f);
    const ZHLN::Entity handgun  = entities.Spawn(
        ZHLN::Components::NameComponent {.name = ZHLN::String64("ProceduralTestHandgun")}, ZHLN::Components::TransformComponent {},
        ZHLN::Components::WorldTransformComponent {}, ZHLN::Components::HierarchyComponent {.parent = player}
    );

    auto addPart = [&](std::string_view name, FVec3 halfExtents, FVec3 localPosition, Quat localRotation, FVec4 color,
                       float metallic) {
        ZHLN::MaterialDesc materialDesc;
        materialDesc.metallic         = metallic;
        materialDesc.roughness        = 0.32f;
        materialDesc.baseColor        = {color.x, color.y, color.z, color.w};
        const ZHLN::Material material = [&]{ auto r = engine.Graphics()->CreateMaterial(materialDesc); return r.HasValue() ? r.value : ZHLN::Material{}; }();
        const ZHLN::Entity part = engine.Assets()->SpawnBox(
            FVec3{halfExtents} * scale,
            ZHLN::PrefabFactory::SpawnParams {.position = {0.0, 0.0, 0.0}, .createPhysics = false, .materialOverride = material}
        );
        entities.AttachMany(part, ZHLN::Components::HierarchyComponent {.parent = handgun});
        entities.Patch<ZHLN::Components::TransformComponent>(part, [&](auto& transform) -> auto {
            transform.position = FVec3{localPosition} * scale;
            transform.rotation = localRotation;
        });
        entities.Patch<ZHLN::Components::NameComponent>(part, [&](auto& component) -> auto { component.name = ZHLN::String64(name); });
    };

    const FVec4 darkMetal{0.10f, 0.12f, 0.15f, 1.0f};
    const FVec4 gripColor{0.20f, 0.12f, 0.08f, 1.0f};
    addPart("Handgun_Slide", FVec3{0.075f, 0.055f, 0.22f}, FVec3{0.0f, 0.025f, 0.08f}, Quat::Identity(), darkMetal, 0.90f);
    addPart(
        "Handgun_Barrel", FVec3{0.035f, 0.035f, 0.24f}, FVec3{0.0f, 0.015f, 0.11f}, Quat::Identity(), FVec4{0.05f, 0.06f, 0.07f, 1.0f}, 1.0f
    );
    addPart(
        "Handgun_Grip", FVec3{0.060f, 0.135f, 0.055f}, FVec3{0.0f, -0.145f, -0.065f},
        Quat::FromAxisAngle(FVec3::UnitX(), ZHLN::DegF{-10.0f}.ToRadians()), gripColor, 0.35f
    );
    addPart("Handgun_TriggerGuard", FVec3{0.045f, 0.035f, 0.055f}, FVec3{0.0f, -0.070f, 0.015f}, Quat::Identity(), darkMetal, 0.80f);
    addPart(
        "Handgun_Sight", FVec3{0.018f, 0.018f, 0.045f}, FVec3{0.0f, 0.095f, 0.02f}, Quat::Identity(), FVec4{0.75f, 0.18f, 0.08f, 1.0f}, 0.20f
    );
    return handgun;
}

[[nodiscard]] auto BuildTestHandgunHandling(ZHLN::Entity handgun, float itemScale) -> ZHLN::Animation::ItemHandlingComponent {
    ZHLN::Animation::ItemHandlingComponent handling;
    const float                            scale  = std::clamp(itemScale, 0.10f, 4.0f);
    const auto                             scaled = [scale](FVec3 value) { return FVec3{value} * scale; };
    handling.driverMode                           = ZHLN::Animation::ItemDriverMode::BodyMounted;
    handling.itemEntity                           = handgun;
    handling.hipLocalOffset                       = ZHLN::CreateTranslation(scaled(FVec3{-0.24f, -0.32f, 0.08f}));
    handling.aimLocalOffset                       = ZHLN::CreateTranslation(scaled(FVec3{0.0f, -0.20f, 0.30f}));
    handling.aimProgress                          = 1.0f;
    handling.sway.massKg                          = 1.1f;
    handling.sway.stiffness                       = 115.0f;
    handling.sway.damping                         = 0.90f;
    handling.avoidance                            = {.probeDistance = 0.55f * scale, .probeRadius = 0.08f * scale, .pushbackScale = 0.80f, .tiltScale = 0.35f};
    handling.grips[0]                             = {
        .assignedLimb      = ZHLN::CharacterBone::HandR,
        .orientationMode   = ZHLN::Animation::GripOrientationMode::AutomaticHanded,
        .localTransform    = ZHLN::CreateTranslation(scaled(FVec3{-0.055f, -0.13f, -0.055f})),
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
        .localTransform    = ZHLN::CreateTranslation(scaled(FVec3{0.060f, -0.070f, 0.015f})),
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
    ZHLN::COM::IEngine&                          engine,
    ZHLN::Entity                           player,
    std::string_view                       glbPath,
    const std::optional<ZHLN::GLTF::Document>&       prefab,
    const std::optional<ZHLN::Animation::PrefabRig>& rigInfo,
    ZHLN::AssetID                                    prefabHash,
    ZHLN::Animation::ItemHandlingComponent           itemHandling,
    FirstPersonViewState&                  viewState
) -> void {
    auto& reg = *engine.Entities();

    ZHLN::ProceduralLocomotionComponent locomotion {
        .strideLength = 1.40f,
        .stepHeight   = 0.22f,
        .legReach     = 0.83f,
    };
    ZHLN::HairStrandsComponent      hair {};
    ZHLN::ProceduralLookAtComponent lookAt {
        .targetWorldPos = FVec3{0.0f, 2.0f, 4.0f},
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
    ZHLN::COM::Log(engine.Logger(), "Pose interpolation: {} (stiffness={}, damping factor={}, bicubic tension={}).", useBicubic ? "bicubic" : "spring-damper", animationConfig.springStiffness, animationConfig.springDampingFactor, animationConfig.bicubicTension);
    ZHLN::COM::Log(engine.Logger(), "Procedural layers: gait={}, gravity bounce={}, leg IK={}, acceleration tilt={}, upper body={}, secondary motion={}.", animationConfig.enableGait, animationConfig.enableGravityBounce, animationConfig.enableLegIK, animationConfig.enableAccelerationTilt, animationConfig.enableUpperBody, animationConfig.enableSecondaryMotion);
    ZHLN::COM::Log(engine.Logger(), "Leg IK weight={}; authored X/Z=true; world lock={}; max height correction={}; pelvis-drop weight={}.", animationConfig.enableLegIK ? animationConfig.legIKWeight : 0.0f, animationConfig.worldLockFeet, animationConfig.maxFootHeightCorrection, animationConfig.pelvisDropWeight);
    ZHLN::COM::Log(engine.Logger(), "IK limits: extension={}, body tilt={} deg, ankle sideways={} deg, ankle forward={} deg.", animationConfig.maxLegExtension, animationConfig.maxIKBodyTiltDegrees, animationConfig.maxAnkleSidewaysDegrees, animationConfig.maxAnkleForwardDegrees);
    if (animationConfig.authoredPoseOnly) {
        ZHLN::COM::Log(engine.Logger(), "Authored-pose-only isolation enabled; all procedural layers are bypassed.");
    } else if (!animationConfig.enableLegIK) {
        ZHLN::COM::Log(engine.Logger(), "Leg IK disabled; gait/keyframe layers remain active.");
    }

    if (prefab) {
        ZHLN::COM::Log(engine.Logger(), "GLB model '{}' loaded successfully. Instantiating visual parts...", glbPath);

        int32_t idleTrack = rigInfo ? ZHLN::FindAnimationTrack(*rigInfo, "idle") : -1;
        if (idleTrack < 0 && !prefab->animations.empty()) {
            idleTrack = 0;
        }
        const int32_t walkTrack = rigInfo ? ZHLN::FindAnimationTrack(*rigInfo, "walk") : -1;
        const int32_t runTrack  = rigInfo ? ZHLN::FindAnimationTrack(*rigInfo, "run") : -1;
        if (idleTrack >= 0) {
            const auto& clip = prefab->animations[static_cast<size_t>(idleTrack)];
            ZHLN::COM::Log(engine.Logger(), "Selected idle track {}: '{}' (duration={}, channels={}).", idleTrack, std::string_view(clip.name), clip.duration, clip.channels.size());
            ZHLN::COM::Log(engine.Logger(), "Locomotion tracks: idle={}, walk={}, run={}.", idleTrack, walkTrack, runTrack);
        } else {
            ZHLN::COM::LoggerView{engine.Logger()}.Warn("'{}' contains no authored animation track; bind pose will be shown.", glbPath);
        }

        // Instantiate the visual hierarchy without physics colliders. A prefab
        // can emit one root, one entity per part, and at most one emissive VPL
        // per part. Size the output dynamically: InstantiatePrefab returns the
        // total number spawned even when the caller's output span is smaller.
        const size_t              outputCapacity = 1 + prefab->parts.size() * 2;
        std::vector<ZHLN::Entity> parts(outputCapacity);
        parts = engine.Assets()->Instantiate(prefabHash, ZHLN::PrefabFactory::SpawnParams {.position = {0.0, 1.20, 0.0}, .createPhysics = false, .isAnimated = true});
        const auto writtenCount = static_cast<uint32_t>(parts.size());

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
            reg.Despawn(parts[0]);
        }

        // Variadic component registration: AnimatorComponent triggers RigBoneMap discovery
        reg.AttachMany(
            player,
            ZHLN::Animation::AnimatorComponent {
                .prefabAsset     = prefabHash,
                .currentTrackIdx = idleTrack,
                .currentLoop     = true,
            },
            ZHLN::ProceduralLocomotionTracksComponent {
                .idleTrack = idleTrack,
                .walkTrack = walkTrack,
                .runTrack  = runTrack,
            },
            ZHLN::RigBoneMap {}, // Initialized lazily on frame 0 by the optional subsystem
            std::move(locomotion), std::move(hair), std::move(lookAt), animationConfig, ZHLN::FirstPersonVisibilityComponent {}, std::move(itemHandling)
        );
    } else {
        ZHLN::COM::LoggerView{engine.Logger()}.Warn("'{}' not found; falling back to the in-memory procedural rig.", glbPath);

        ZHLN::RigBoneMap proceduralRig {};
        ZHLN::BuildStandardProceduralRig(proceduralRig);

        reg.AttachMany(
            player, std::move(proceduralRig), std::move(locomotion), std::move(hair), std::move(lookAt),
            animationConfig, ZHLN::FirstPersonVisibilityComponent {}, std::move(itemHandling)
        );
    }
}

} // namespace

auto main(int argc, char* argv[]) -> int {
    ZHLN::Platform::Init();
    ZHLN::EngineConfig engineConfig {
        .physics = {.maxBodies = 2048, .maxBodyPairs = 4096, .maxContactConstraints = 4096},
        .appName = "Zahlen :: Procedural Locomotion Sample",
    };

    auto optionsRes = ZHLN::ParseEngineConfig(std::span(argv, static_cast<size_t>(argc)), engineConfig);
    if (!optionsRes) {
        std::println(stderr, "[Sample] command line: {}", optionsRes.error());
        return EXIT_FAILURE;
    }
    if (optionsRes->helpRequested || optionsRes->versionRequested || optionsRes->graphRequested) {
        return EXIT_SUCCESS;
    }

    static ZHLN::CrashState crashState;
    ZHLN::SetupSignalHandler(crashState);
    ZHLN::TaskSystem::Scope taskScope;

    auto engineRes = ZHLN::COM::CreateEngine(engineConfig);
    if (!engineRes) {
        std::println(stderr, "Failed to initialize Engine: {}", engineRes.error());
        return EXIT_FAILURE;
    }

    auto engine = std::move(*engineRes);
    if (!engineConfig.headless) {
        engine->FocusWindow(engine->PrimaryWindow());
    }

    ZHLN::Terrain::System terrain;
    terrain.Start(*engine);
    BuildProceduralArena(*engine, terrain);

    const char*            rigOverride = std::getenv("ZHLN_PROCEDURAL_RIG");
    const std::string_view rigPath     = rigOverride != nullptr && rigOverride[0] != '\0' ? std::string_view(rigOverride) :
                                                                                             std::string_view("ProceduralAnimationBaseRig.glb");
    ZHLN::COM::Log(engine->Logger(), "Using procedural rig '{}'. Set ZHLN_PROCEDURAL_RIG to override.", rigPath);

    std::optional<ZHLN::GLTF::Document>       prefabDoc;
    ZHLN::GLTF::Asset                         prefabAsset {};
    std::optional<ZHLN::Animation::PrefabRig> rigInfo;
    auto&                                     assets         = *engine->Assets();
    std::string                               rigVirtualPath = std::string(rigPath);
    if (!assets.VirtualFileExists(std::string_view(rigVirtualPath))) {
        std::filesystem::path hostPath(rigPath);
        if (!std::filesystem::exists(hostPath)) {
            if (const auto found = ZHLN::FS::Paths::FindDataFile("resources/assets/" + std::string(rigPath))) {
                hostPath = *found;
            }
        }
        if (std::filesystem::exists(hostPath)) {
            const std::string dir = (hostPath.has_parent_path() ? hostPath.parent_path() : std::filesystem::current_path()).string();
            (void)assets.MountDirectory(std::string_view(dir), ZHLN::COM::MountDirectoryOptions {.virtualPrefix = "rig"});
            rigVirtualPath = "rig/" + hostPath.filename().string();
        }
    }
    if (auto decoded = ZHLN::GLTF::Read(assets, rigVirtualPath)) {
        for (const auto& warning : decoded->warnings)
            ZHLN::Log<ZHLN::LogSeverity::Warning>("glTF: {}", warning);
        if (auto uploaded = ZHLN::GLTF::Upload(*engine->Assets(), *engine->Graphics(), *decoded)) {
            prefabDoc   = std::move(*decoded);
            prefabAsset = std::move(*uploaded);
            rigInfo     = ZHLN::Animation::LoadPrefabRig(*engine->Assets(), prefabAsset.hash);
        }
    }

    const ZHLN::Locomotion::PrefabBounds bounds =
        rigInfo ? ZHLN::Locomotion::MeasurePrefabBounds(*rigInfo) : ZHLN::Locomotion::PrefabBounds {};
    const auto dualShapeConfig = ZHLN::Locomotion::FitDualShapeToBounds(bounds);
    if (bounds.valid) {
        const FVec3 size = bounds.GetSize();
        ZHLN::COM::Log(engine->Logger(),
            "Estimated GLB bounds min=({}, {}, {}), max=({}, {}, {}), size=({}, {}, {}).", bounds.min.x, bounds.min.y, bounds.min.z, bounds.max.x,
            bounds.max.y, bounds.max.z, size.x, size.y, size.z);
    }
    ZHLN::COM::Log(engine->Logger(),
        "Character hull: lifter radius={}, bumper radius XZ={}, bumper radius Y={}, top={}.", dualShapeConfig.lifterRadius,
        dualShapeConfig.bumperRadiusXZ, dualShapeConfig.bumperRadiusY, dualShapeConfig.GetBumperOffsetY() + dualShapeConfig.bumperRadiusY);

    constexpr float handgunAuthoringHeight = 1.75f;
    const float     characterHeight        = bounds.valid ? std::max(bounds.GetSize().y, 0.01f) : handgunAuthoringHeight;
    const float     handgunScale           = std::clamp(characterHeight / handgunAuthoringHeight, 0.35f, 1.75f);

    constexpr float    kWalkSpeed = 2.40f;
    constexpr float    kJumpForce = 7.00f;
    const auto playerBody = engine->Simulation()->CreateBody(
        ZHLN::Locomotion::MakeDualCollider(dualShapeConfig),
        ZHLN::Physics::CharacterBody {
            .position         = {0.0, 1.20, 0.0},
            .supportingVolume = ZHLN::Physics::Plane {{0.0f, 1.0f, 0.0f}, -dualShapeConfig.GetLifterOffsetY()},
        }).value;
    const ZHLN::Entity player = engine->Entities()->Spawn(
        ZHLN::Components::TransformComponent {.position = {0.0f, 1.20f, 0.0f}},
        ZHLN::Components::CharacterBodyComponent {.bodyID = playerBody}
    );
    ZHLN::Character::Attach(*engine->Entities(), player, {.speed = kWalkSpeed, .jumpForce = kJumpForce});
    const ZHLN::Entity handgun    = CreateTestHandgun(*engine, player, handgunScale);

    FirstPersonViewState viewState;
    viewState.eyeUpOffset      = std::clamp(characterHeight * 0.045f, 0.04f, 0.11f);
    viewState.eyeForwardOffset = std::clamp(characterHeight * 0.040f, 0.04f, 0.10f);
    AttachCharacterRig(*engine, player, rigPath, prefabDoc, rigInfo, prefabAsset.hash, BuildTestHandgunHandling(handgun, handgunScale), viewState);
    SetHandgunVisibility(*engine->Entities(), handgun, false);

    ZHLN::ProceduralAnimation::Layer procAnimLayer {};
    uint64_t lastGeneration = engine->DeviceGeneration();
    ZHLN::Clock clock({.maxDeltaTime = 0.05f});
    bool        handgunEquipped = false;
    bool        equipKeyWasDown = false;
    bool        slowMotion      = false;
    bool        tabKeyWasDown   = false;
    ZHLN::COM::Log(engine->Logger(),
        "Ready. WASD move, LSHIFT sprint, SPACE jump, E equip/rest handgun, V first/third person, TAB slow motion, Right-Click look.");

    while (engine->IsRunning()) {
        clock.Tick();
        ZHLN::CheckForCrashes(crashState, engine.Get());
        engine->ProcessEvents();

        if (prefabDoc && engine->DeviceGeneration() != lastGeneration) {
            if (ZHLN::GLTF::RebuildGPUResources(*engine, *prefabDoc, prefabAsset)) {
                lastGeneration = engine->DeviceGeneration();
            }
        }

        auto&       entities = *engine->Entities();
        const auto& input    = *engine->Actions();
        if (input.KeyPressed(ZHLN::KeyCode::Escape, ZHLN::kPrimarySeat)) {
            engine->RequestClose();
            break;
        }

        const bool equipKeyDown = input.KeyDown(ZHLN::KeyCode::E, ZHLN::kPrimarySeat);
        const bool viewKeyDown  = input.KeyDown(ZHLN::KeyCode::V, ZHLN::kPrimarySeat);
        const bool tabKeyDown   = input.KeyDown(ZHLN::KeyCode::Tab, ZHLN::kPrimarySeat);
        if (input.MouseButtonDown(ZHLN::MouseButton::Right, ZHLN::kPrimarySeat)) {
            const ZHLN::Axis2DValue look = input.MouseDelta(ZHLN::kPrimarySeat);
            if (viewState.enabled) {
                viewState.lookYawOffset   = std::clamp(viewState.lookYawOffset + look.x * 0.15f, -80.0f, 80.0f);
                viewState.lookPitchOffset = std::clamp(viewState.lookPitchOffset - look.y * 0.15f, -70.0f, 70.0f);
            } else {
                entities.PatchSingleton<ZHLN::Components::CameraComponent>([&](auto& cameraComp) {
                    cameraComp.camera.yaw += ZHLN::DegF{look.x * 0.15f};
                    cameraComp.camera.pitch = std::clamp(cameraComp.camera.pitch - ZHLN::DegF{look.y * 0.15f}, ZHLN::DegF{-85.0f}, ZHLN::DegF{85.0f});
                });
            }
        }
        if (equipKeyDown && !equipKeyWasDown) {
            handgunEquipped = !handgunEquipped;
            entities.Patch<ZHLN::Animation::ItemHandlingComponent>(player, [&](auto& handling) {
                handling.driverMode    = handgunEquipped ? ZHLN::Animation::ItemDriverMode::AimGuided : ZHLN::Animation::ItemDriverMode::BodyMounted;
                handling.aimProgress   = handgunEquipped ? 1.0f : 0.0f;
                const size_t gripCount = std::min(handling.gripCount, handling.grips.size());
                for (size_t gripIndex = 0; gripIndex < gripCount; ++gripIndex) {
                    handling.grips[gripIndex].ikWeight = handgunEquipped ? 1.0f : 0.0f;
                }
            });
            SetHandgunVisibility(entities, handgun, handgunEquipped);
        }
        equipKeyWasDown = equipKeyDown;
        if (viewKeyDown && !viewState.toggleKeyWasDown) {
            SetFirstPersonMode(*engine, player, viewState, !viewState.enabled);
        }
        viewState.toggleKeyWasDown = viewKeyDown;
        if (tabKeyDown && !tabKeyWasDown) {
            slowMotion = !slowMotion;
            clock.SetTimeScale(slowMotion ? 0.25f : 1.0f);
        }
        tabKeyWasDown = tabKeyDown;
        if (viewState.enabled) {
            entities.Patch<ZHLN::FirstPersonVisibilityComponent>(player, [&](auto& visibility) {
                visibility.lookYawDegrees   = viewState.lookYawOffset;
                visibility.lookPitchDegrees = viewState.lookPitchOffset;
            });
        }

        const auto sampleTime = static_cast<float>(clock.UnscaledElapsedTime());
        if (const auto trans = entities.Get<ZHLN::Components::TransformComponent>(player)) {
            const FVec3 pos = trans->position;
            const Quat  rot {trans->rotation};
            entities.Patch<ZHLN::ProceduralLookAtComponent>(player, [&](auto& lookAt) {
                lookAt.targetWorldPos = pos + rot * FVec3{std::sin(sampleTime * 0.65f) * 2.2f, 1.65f, 4.0f};
            });
        }

        const float dt = clock.DeltaTime();
        ZHLN::Character::Step(*engine, dt);
        (void)terrain.BeforeRender(*engine, dt);
        procAnimLayer.Tick(*engine, dt);
        if (!viewState.enabled) {
            ZHLN::CameraRig::Step(*engine, dt);
        }

        if (!viewState.enabled && entities.IsAlive(player)) {
            if (const auto transform = entities.Get<ZHLN::Components::TransformComponent>(player)) {
                const auto movement = entities.Get<ZHLN::Character::MovementComponent>(player);
                FVec3      velocity = FVec3::Zero();
                if (movement) {
                    const float speed = movement->speed * (movement->isSprinting ? std::max(movement->sprintMultiplier, 1.0f) : 1.0f);
                    velocity          = FVec3 {movement->inputX * speed, movement->currentYVel, movement->inputZ * speed};
                }
                (void)velocity;
                ZHLN::Locomotion::DrawDualShapeDebug(*engine->Debug(), transform->position, dualShapeConfig);

                if (const auto rig = entities.Get<ZHLN::RigBoneMap>(player)) {
                    const auto gait = entities.Get<ZHLN::ProceduralLocomotionComponent>(player);
                    ZHLN::ProceduralAnimation::DrawDebugRig(*engine->Debug(), transform->position, Quat{transform->rotation}, *rig, gait ? &*gait : nullptr);
                }
            }
        }

        const auto result = engine->Tick(dt);
        if (!result.HasValue()) {
            ZHLN::COM::LoggerView{engine->Logger()}.Error("frame failed: {}", result.error);
            break;
        }
    }

    procAnimLayer.Stop(*engine);
    terrain.Stop(*engine);
    if (prefabAsset.ownsResources) {
        for (ZHLN::TextureID tex : prefabAsset.textures) {
            if (tex != ZHLN::TextureID::Invalid) engine->Graphics()->UnloadTexture(tex);
        }
    }
    return EXIT_SUCCESS;
}
