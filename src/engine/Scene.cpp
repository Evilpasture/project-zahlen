// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/Scene.cpp
//
// Turns a Scene description into entities in a given engine.
//
// Everything here takes the engine as an argument. There is no ambient lookup
// and no static scene state, so instantiating the same description twice --
// into two engines, or into one engine after a reset -- produces the same
// result both times. That reproducibility is the whole reason the description
// is data instead of a function that builds a scene.

#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Scene.hpp>
#include <Zahlen/TOML.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <array>
#include <string>

namespace ZHLN::Scene {

namespace {

[[nodiscard]] auto ToVec3(const std::array<float, 3>& v) noexcept -> JPH::Vec3 {
    return JPH::Vec3 {v[0], v[1], v[2]};
}

[[nodiscard]] auto ToRVec3(const std::array<float, 3>& v) noexcept -> JPH::RVec3 {
    return JPH::RVec3 {v[0], v[1], v[2]};
}

[[nodiscard]] auto ToVec4(const std::array<float, 4>& v) noexcept -> JPH::Vec4 {
    return JPH::Vec4 {v[0], v[1], v[2], v[3]};
}

/// Builds the SpawnParams shared by every shape: placement, body kind and the
/// emissive-light opt-in.
[[nodiscard]] auto MakeSpawnParams(const SceneEntity& entity) -> CreativeWorksFactory::SpawnParams {
    return CreativeWorksFactory::SpawnParams {
        .position        = ToRVec3(entity.transform.position),
        .rotation        = Math::EulerDegreesToQuat(ToVec3(entity.transform.rotation)),
        .scale           = ToVec3(entity.transform.scale),
        .createPhysics   = entity.body != BodyKind::None,
        // SpawnParams defaults this to true, and a description that asked for
        // BodyKind::Dynamic must not silently get a body that cannot move.
        .isStaticPhysics = entity.body != BodyKind::Dynamic,

        .emissiveVirtualLights = entity.material.emissiveVirtualLights,

        .roughness = entity.material.roughness,
        .metallic  = entity.material.metallic,
        .color     = ToVec4(entity.material.baseColor)
    };
}

/// Emissive is the reason a scene entity needs a real material rather than the
/// colour/roughness shorthand: the factory's built-in material has no emissive
/// factor to set.
[[nodiscard]] auto NeedsMaterial(const SceneMaterial& material) noexcept -> bool {
    return material.emissive[0] > 0.0f || material.emissive[1] > 0.0f || material.emissive[2] > 0.0f;
}

[[nodiscard]] auto BuildMaterial(RenderContext& ctx, const SceneMaterial& material) -> std::expected<ZHLN::Material, Error> {
    return CreativeWorksFactory::CreateMaterial(
        ctx, CreativeWorksFactory::MaterialDesc {
                 .metallic  = material.metallic,
                 .roughness = material.roughness,
                 .baseColor = material.baseColor,
                 .emissive  = {material.emissive[0], material.emissive[1], material.emissive[2], 1.0f}
             }
    );
}

void NameEntity(ECS::Registry& registry, Entity entity, const std::string& name) {
    if (name.empty() || entity == Entity::Null()) {
        return;
    }
    registry.Assign<Components::NameComponent>(entity, String64(name));
}

} // namespace

auto Instantiate(Engine& engine, const Scene& description) -> std::expected<Instance, Error> {
    Instance instance;
    instance.entities.reserve(description.entities.size());
    instance.lights.reserve(description.lights.size());

    auto& registry = engine.GetRegistry();

    // --- camera -------------------------------------------------------------
    auto& camera    = engine.GetCamera();
    camera.position = ToVec3(description.camera.position);
    camera.yaw      = description.camera.yaw;
    camera.pitch    = description.camera.pitch;
    camera.fov      = description.camera.fov;

    // --- environment --------------------------------------------------------
    const SceneEnvironment& environment = description.environment;
    for (const Entity settings: registry.GetEntitiesWith<Components::GlobalSettingsTagComponent>()) {
        registry.Patch<Components::PostProcessSettingsComponent>(settings, [&](auto& pp) {
            pp.ambientExposure = environment.ambientExposure;
            pp.giIntensity     = environment.giIntensity;
            pp.skyZenith       = JPH::Vec4(environment.skyZenith[0], environment.skyZenith[1], environment.skyZenith[2], 1.0f);
            pp.skyHorizon      = JPH::Vec4(environment.skyHorizon[0], environment.skyHorizon[1], environment.skyHorizon[2], 1.0f);
            pp.skyGround       = JPH::Vec4(environment.skyGround[0], environment.skyGround[1], environment.skyGround[2], 1.0f);
        });
    }

    // --- entities -----------------------------------------------------------
    for (const SceneEntity& entity: description.entities) {
        CreativeWorksFactory::SpawnParams params = MakeSpawnParams(entity);

        if (NeedsMaterial(entity.material)) {
            auto material = BuildMaterial(engine.GetRenderContext(), entity.material);
            if (!material) {
                ZHLN::Log("[Scene] entity '{}': material creation failed", entity.name);
                return std::unexpected(SceneError::MaterialCreationFailed);
            }
            params.materialOverride = *material;
        }

        switch (entity.shape) {
            case ShapeKind::Box: {
                const Entity created = CreativeWorksFactory::CreateBox(engine, ToVec3(entity.halfExtents), params);
                NameEntity(registry, created, entity.name);
                instance.entities.push_back(created);
                break;
            }
            case ShapeKind::Plane: {
                const Entity created = CreativeWorksFactory::CreatePlane(engine, entity.extent, ToVec4(entity.material.baseColor), params);
                NameEntity(registry, created, entity.name);
                instance.entities.push_back(created);
                break;
            }
            case ShapeKind::Prefab: {
                // The prefab decides how many entities it is worth; the buffer
                // is sized for the parts an authored prop realistically has and
                // truncation is reported rather than hidden.
                std::array<Entity, 256> parts {};
                const uint32_t          count =
                    CreativeWorksFactory::InstantiatePrefab(engine, entity.source, params, parts.data(), static_cast<uint32_t>(parts.size()));
                if (count == 0) {
                    ZHLN::Log("[Scene] entity '{}': prefab '{}' produced nothing", entity.name, entity.source);
                    return std::unexpected(SceneError::PrefabNotFound);
                }
                if (count > parts.size()) {
                    ZHLN::Log(
                        "[Scene] entity '{}': prefab '{}' has {} parts, only the first {} were recorded", entity.name, entity.source, count, parts.size()
                    );
                }

                const uint32_t recorded = std::min(count, static_cast<uint32_t>(parts.size()));
                NameEntity(registry, parts[0], entity.name);
                for (uint32_t i = 0; i < recorded; ++i) {
                    instance.entities.push_back(parts[i]);
                }
                break;
            }
        }
    }

    // --- lights -------------------------------------------------------------
    for (const SceneLight& light: description.lights) {
        const auto type = ZHLN::Reflect::StringToEnum<LightType>(light.type);
        if (!type) {
            ZHLN::Log("[Scene] light '{}': '{}' is not a LightType", light.name, light.type);
            return std::unexpected(SceneError::UnknownLightType);
        }

        const JPH::Vec3  position = ToVec3(light.position);
        const JPH::Mat44 world    = Math::CreateTransform(position, JPH::Quat::sIdentity(), JPH::Vec3::sReplicate(1.0f));

        const Entity created = registry.Create(
            Components::NameComponent {.name = String64(light.name)},
            Components::TransformComponent {.position = position, .rotation = JPH::Quat::sIdentity(), .scale = JPH::Vec3::sReplicate(1.0f)},
            Components::WorldTransformComponent {.world = world, .previous = world},
            Components::LightComponent {
                .type        = *type,
                .color       = ToVec3(light.color),
                .intensity   = light.intensity,
                .radius      = light.radius,
                .direction   = ToVec3(light.direction),
                .range       = light.range,
                .shadowLayer = light.shadowLayer
            }
        );

        instance.lights.push_back(created);
    }

    ZHLN::Log(
        "[Scene] '{}' instantiated: {} entities, {} lights", description.name.empty() ? std::string {"untitled"} : description.name,
        instance.entities.size(), instance.lights.size()
    );
    return instance;
}

auto InstantiateFromTOML(Engine& engine, std::string_view tomlText) -> std::expected<Instance, Error> {
    auto description = ReflectTOML::TryParse<Scene>(tomlText);
    if (!description) {
        return std::unexpected(description.error());
    }
    return Instantiate(engine, *description);
}

} // namespace ZHLN::Scene
