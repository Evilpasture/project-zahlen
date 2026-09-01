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

/// The only conversion the schema needs a helper for. JPH::Vec3 constructs
/// from a Float3 and JPH::Vec4 loads a Float4, but RVec3 is DVec3 in a
/// JPH_DOUBLE_PRECISION build (which this one is) and Vec3 in every other,
/// and only the widen-through-Vec3 spelling compiles in both.
[[nodiscard]] auto ToRVec3(const JPH::Float3& v) noexcept -> JPH::RVec3 {
    return JPH::RVec3 {JPH::Vec3 {v}};
}

/// Builds the SpawnParams shared by every shape: placement, body kind and the
/// emissive-light opt-in.
[[nodiscard]] auto MakeSpawnParams(const SceneEntity& entity) -> CreativeWorksFactory::SpawnParams {
    return CreativeWorksFactory::SpawnParams {
        .position        = ToRVec3(entity.transform.position),
        .rotation        = Math::EulerDegreesToQuat(JPH::Vec3 {entity.transform.rotation}),
        .scale           = JPH::Vec3 {entity.transform.scale},
        .createPhysics   = entity.body != BodyKind::None,
        // SpawnParams defaults this to true, and a description that asked for
        // BodyKind::Dynamic must not silently get a body that cannot move.
        .isStaticPhysics = entity.body != BodyKind::Dynamic,

        .emissiveVirtualLights = entity.material.emissiveVirtualLights,

        .roughness = entity.material.roughness,
        .metallic  = entity.material.metallic,
        .color     = JPH::Vec4::sLoadFloat4(&entity.material.baseColor)
    };
}

/// Emissive is the reason a scene entity needs a real material rather than the
/// colour/roughness shorthand: the factory's built-in material has no emissive
/// factor to set.
[[nodiscard]] auto NeedsMaterial(const SceneMaterial& material) noexcept -> bool {
    return material.emissive.x > 0.0f || material.emissive.y > 0.0f || material.emissive.z > 0.0f;
}

[[nodiscard]] auto BuildMaterial(RenderContext& ctx, const SceneMaterial& material) -> std::expected<ZHLN::Material, Error> {
    return CreativeWorksFactory::CreateMaterial(
        ctx, CreativeWorksFactory::MaterialDesc {
                 .metallic  = material.metallic,
                 .roughness = material.roughness,
                 .baseColor = {material.baseColor.x, material.baseColor.y, material.baseColor.z, material.baseColor.w},
                 .emissive  = {material.emissive.x, material.emissive.y, material.emissive.z, 1.0f}
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
    camera.position = JPH::Vec3 {description.camera.position};
    camera.yaw      = description.camera.yaw;
    camera.pitch    = description.camera.pitch;
    camera.fov      = description.camera.fov;

    // --- environment --------------------------------------------------------
    const SceneEnvironment& environment = description.environment;
    for (const Entity settings: registry.GetEntitiesWith<Components::GlobalSettingsTagComponent>()) {
        registry.Patch<Components::PostProcessSettingsComponent>(settings, [&](auto& pp) {
            pp.ambientExposure = environment.ambientExposure;
            pp.giIntensity     = environment.giIntensity;
            pp.enableSSR       = environment.enableSSR ? 1 : 0;
            pp.enableRTR       = environment.enableRTR ? 1 : 0;
            pp.skyZenith       = JPH::Vec4(JPH::Vec3 {environment.skyZenith}, 1.0f);
            pp.skyHorizon      = JPH::Vec4(JPH::Vec3 {environment.skyHorizon}, 1.0f);
            pp.skyGround       = JPH::Vec4(JPH::Vec3 {environment.skyGround}, 1.0f);
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
                const Entity created = CreativeWorksFactory::CreateBox(engine, JPH::Vec3 {entity.halfExtents}, params);
                NameEntity(registry, created, entity.name);
                instance.entities.push_back(created);
                break;
            }
            case ShapeKind::Plane: {
                const Entity created = CreativeWorksFactory::CreatePlane(engine, entity.extent, JPH::Vec4::sLoadFloat4(&entity.material.baseColor), params);
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

        const JPH::Vec3  position = JPH::Vec3 {light.position};
        const JPH::Quat  rotation = Math::EulerDegreesToQuat(JPH::Vec3 {light.rotation});
        const JPH::Mat44 world    = Math::CreateTransform(position, rotation, JPH::Vec3::sReplicate(1.0f));

        // A direction is a direction: a document writing [0.4, 1.0, 0.3] means
        // the bearing, and an unnormalized vector reaches the shader as an
        // intensity multiplier nobody asked for.
        const JPH::Vec3 rawDirection = JPH::Vec3 {light.direction};
        const JPH::Vec3 direction    = rawDirection.LengthSq() > 1e-8f ? rawDirection.Normalized() : rawDirection;

        const Entity created = registry.Create(
            Components::NameComponent {.name = String64(light.name)},
            Components::TransformComponent {.position = position, .rotation = rotation, .scale = JPH::Vec3::sReplicate(1.0f)},
            Components::WorldTransformComponent {.world = world, .previous = world},
            Components::LightComponent {
                .type        = *type,
                .color       = JPH::Vec3 {light.color},
                .intensity   = light.intensity,
                .radius      = light.radius,
                .direction   = direction,
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
